#include "../../include/query/query_engine.h"
#include "../../include/query/selective_decompressor.h"
#include "../../include/field_dictionary_manager.h"
#include "../../include/timestamp_dictionary.h"
#include "../../include/logtype_dictionary.h"
#include "../../include/loudsTotrie.h"
#include "../../include/reconstruct.h"
#include "../test/test_config_utils.h"
#include <iostream>
#include <optional>
#include <variant>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <future>

#ifdef _MSC_VER
#include <time.h>
#else
#include <time.h>
#include <sys/time.h>
#endif

namespace json2 {
namespace query {

// Implementation of executeExactMatchQuery
RecordQueryResult QueryEngine::executeExactMatchQuery(
    const std::string& field_name,
    const std::string& exact_value,
    FieldType expected_type,
    const GranularCompressedData& granular_data) {
    
    RecordQueryResult result;
    result.count = 0;
    result.chunks_accessed = 0;
    result.decompression_ratio = 0.0;
    result.is_complete = false;
    
    try {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Step 1: Pre-decompress metadata to avoid redundant decompression
        const compression::TypeAwareCompressionConfig& config = getTestConfig().type_aware_config;
        
        // Pre-decompress metadata once
        std::vector<FieldKey> field_order;
        if (!granular_data.metadata.empty()) {
            std::vector<uint8_t> metadata_raw = decompressWithConfig(
                granular_data.metadata, compression::FieldType::STRING, config);
            if (!metadata_raw.empty()) {
                field_order = Compressor::deserializeMetadata(metadata_raw);
            }
        }
        
        // Step 2: Perform field existence and type checking using pre-decompressed metadata
        auto field_result = this->checkFieldExistenceAndType(field_name, expected_type, granular_data, &field_order);
        
        if (!field_result.exists) {
            result.error_message = "Field '" + field_name + "' does not exist in the data";
            return result;
        }
        
        // Use the field index and field key from the check result to avoid redundant field lookup
        int field_index = field_result.field_index;
        FieldKey target_field_key = field_result.field_key;
        
        if (field_index == -1) {
            result.error_message = "Field '" + field_name + "' not found in the data";
            return result;
        }
        
        // Create dictionary manager and decompress dictionaries as needed for reconstruction
        std::unique_ptr<FieldDictionaryManager> dict_manager = std::make_unique<FieldDictionaryManager>();
        bool dict_preloaded = false;
        
        // Only pre-decompress dictionary for dictionary-encoded field types
        switch (expected_type) {
            case FieldType::STRING:
                if (!granular_data.string_dict.empty()) {
                    std::vector<uint8_t> string_dict_raw = decompressWithConfig(
                        granular_data.string_dict, compression::FieldType::STRING, config);
                    if (!string_dict_raw.empty()) {
                        Compressor::deserializeStringDictionary(string_dict_raw, *dict_manager);
                        dict_preloaded = true;
                    }
                }
                break;
            case FieldType::TIMESTAMP:
                if (!granular_data.timestamp_dict.empty()) {
                    std::vector<uint8_t> timestamp_dict_raw = decompressWithConfig(
                        granular_data.timestamp_dict, compression::FieldType::TIMESTAMP, config);
                    if (!timestamp_dict_raw.empty()) {
                        Compressor::deserializeTimestampDictionary(timestamp_dict_raw, *dict_manager);
                        dict_preloaded = true;
                    }
                }
                break;
            case FieldType::LOGTYPE:
                if (!granular_data.logtype_dict.empty()) {
                    std::vector<uint8_t> logtype_dict_raw = decompressWithConfig(
                        granular_data.logtype_dict, compression::FieldType::LOGTYPE, config);
                    if (!logtype_dict_raw.empty()) {
                        Compressor::deserializeLogTypeDictionary(logtype_dict_raw, *dict_manager);
                        dict_preloaded = true;
                    }
                }
                break;
            default:
                // For non-dictionary types (INT64, DOUBLE, BOOL), no dictionary needed
                break;
        }
        
        // Step 3: Perform layer value filtering
        // Pass the pre-loaded dict_manager to avoid redundant dictionary decompression
        auto filter_result = this->filterLayerValues(field_name, expected_type, exact_value, "=", granular_data, 
                                                          dict_preloaded ? dict_manager.get() : nullptr);

        if (!filter_result.match_found) {
            // No matches found, return empty result
            result.is_complete = true;
            auto end_time = std::chrono::high_resolution_clock::now();
            result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
            return result;
        }
        
        // Step 4: Decompress remaining content needed for record reconstruction
        std::unique_ptr<LOUDSTrie> louds_trie = nullptr;
        
        try {
            // Decompress directly to LOUDSTrie
            louds_trie = std::make_unique<LOUDSTrie>();
            
            // Use the already decompressed field_order
            if (!field_order.empty()) {
                louds_trie->setFieldOrder(field_order);
            }
            
            // Load trie structure
            if (!granular_data.trie_bitmap.empty()) {
                std::vector<uint8_t> trie_raw = decompressWithConfig(
                    granular_data.trie_bitmap, compression::FieldType::BOOL, config);
                if (!trie_raw.empty()) {
                    std::istringstream trie_stream(std::string(trie_raw.begin(), trie_raw.end()));
                    louds_trie->deserializeBitmap(trie_stream);
                }
            }
            
            // Load all layers (some may have been partially loaded during filtering)
            if (granular_data.use_layer_separation) {
                // Load layer sizes if available
                std::vector<uint32_t> layer_sizes;
                if (!granular_data.layer_sizes.empty()) {
                    std::vector<uint8_t> layer_sizes_raw = decompressWithConfig(
                        granular_data.layer_sizes, compression::FieldType::STRING, config);
                    if (layer_sizes_raw.size() >= sizeof(uint32_t)) {
                        size_t layer_count = layer_sizes_raw.size() / sizeof(uint32_t);
                        layer_sizes.resize(layer_count);
                        std::memcpy(layer_sizes.data(), layer_sizes_raw.data(), layer_count * sizeof(uint32_t));
                    }
                }
                
                // Load each layer
                for (size_t i = 0; i < granular_data.layer_data_by_level.size(); ++i) {
                    // Determine field type for this layer
                    compression::FieldType field_type = compression::FieldType::STRING; // Default
                    if (i < field_order.size()) {
                        // Map json2::FieldType to compression::FieldType
                        switch (field_order[i].type) {
                            case FieldType::INT64:
                                field_type = compression::FieldType::INT64;
                                break;
                            case FieldType::DOUBLE:
                                field_type = compression::FieldType::DOUBLE;
                                break;
                            case FieldType::BOOL:
                                field_type = compression::FieldType::BOOL;
                                break;
                            case FieldType::STRING:
                                field_type = compression::FieldType::STRING;
                                break;
                            case FieldType::TIMESTAMP:
                                field_type = compression::FieldType::TIMESTAMP;
                                break;
                            case FieldType::LOGTYPE:
                                field_type = compression::FieldType::LOGTYPE;
                                break;
                            case FieldType::ARRAY:
                                field_type = compression::FieldType::ARRAY;
                                break;
                            default:
                                field_type = compression::FieldType::STRING;
                                break;
                        }
                    }
                    
                    std::vector<uint8_t> layer_data = decompressWithConfig(
                        granular_data.layer_data_by_level[i], field_type, config);
                    if (!layer_data.empty()) {
                        std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()));
                        louds_trie->getLayeredStorage().deserializeLayer(i, layer_stream);
                    }
                }
            }
        } catch (const std::exception& e) {
            result.error_message = "Failed to decompress granular data for exact match: " + std::string(e.what());
            return result;
        }
        
        if (!louds_trie) {
            result.error_message = "Failed to decompress granular data for exact match";
            return result;
        }
        
        // Step 5: Get field order and use the already found target field
        const auto& final_field_order = louds_trie->getFieldOrder();
        result.chunks_accessed = 1; // We're accessing one chunk
        
        // Step 6: Process all matching indices to reconstruct their paths
        // Now decompress the remaining dictionaries needed for reconstruction
        // If we didn't pre-load a dictionary or need different dictionaries, create a new manager
        if (!dict_manager) {
            dict_manager = std::make_unique<FieldDictionaryManager>();
        }
        
        // Decompress dictionaries based on what's actually needed for the fields in the data
        bool need_string_dict = false;
        bool need_timestamp_dict = false;
        bool need_logtype_dict = false;
        
        // Check which dictionaries are needed based on field types in the data
        for (const auto& field_key : final_field_order) {
            switch (field_key.type) {
                case FieldType::STRING:
                    need_string_dict = true;
                    break;
                case FieldType::TIMESTAMP:
                    need_timestamp_dict = true;
                    break;
                case FieldType::LOGTYPE:
                    need_logtype_dict = true;
                    break;
                default:
                    break;
            }
        }
        
        // Decompress only the dictionaries that are actually needed and weren't already pre-loaded
        if (need_string_dict && !granular_data.string_dict.empty()) {
            // Check if we already have string dictionary loaded
            bool already_loaded = false;
            if (expected_type == FieldType::STRING && dict_preloaded) {
                try {
                    // Try to access the variable dictionary to see if it's loaded
                    const auto& var_dict = dict_manager->variableDict();
                    already_loaded = true;
                } catch (...) {
                    already_loaded = false;
                }
            }
            
            if (!already_loaded) {
                std::vector<uint8_t> string_dict_raw = decompressWithConfig(
                    granular_data.string_dict, compression::FieldType::STRING, config);
                if (!string_dict_raw.empty()) {
                    Compressor::deserializeStringDictionary(string_dict_raw, *dict_manager);
                }
            }
        }
        
        if (need_timestamp_dict && !granular_data.timestamp_dict.empty()) {
            // Check if we already have timestamp dictionary loaded
            bool already_loaded = false;
            if (expected_type == FieldType::TIMESTAMP && dict_preloaded) {
                try {
                    // Try to access the timestamp dictionary to see if it's loaded
                    const auto& timestamp_dict = dict_manager->timestampDict();
                    already_loaded = true;
                } catch (...) {
                    already_loaded = false;
                }
            }
            
            if (!already_loaded) {
                std::vector<uint8_t> timestamp_dict_raw = decompressWithConfig(
                    granular_data.timestamp_dict, compression::FieldType::TIMESTAMP, config);
                if (!timestamp_dict_raw.empty()) {
                    Compressor::deserializeTimestampDictionary(timestamp_dict_raw, *dict_manager);
                }
            }
        }
        
        if (need_logtype_dict && !granular_data.logtype_dict.empty()) {
            // Check if we already have logtype dictionary loaded
            bool already_loaded = false;
            if (expected_type == FieldType::LOGTYPE && dict_preloaded) {
                try {
                    // Try to access the logtype dictionary to see if it's loaded
                    const auto& logtype_dict = dict_manager->logtypeDict();
                    already_loaded = true;
                } catch (...) {
                    already_loaded = false;
                }
            }
            
            if (!already_loaded) {
                std::vector<uint8_t> logtype_dict_raw = decompressWithConfig(
                    granular_data.logtype_dict, compression::FieldType::LOGTYPE, config);
                if (!logtype_dict_raw.empty()) {
                    Compressor::deserializeLogTypeDictionary(logtype_dict_raw, *dict_manager);
                }
            }
        }
        
        // Process all matching indices to reconstruct their paths
        for (size_t matched_idx : filter_result.matched_indices) {
            // Convert layer index and node index to BFS index
            size_t bfs_idx = louds_trie->getLayeredStorage().layerIndexToBFS(field_index, matched_idx);
            
            // Reconstruct and decode all paths that go through this node
            // Use the pre-decompressed dictionary manager if available
            auto decoded_paths = reconstructAndDecodePathsFromIntermediateNode(
                *louds_trie, bfs_idx, *dict_manager);
            
            // Add matching records to result
            for (const auto& path : decoded_paths) {
                // Convert path to JSON-like string representation using the helper function
                std::string record = this->pathToJSONString(path, final_field_order);
                result.records.push_back(record);
                result.count++;
            }
        }
        
        // Mark the query as complete
        result.is_complete = true;
        auto end_time = std::chrono::high_resolution_clock::now();
        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing exact match query: " + std::string(e.what());
    }
    
    return result;
}

RecordQueryResult QueryEngine::executeExactMatchQueryMultiBlock(
    const std::string& field_name,
    const std::string& exact_value,
    FieldType expected_type,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    RecordQueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.decompression_ratio = 0.0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // Process each chunk
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        // Extract granular data from chunk
        GranularCompressedData granular_data = extractGranularDataFromChunk(chunk, i);
        
        // Execute query on this chunk
        RecordQueryResult chunk_result = executeExactMatchQuery(field_name, exact_value, expected_type, granular_data);
        
        // Merge results
        mergeRecordQueryResults(final_result, chunk_result);
    }
    
    return final_result;
}

RecordQueryResult QueryEngine::executeExactMatchQueryMultiBlockParallel(
    const std::string& field_name,
    const std::string& exact_value,
    FieldType expected_type,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    RecordQueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.decompression_ratio = 0.0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeExactMatchQueryMultiBlock(field_name, exact_value, expected_type, chunks);
    }
    
    // Limit thread count to the number of chunks or hardware concurrency
    thread_count = std::min(thread_count, std::max(static_cast<size_t>(1), chunks.size()));
    thread_count = std::min(thread_count, static_cast<size_t>(std::thread::hardware_concurrency()));
    
    // Split chunk indices into thread_count groups
    std::vector<std::vector<size_t>> chunk_groups(thread_count);
    for (size_t i = 0; i < chunks.size(); ++i) {
        chunk_groups[i % thread_count].push_back(i);
    }
    
    // Launch threads to process each group
    std::vector<std::future<RecordQueryResult>> futures;
    for (size_t group_idx = 0; group_idx < thread_count; ++group_idx) {
        if (chunk_groups[group_idx].empty()) continue;
        
        // Launch async task with chunk indices
        futures.push_back(std::async(std::launch::async, [this, &field_name, &exact_value, expected_type, &chunks, group_indices = chunk_groups[group_idx]]() {
            RecordQueryResult group_result;
            group_result.count = 0;
            group_result.chunks_accessed = 0;
            group_result.decompression_ratio = 0.0;
            group_result.is_complete = true;
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                RecordQueryResult chunk_result = executeExactMatchQuery(field_name, exact_value, expected_type, granular_data);
                
                // Merge results
                mergeRecordQueryResults(group_result, chunk_result);
            }
            
            return group_result;
        }));
    }
    
    // Collect results from all threads
    for (auto& future : futures) {
        try {
            RecordQueryResult group_result = future.get();
            mergeRecordQueryResults(final_result, group_result);
        } catch (const std::exception& e) {
            // Handle exceptions from threads
            if (final_result.error_message.empty()) {
                final_result.error_message = "Error in parallel processing: " + std::string(e.what());
            }
        }
    }
    
    return final_result;
}

} // namespace query
} // namespace json2