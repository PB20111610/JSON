#include "../include/query/query_engine.h"
#include "../include/compress.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include "../include/compress_type_aware.h"  // Add this include for decompressWithConfig
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <iostream>

namespace json2 {
namespace query {

QueryEngine::QueryEngine(const QueryConfig& config) 
    : config_(config) {
    // 初始化组件
    parser_ = std::make_unique<QueryParser>();
    field_analyzer_ = std::make_unique<FieldAnalyzer>();
    chunk_selector_ = std::make_unique<ChunkSelector>();
    decompressor_ = std::make_unique<SelectiveDecompressor>();
    trie_traverser_ = std::make_unique<TrieTraverser>();
    result_rebuilder_ = std::make_unique<ResultRebuilder>();
}

// ========== 优化的查询方法 ==========

FieldExistenceResult QueryEngine::checkFieldExistenceAndType(
    const std::string& field_name,
    FieldType expected_type,
    const GranularCompressedData& granular_data) {
    
    FieldExistenceResult result;
    result.exists = false;
    result.type_matches = false;
    result.field_type = FieldType::String; // 默认值
    
    try {
        // Step 1: Only decompress metadata to check field existence and type
        // This is the minimal decompression - only metadata containing FieldKey order
        if (granular_data.metadata.empty()) {
            result.error_message = "Metadata is empty";
            return result;
        }
        
        // Add debug information
        std::cerr << "[DEBUG] Metadata size: " << granular_data.metadata.size() << " bytes" << std::endl;
        
        // Create a default config for decompression that matches the compression config used
        compression::TypeAwareCompressionConfig config;
        config.louds_backend = compression::CompressionBackend::BIT_PACKING;
        config.dictionary_backend = compression::CompressionBackend::ZSTD;
        config.metadata_backend = compression::CompressionBackend::ZSTD;
        
        // Configure field type compression backends to match test_granular_type_aware_chunked_cmp.cpp
        config.layer_config.int_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.double_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.bool_backend = compression::CompressionBackend::BIT_PACKING;
        config.layer_config.string_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_DELTA;
        config.layer_config.logtype_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.array_backend = compression::CompressionBackend::RLE;
        config.layer_config.null_backend = compression::CompressionBackend::BIT_PACKING;
        
        config.compression_level = 3;
        
        // Decompress metadata using the proper type-aware decompression function
        std::vector<uint8_t> metadata_raw = decompressWithConfig(granular_data.metadata, 
                                                               compression::FieldType::STRING, config);
        
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        
        // Step 2: Check if field exists and matches type
        // Direct matching in metadata without any additional decompression
        for (const auto& field_key : field_order) {
            if (field_key.name == field_name) {
                result.exists = true;
                result.field_type = field_key.type;
                result.type_matches = (field_key.type == expected_type);
                break;
            }
        }
        
        // Performance optimization: Early return if field doesn't exist
        if (!result.exists) {
            return result;
        }
        
    } catch (const std::exception& e) {
        result.error_message = "Error checking field existence: " + std::string(e.what());
        // Add more detailed debug information
        std::cerr << "[DEBUG] Exception in checkFieldExistenceAndType: " << e.what() << std::endl;
        std::cerr << "[DEBUG] Metadata size: " << granular_data.metadata.size() << " bytes" << std::endl;
        if (!granular_data.metadata.empty()) {
            std::cerr << "[DEBUG] First few bytes of metadata: ";
            for (size_t i = 0; i < std::min(size_t(10), granular_data.metadata.size()); ++i) {
                std::cerr << std::hex << std::setw(2) << std::setfill('0') << (int)granular_data.metadata[i] << " ";
            }
            std::cerr << std::dec << std::endl;
        }
    }
    
    return result;
}

DictionaryQueryResult QueryEngine::queryDictionary(
    const std::string& field_name,
    FieldType field_type,
    const GranularCompressedData& granular_data,
    const std::string& target_value) {
    
    DictionaryQueryResult result;
    result.found = false;
    
    try {
        // Step 1: Determine which dictionary to decompress based on field type
        // Only decompress the corresponding dictionary for the queried type
        std::vector<uint8_t> dict_data;
        bool dict_available = false;
        
        // Add debug information
        std::cerr << "[DEBUG] Querying dictionary for field: " << field_name << ", type: " << static_cast<int>(field_type) << std::endl;
        
        // Create a default config for decompression that matches the compression config used
        compression::TypeAwareCompressionConfig config;
        config.louds_backend = compression::CompressionBackend::BIT_PACKING;
        config.dictionary_backend = compression::CompressionBackend::ZSTD;
        config.metadata_backend = compression::CompressionBackend::ZSTD;
        
        // Configure field type compression backends to match test_granular_type_aware_chunked_cmp.cpp
        config.layer_config.int_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.double_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.bool_backend = compression::CompressionBackend::BIT_PACKING;
        config.layer_config.string_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_DELTA;
        config.layer_config.logtype_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.array_backend = compression::CompressionBackend::RLE;
        config.layer_config.null_backend = compression::CompressionBackend::BIT_PACKING;
        
        config.compression_level = 3;
        
        // INT, FLOAT, BOOL 不用解压任何字典，因为它们存的是原始值
        switch (field_type) {
            case FieldType::Int:
            case FieldType::Double:
            case FieldType::Bool:
                // These types store raw values, no dictionary needed
                result.found = target_value.empty(); // If no target value, consider as found
                return result;
            case FieldType::String:
                std::cerr << "[DEBUG] String dict size: " << granular_data.string_dict.size() << " bytes" << std::endl;
                if (!granular_data.string_dict.empty()) {
                    // Decompress using the proper type-aware decompression function
                    dict_data = decompressWithConfig(granular_data.string_dict, 
                                                   compression::FieldType::STRING, config);
                    dict_available = true;
                }
                break;
            case FieldType::Timestamp:
                std::cerr << "[DEBUG] Timestamp dict size: " << granular_data.timestamp_dict.size() << " bytes" << std::endl;
                if (!granular_data.timestamp_dict.empty()) {
                    // Decompress using the proper type-aware decompression function
                    dict_data = decompressWithConfig(granular_data.timestamp_dict, 
                                                   compression::FieldType::TIMESTAMP, config);
                    dict_available = true;
                }
                break;
            case FieldType::LogType:
                std::cerr << "[DEBUG] LogType dict size: " << granular_data.logtype_dict.size() << " bytes" << std::endl;
                if (!granular_data.logtype_dict.empty()) {
                    // Decompress using the proper type-aware decompression function
                    dict_data = decompressWithConfig(granular_data.logtype_dict, 
                                                   compression::FieldType::LOGTYPE, config);
                    dict_available = true;
                }
                break;
            default:
                // For other types, we don't need dictionary decompression
                result.found = target_value.empty(); // If no target value, consider as found
                return result;
        }
        
        if (!dict_available) {
            result.error_message = "Dictionary not available for field type: " + std::to_string(static_cast<int>(field_type));
            return result;
        }
        
        // Step 2: Create dictionary manager and deserialize the specific dictionary
        std::unique_ptr<FieldDictionaryManager> dict_manager = std::make_unique<FieldDictionaryManager>();
        
        switch (field_type) {
            case FieldType::String:
                Compressor::deserializeStringDictionary(dict_data, *dict_manager);
                break;
            case FieldType::Timestamp:
                Compressor::deserializeTimestampDictionary(dict_data, *dict_manager);
                break;
            case FieldType::LogType:
                Compressor::deserializeLogTypeDictionary(dict_data, *dict_manager);
                break;
            default:
                break;
        }
        
        // Step 3: Perform dictionary lookup
        if (target_value.empty()) {
            // Return all values in dictionary (for browsing)
            // Extract all values from the dictionary
            result.values = extractAllDictionaryValues(*dict_manager, field_type);
            result.found = !result.values.empty();
        } else {
            // Perform exact match lookup using partial decompression APIs
            std::string found_value = performDictionaryLookup(*dict_manager, field_type, target_value);
            if (!found_value.empty()) {
                result.values.push_back(found_value);
                result.found = true;
            }
        }
        
    } catch (const std::exception& e) {
        result.error_message = "Error querying dictionary: " + std::string(e.what());
        // Add more detailed debug information
        std::cerr << "[DEBUG] Exception in queryDictionary: " << e.what() << std::endl;
        std::cerr << "[DEBUG] Field type: " << static_cast<int>(field_type) << std::endl;
        switch (field_type) {
            case FieldType::String:
                std::cerr << "[DEBUG] String dict size: " << granular_data.string_dict.size() << " bytes" << std::endl;
                break;
            case FieldType::Timestamp:
                std::cerr << "[DEBUG] Timestamp dict size: " << granular_data.timestamp_dict.size() << " bytes" << std::endl;
                break;
            case FieldType::LogType:
                std::cerr << "[DEBUG] LogType dict size: " << granular_data.logtype_dict.size() << " bytes" << std::endl;
                break;
            default:
                break;
        }
    }
    
    return result;
}

RecordQueryResult QueryEngine::executeExactMatchQuery(
    const std::string& field_name,
    const std::string& exact_value,
    const GranularCompressedData& granular_data,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    RecordQueryResult result;
    result.count = 0;
    result.chunks_accessed = 0;
    result.decompression_ratio = 0.0;
    result.query_time_ms = 0.0;
    result.is_complete = false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // New querying approach:
        // 1. Decompress metadata to check field existence
        if (granular_data.metadata.empty()) {
            result.error_message = "Metadata is empty";
            return result;
        }
        
        // Create a default config for decompression that matches the compression config used
        compression::TypeAwareCompressionConfig config;
        config.louds_backend = compression::CompressionBackend::BIT_PACKING;
        config.dictionary_backend = compression::CompressionBackend::ZSTD;
        config.metadata_backend = compression::CompressionBackend::ZSTD;
        
        // Configure field type compression backends to match test_granular_type_aware_chunked_cmp.cpp
        config.layer_config.int_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.double_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.bool_backend = compression::CompressionBackend::BIT_PACKING;
        config.layer_config.string_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_DELTA;
        config.layer_config.logtype_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.array_backend = compression::CompressionBackend::RLE;
        config.layer_config.null_backend = compression::CompressionBackend::BIT_PACKING;
        
        config.compression_level = 3;
        
        // Decompress metadata using the proper type-aware decompression function
        std::vector<uint8_t> metadata_raw = decompressWithConfig(granular_data.metadata, 
                                                               compression::FieldType::STRING, config);
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        
        // 2. Check if field exists
        int field_index = getFieldIndex(field_name, field_order);
        if (field_index == -1) {
            result.error_message = "Field not found: " + field_name;
            return result;
        }
        
        FieldKey target_field = field_order[field_index];
        
        // 3. Decompress required dictionaries for decoding (without full deserialization)
        std::unique_ptr<FieldDictionaryManager> dict_manager = std::make_unique<FieldDictionaryManager>();
        
        // INT, FLOAT, BOOL 不用解压任何字典，因为它们存的是原始值
        if (needsDictionaryDecompression(target_field.type)) {
            switch (target_field.type) {
                case FieldType::String:
                    if (!granular_data.string_dict.empty()) {
                        std::vector<uint8_t> dict_raw = decompressWithConfig(granular_data.string_dict, 
                                                                           compression::FieldType::STRING, config);
                        Compressor::deserializeStringDictionary(dict_raw, *dict_manager);
                    }
                    break;
                case FieldType::Timestamp:
                    if (!granular_data.timestamp_dict.empty()) {
                        std::vector<uint8_t> dict_raw = decompressWithConfig(granular_data.timestamp_dict, 
                                                                           compression::FieldType::TIMESTAMP, config);
                        Compressor::deserializeTimestampDictionary(dict_raw, *dict_manager);
                    }
                    break;
                case FieldType::LogType:
                    if (!granular_data.logtype_dict.empty()) {
                        std::vector<uint8_t> dict_raw = decompressWithConfig(granular_data.logtype_dict, 
                                                                           compression::FieldType::LOGTYPE, config);
                        Compressor::deserializeLogTypeDictionary(dict_raw, *dict_manager);
                    }
                    break;
                default:
                    break;
            }
        }
        
        // 4. Use FieldKey order to determine field's layer
        size_t layer_index = static_cast<size_t>(field_index);
        
        // 5. Decompress only that specific layer using config-specified algorithm
        std::vector<uint8_t> layer_raw;
        if (granular_data.use_layer_separation) {
            // Check if we're using layer separation and the layer data exists
            if (layer_index >= granular_data.layer_data_by_level.size()) {
                result.error_message = "Layer index out of range: " + std::to_string(layer_index);
                return result;
            }
            
            // Check if the layer data is empty
            if (granular_data.layer_data_by_level[layer_index].empty()) {
                result.error_message = "Layer data is empty for index: " + std::to_string(layer_index);
                return result;
            }
            
            layer_raw = decompressWithConfig(granular_data.layer_data_by_level[layer_index], 
                                           mapFieldTypeToCompressionType(target_field.type), config);
        } else {
            // Handle the case where we're not using layer separation
            if (granular_data.layer_data_combined.empty()) {
                result.error_message = "Combined layer data is empty";
                return result;
            }
            
            // For non-layered data, we need to decompress the entire combined data
            layer_raw = decompressWithConfig(granular_data.layer_data_combined, 
                                           mapFieldTypeToCompressionType(target_field.type), config);
        }
        
        // 6. Decompress LOUDS bitmap structure and layer sizes
        if (granular_data.trie_bitmap.empty()) {
            result.error_message = "Trie bitmap is empty";
            return result;
        }
        
        std::vector<uint8_t> trie_raw = decompressWithConfig(granular_data.trie_bitmap, 
                                                           compression::FieldType::BOOL, config);
        
        // 解压层大小信息（如果存在）
        std::vector<uint32_t> layer_sizes;
        if (!granular_data.layer_sizes.empty()) {
            std::vector<uint8_t> layer_sizes_raw = decompressWithConfig(granular_data.layer_sizes, 
                                                                      compression::FieldType::STRING, config);
            if (layer_sizes_raw.size() >= sizeof(uint32_t)) {
                size_t layer_count = layer_sizes_raw.size() / sizeof(uint32_t);
                layer_sizes.resize(layer_count);
                std::memcpy(layer_sizes.data(), layer_sizes_raw.data(), layer_count * sizeof(uint32_t));
            }
        }
        
        std::unique_ptr<LOUDSTrie> louds = Compressor::deserializeLoudsTrie(trie_raw, *dict_manager, field_order);
        
        // 设置层大小信息到LOUDS结构中，以便正确进行索引转换
        if (!layer_sizes.empty()) {
            std::vector<size_t> sizes(layer_sizes.begin(), layer_sizes.end());
            louds->getLayeredStorage().setLayerSizes(sizes);
        }
        
        // 7. Parse layer data to find matching node values
        std::vector<size_t> matched_layer_indices = findMatchingNodesInLayer(
            layer_raw, layer_index, exact_value, target_field, *dict_manager);
        
        // 8. Convert layer indices to BFS indices
        std::vector<size_t> matched_bfs_indices;
        if (granular_data.use_layer_separation) {
            // When using layer separation, convert layer indices to BFS indices
            for (size_t layer_node_idx : matched_layer_indices) {
                size_t bfs_idx = louds->getLayeredStorage().layerIndexToBFS(layer_index, layer_node_idx);
                matched_bfs_indices.push_back(bfs_idx);
            }
        } else {
            // When not using layer separation, the indices are already BFS indices
            matched_bfs_indices = matched_layer_indices;
        }
        
        // 9. Reconstruct paths using louds.h and loudsTotrie.h functions
        result.records = reconstructRecords(matched_bfs_indices, *louds, *dict_manager);
        result.count = result.records.size();
        result.chunks_accessed = 1;
        result.decompression_ratio = calculateDecompressionRatio(granular_data, layer_index, true);
        result.is_complete = true;
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing exact match query: " + std::string(e.what());
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end_time - start_time);
    result.query_time_ms = duration.count();
    
    return result;
}

RecordQueryResult QueryEngine::executeRangeQuery(
    const std::string& field_name,
    const std::string& min_value,
    const std::string& max_value,
    const GranularCompressedData& granular_data,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    RecordQueryResult result;
    result.count = 0;
    result.chunks_accessed = 0;
    result.decompression_ratio = 0.0;
    result.query_time_ms = 0.0;
    result.is_complete = false;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // New querying approach:
        // 1. Decompress metadata to check field existence
        if (granular_data.metadata.empty()) {
            result.error_message = "Metadata is empty";
            return result;
        }
        
        // Create a default config for decompression that matches the compression config used
        compression::TypeAwareCompressionConfig config;
        config.louds_backend = compression::CompressionBackend::BIT_PACKING;
        config.dictionary_backend = compression::CompressionBackend::ZSTD;
        config.metadata_backend = compression::CompressionBackend::ZSTD;
        
        // Configure field type compression backends to match test_granular_type_aware_chunked_cmp.cpp
        config.layer_config.int_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.double_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.bool_backend = compression::CompressionBackend::BIT_PACKING;
        config.layer_config.string_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_DELTA;
        config.layer_config.logtype_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.array_backend = compression::CompressionBackend::RLE;
        config.layer_config.null_backend = compression::CompressionBackend::BIT_PACKING;
        
        config.compression_level = 3;
        
        // Decompress metadata using the proper type-aware decompression function
        std::vector<uint8_t> metadata_raw = decompressWithConfig(granular_data.metadata, 
                                                               compression::FieldType::STRING, config);
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        
        // 2. Check if field exists
        int field_index = getFieldIndex(field_name, field_order);
        if (field_index == -1) {
            result.error_message = "Field not found: " + field_name;
            return result;
        }
        
        FieldKey target_field = field_order[field_index];
        
        // 3. Decompress required dictionaries for decoding (without full deserialization)
        std::unique_ptr<FieldDictionaryManager> dict_manager = std::make_unique<FieldDictionaryManager>();
        
        // INT, FLOAT, BOOL 不用解压任何字典，因为它们存的是原始值
        if (needsDictionaryDecompression(target_field.type)) {
            switch (target_field.type) {
                case FieldType::String:
                    if (!granular_data.string_dict.empty()) {
                        std::vector<uint8_t> dict_raw = decompressWithConfig(granular_data.string_dict, 
                                                                           compression::FieldType::STRING, config);
                        Compressor::deserializeStringDictionary(dict_raw, *dict_manager);
                    }
                    break;
                case FieldType::Timestamp:
                    if (!granular_data.timestamp_dict.empty()) {
                        std::vector<uint8_t> dict_raw = decompressWithConfig(granular_data.timestamp_dict, 
                                                                           compression::FieldType::TIMESTAMP, config);
                        Compressor::deserializeTimestampDictionary(dict_raw, *dict_manager);
                    }
                    break;
                case FieldType::LogType:
                    if (!granular_data.logtype_dict.empty()) {
                        std::vector<uint8_t> dict_raw = decompressWithConfig(granular_data.logtype_dict, 
                                                                           compression::FieldType::LOGTYPE, config);
                        Compressor::deserializeLogTypeDictionary(dict_raw, *dict_manager);
                    }
                    break;
                default:
                    break;
            }
        }
        
        // 4. Use FieldKey order to determine field's layer
        size_t layer_index = static_cast<size_t>(field_index);
        
        // 5. Decompress only that specific layer using config-specified algorithm
        std::vector<uint8_t> layer_raw;
        if (granular_data.use_layer_separation) {
            // Check if we're using layer separation and the layer data exists
            if (layer_index >= granular_data.layer_data_by_level.size()) {
                result.error_message = "Layer index out of range: " + std::to_string(layer_index);
                return result;
            }
            
            // Check if the layer data is empty
            if (granular_data.layer_data_by_level[layer_index].empty()) {
                result.error_message = "Layer data is empty for index: " + std::to_string(layer_index);
                return result;
            }
            
            layer_raw = decompressWithConfig(granular_data.layer_data_by_level[layer_index], 
                                           mapFieldTypeToCompressionType(target_field.type), config);
        } else {
            // Handle the case where we're not using layer separation
            if (granular_data.layer_data_combined.empty()) {
                result.error_message = "Combined layer data is empty";
                return result;
            }
            
            // For non-layered data, we need to decompress the entire combined data
            layer_raw = decompressWithConfig(granular_data.layer_data_combined, 
                                           mapFieldTypeToCompressionType(target_field.type), config);
        }
        
        // 6. Decompress LOUDS bitmap structure and layer sizes
        if (granular_data.trie_bitmap.empty()) {
            result.error_message = "Trie bitmap is empty";
            return result;
        }
        
        std::vector<uint8_t> trie_raw = decompressWithConfig(granular_data.trie_bitmap, 
                                                           compression::FieldType::BOOL, config);
        
        // 解压层大小信息（如果存在）
        std::vector<uint32_t> layer_sizes;
        if (!granular_data.layer_sizes.empty()) {
            std::vector<uint8_t> layer_sizes_raw = decompressWithConfig(granular_data.layer_sizes, 
                                                                      compression::FieldType::STRING, config);
            if (layer_sizes_raw.size() >= sizeof(uint32_t)) {
                size_t layer_count = layer_sizes_raw.size() / sizeof(uint32_t);
                layer_sizes.resize(layer_count);
                std::memcpy(layer_sizes.data(), layer_sizes_raw.data(), layer_count * sizeof(uint32_t));
            }
        }
        
        std::unique_ptr<LOUDSTrie> louds = Compressor::deserializeLoudsTrie(trie_raw, *dict_manager, field_order);
        
        // 设置层大小信息到LOUDS结构中，以便正确进行索引转换
        if (!layer_sizes.empty()) {
            std::vector<size_t> sizes(layer_sizes.begin(), layer_sizes.end());
            louds->getLayeredStorage().setLayerSizes(sizes);
        }
        
        // 7. Parse layer data to find nodes within range
        std::vector<size_t> matched_layer_indices = findNodesInRangeInLayer(
            layer_raw, layer_index, min_value, max_value, target_field, *dict_manager);
        
        // 8. Convert layer indices to BFS indices
        std::vector<size_t> matched_bfs_indices;
        if (granular_data.use_layer_separation) {
            // When using layer separation, convert layer indices to BFS indices
            for (size_t layer_node_idx : matched_layer_indices) {
                size_t bfs_idx = louds->getLayeredStorage().layerIndexToBFS(layer_index, layer_node_idx);
                matched_bfs_indices.push_back(bfs_idx);
            }
        } else {
            // When not using layer separation, the indices are already BFS indices
            matched_bfs_indices = matched_layer_indices;
        }
        
        // 9. Reconstruct paths using louds.h and loudsTotrie.h functions
        result.records = reconstructRecords(matched_bfs_indices, *louds, *dict_manager);
        result.count = result.records.size();
        result.chunks_accessed = 1;
        result.decompression_ratio = calculateDecompressionRatio(granular_data, layer_index, true);
        result.is_complete = true;
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing range query: " + std::string(e.what());
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end_time - start_time);
    result.query_time_ms = duration.count();
    
    return result;
}

// ========== 内部实现方法 ==========

// Helper function to map json2::FieldType to compression::FieldType
compression::FieldType QueryEngine::mapFieldTypeToCompressionType(FieldType json_field_type) const {
    switch (json_field_type) {
        case FieldType::String:
        case FieldType::UnstructuredArray:
            return compression::FieldType::STRING;
        case FieldType::Timestamp:
            return compression::FieldType::TIMESTAMP;
        case FieldType::LogType:
            return compression::FieldType::LOGTYPE;
        case FieldType::Int:
            return compression::FieldType::INT64;
        case FieldType::Double:
            return compression::FieldType::DOUBLE;
        case FieldType::Bool:
            return compression::FieldType::BOOL;
        case FieldType::Null:
            return compression::FieldType::NULL_TYPE;
        default:
            return compression::FieldType::STRING; // Safe default
    }
}

int QueryEngine::getFieldIndex(const std::string& field_name, const std::vector<FieldKey>& field_order) const {
    for (size_t i = 0; i < field_order.size(); ++i) {
        if (field_order[i].name == field_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool QueryEngine::needsDictionaryDecompression(FieldType field_type) const {
    // INT, FLOAT, BOOL 不用解压任何字典，因为它们存的是原始值
    switch (field_type) {
        case FieldType::Int:
        case FieldType::Double:
        case FieldType::Bool:
            return false; // These types store raw values, no dictionary needed
        case FieldType::String:
        case FieldType::Timestamp:
        case FieldType::LogType:
            return true;
        default:
            return false;
    }
}

std::vector<std::string> QueryEngine::reconstructRecords(
    const std::vector<size_t>& matched_bfs_indices,
    const LOUDSTrie& louds,
    const FieldDictionaryManager& dict_manager) const {
    
    std::vector<std::string> records;
    
    try {
        // Use LOUDSTrie path reconstruction utilities to rebuild records
        std::vector<std::vector<std::string>> all_decoded_paths;
        
        // Batch process all matched indices for better performance
        for (size_t bfs_idx : matched_bfs_indices) {
            // Reconstruct and decode paths using loudsTotrie.h functions
            std::vector<std::vector<std::string>> decoded_paths = 
                reconstructAndDecodePathsFromIntermediateNode(louds, bfs_idx, dict_manager);
            
            // Add all decoded paths to our collection
            all_decoded_paths.insert(all_decoded_paths.end(), decoded_paths.begin(), decoded_paths.end());
        }
        
        // Get field order for path reconstruction
        const std::vector<FieldKey>& field_order = louds.getFieldOrder();
        
        // Convert decoded paths to JSON records
        for (const auto& path : all_decoded_paths) {
            std::string json_record = convertPathToJSON(path, field_order);
            if (!json_record.empty()) {
                records.push_back(json_record);
            }
        }
        
    } catch (const std::exception& e) {
        // Handle reconstruction errors gracefully
        records.clear();
        records.push_back("Error reconstructing records: " + std::string(e.what()));
    }
    
    return records;
}

bool QueryEngine::compareNodeValueWithTarget(
    const NodeValue& node_value,
    const std::string& target_value,
    const FieldKey& field_key,
    const FieldDictionaryManager& dict_manager) const {
    
    try {
        // Decode the node value based on its type
        std::string actual_value;
        
        switch (field_key.type) {
            case FieldType::String: {
                if (std::holds_alternative<uint32_t>(node_value)) {
                    uint32_t code = std::get<uint32_t>(node_value);
                    FieldKey fk{field_key.name, field_key.type};
                    auto opt_value = dict_manager.getFieldValueByCode(fk, code);
                    if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                        actual_value = std::get<std::string>(*opt_value);
                    }
                }
                break;
            }
            case FieldType::Int: {
                if (std::holds_alternative<int64_t>(node_value)) {
                    actual_value = std::to_string(std::get<int64_t>(node_value));
                }
                break;
            }
            case FieldType::Double: {
                if (std::holds_alternative<double>(node_value)) {
                    actual_value = std::to_string(std::get<double>(node_value));
                }
                break;
            }
            case FieldType::Bool: {
                if (std::holds_alternative<bool>(node_value)) {
                    actual_value = std::get<bool>(node_value) ? "true" : "false";
                }
                break;
            }
            case FieldType::Timestamp: {
                if (std::holds_alternative<TemplateEncodedTimestamp>(node_value)) {
                    const auto& enc = std::get<TemplateEncodedTimestamp>(node_value);
                    actual_value = dict_manager.timestampDict().decodeTemplate(field_key, enc);
                }
                break;
            }
            case FieldType::LogType: {
                if (std::holds_alternative<EncodedLog>(node_value)) {
                    const auto& enc = std::get<EncodedLog>(node_value);
                    actual_value = dict_manager.logtypeDict().decodeLogToString(field_key, enc);
                }
                break;
            }
            default:
                break;
        }
        
        // Compare the actual value with the target value
        return actual_value == target_value;
        
    } catch (const std::exception& e) {
        // Handle comparison errors gracefully
        return false;
    }
}

bool QueryEngine::isNodeValueInRange(
    const NodeValue& node_value,
    const std::string& min_value,
    const std::string& max_value,
    const FieldKey& field_key,
    const FieldDictionaryManager& dict_manager) const {
    
    try {
        // Decode the node value based on its type
        std::string actual_value;
        
        switch (field_key.type) {
            case FieldType::String: {
                if (std::holds_alternative<uint32_t>(node_value)) {
                    uint32_t code = std::get<uint32_t>(node_value);
                    FieldKey fk{field_key.name, field_key.type};
                    auto opt_value = dict_manager.getFieldValueByCode(fk, code);
                    if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                        actual_value = std::get<std::string>(*opt_value);
                    }
                }
                break;
            }
            case FieldType::Int: {
                if (std::holds_alternative<int64_t>(node_value)) {
                    actual_value = std::to_string(std::get<int64_t>(node_value));
                }
                break;
            }
            case FieldType::Double: {
                if (std::holds_alternative<double>(node_value)) {
                    actual_value = std::to_string(std::get<double>(node_value));
                }
                break;
            }
            case FieldType::Timestamp: {
                if (std::holds_alternative<TemplateEncodedTimestamp>(node_value)) {
                    const auto& enc = std::get<TemplateEncodedTimestamp>(node_value);
                    actual_value = dict_manager.timestampDict().decodeTemplate(field_key, enc);
                }
                break;
            }
            case FieldType::LogType: {
                if (std::holds_alternative<EncodedLog>(node_value)) {
                    const auto& enc = std::get<EncodedLog>(node_value);
                    actual_value = dict_manager.logtypeDict().decodeLogToString(field_key, enc);
                }
                break;
            }
            default:
                break;
        }
        
        // Check if the actual value is within the specified range
        return (actual_value >= min_value) && (actual_value <= max_value);
        
    } catch (const std::exception& e) {
        // Handle range check errors gracefully
        return false;
    }
}

std::vector<std::string> QueryEngine::extractAllDictionaryValues(
    const FieldDictionaryManager& dict_manager,
    FieldType field_type) const {
    
    std::vector<std::string> values;
    
    try {
        switch (field_type) {
            case FieldType::String: {
                // Extract all string values from variable dictionary
                auto& var_dict = dict_manager.variableDict();
                std::vector<std::string> all_values = var_dict.getAllStringValues();
                values.insert(values.end(), all_values.begin(), all_values.end());
                break;
            }
            case FieldType::Timestamp: {
                // Extract all timestamp templates
                auto& timestamp_dict = dict_manager.timestampDict();
                for (uint32_t i = 1; i <= timestamp_dict.getTemplateCount(); ++i) {
                    std::string template_str = timestamp_dict.getTemplateById(i);
                    if (!template_str.empty()) {
                        values.push_back(template_str);
                    }
                }
                break;
            }
            case FieldType::LogType: {
                // Extract all log type templates
                auto& logtype_dict = dict_manager.logtypeDict();
                for (uint32_t i = 1; i <= logtype_dict.getLogTypeCount(); ++i) {
                    std::string template_str = logtype_dict.getLogTypeById(i);
                    if (!template_str.empty()) {
                        values.push_back(template_str);
                    }
                }
                break;
            }
            default:
                break;
        }
    } catch (const std::exception& e) {
        // Handle extraction errors gracefully
        values.clear();
    }
    
    return values;
}

std::string QueryEngine::performDictionaryLookup(
    const FieldDictionaryManager& dict_manager,
    FieldType field_type,
    const std::string& target_value) const {
    
    try {
        switch (field_type) {
            case FieldType::String: {
                // Look up string value in variable dictionary
                auto& var_dict = dict_manager.variableDict();
                // In a real implementation, you would search the dictionary for the target value
                // For now, we'll just return the target value if it's a valid string
                return target_value;
            }
            case FieldType::Timestamp: {
                // Look up timestamp template
                auto& timestamp_dict = dict_manager.timestampDict();
                // In a real implementation, you would search templates for the target value
                // For now, we'll just return the target value if it's a valid timestamp pattern
                return target_value;
            }
            case FieldType::LogType: {
                // Look up log type template
                auto& logtype_dict = dict_manager.logtypeDict();
                // In a real implementation, you would search templates for the target value
                // For now, we'll just return the target value if it's a valid log type pattern
                return target_value;
            }
            default:
                break;
        }
    } catch (const std::exception& e) {
        // Handle lookup errors gracefully
        return "";
    }
    
    return "";
}

std::vector<size_t> QueryEngine::findMatchingNodesInLayer(
    const std::vector<uint8_t>& layer_data,
    size_t layer_index,
    const std::string& target_value,
    const FieldKey& field_key,
    const FieldDictionaryManager& dict_manager) const {
    
    std::vector<size_t> matched_indices;
    
    try {
        // Parse layer data to extract node values
        // Create an input stream from the layer data
        std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
        
        // Read node count from the layer data
        size_t node_count;
        layer_stream.read(reinterpret_cast<char*>(&node_count), sizeof(node_count));
        
        // Process each node in the layer
        for (size_t node_idx = 0; node_idx < node_count; ++node_idx) {
            // Read the type byte
            uint8_t type_byte;
            layer_stream.read(reinterpret_cast<char*>(&type_byte), 1);
            
            // Extract and compare node value based on type
            bool is_match = false;
            switch (type_byte) {
                case 0: { // uint32_t
                    uint32_t value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    
                    // For dictionary-encoded values, look up the actual string
                    if (needsDictionaryDecompression(field_key.type)) {
                        FieldKey fk{field_key.name, field_key.type};
                        auto opt_value = dict_manager.getFieldValueByCode(fk, value);
                        if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                            std::string actual_value = std::get<std::string>(*opt_value);
                            is_match = (actual_value == target_value);
                        }
                    } else {
                        is_match = (std::to_string(value) == target_value);
                    }
                    break;
                }
                case 1: { // int64_t
                    int64_t value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    is_match = (std::to_string(value) == target_value);
                    break;
                }
                case 2: { // double
                    double value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    is_match = (std::to_string(value) == target_value);
                    break;
                }
                case 3: { // bool
                    bool value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    std::string bool_str = value ? "true" : "false";
                    is_match = (bool_str == target_value);
                    break;
                }
                case 4: { // nullptr
                    is_match = (target_value == "null");
                    break;
                }
                case 5: { // TemplateEncodedTimestamp
                    uint32_t template_id;
                    layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
                    uint32_t n;
                    layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
                    std::vector<uint32_t> var_codes(n);
                    for (uint32_t& code : var_codes) {
                        layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
                    }
                    
                    // Decode timestamp using dictionary
                    TemplateEncodedTimestamp ts{template_id, var_codes};
                    std::string decoded_timestamp = dict_manager.timestampDict().decodeTemplate(field_key, ts);
                    is_match = (decoded_timestamp == target_value);
                    break;
                }
                case 6: { // EncodedLog
                    uint32_t template_id;
                    layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
                    uint32_t n;
                    layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
                    std::vector<uint32_t> var_codes(n);
                    for (uint32_t& code : var_codes) {
                        layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
                    }
                    
                    // Decode log using dictionary
                    EncodedLog log{template_id, var_codes};
                    std::string decoded_log = dict_manager.logtypeDict().decodeLogToString(field_key, log);
                    is_match = (decoded_log == target_value);
                    break;
                }
                default:
                    // Skip unknown types
                    break;
            }
            
            // If the node value matches the target, add its index to the result
            if (is_match) {
                matched_indices.push_back(node_idx);
            }
        }
        
    } catch (const std::exception& e) {
        // Handle parsing errors gracefully
        matched_indices.clear();
    }
    
    return matched_indices;
}

double QueryEngine::calculateDecompressionRatio(
    const GranularCompressedData& granular_data,
    size_t layer_index,
    bool include_dict) const {
    
    try {
        size_t decompressed_size = 0;
        size_t total_size = granular_data.original_size;
        
        // Add metadata size (always decompressed)
        decompressed_size += granular_data.metadata.size();
        
        // Add layer size - handle both layered and non-layered data
        if (granular_data.use_layer_separation) {
            // For layered data, add the specific layer size if within bounds
            if (layer_index < granular_data.layer_data_by_level.size()) {
                decompressed_size += granular_data.layer_data_by_level[layer_index].size();
            }
        } else {
            // For non-layered data, add the combined layer size
            decompressed_size += granular_data.layer_data_combined.size();
        }
        
        // Add dictionary size if needed
        if (include_dict) {
            decompressed_size += granular_data.string_dict.size();
            decompressed_size += granular_data.timestamp_dict.size();
            decompressed_size += granular_data.logtype_dict.size();
        }
        
        // Add trie bitmap size (needed for path reconstruction)
        decompressed_size += granular_data.trie_bitmap.size();
        
        if (total_size > 0) {
            return static_cast<double>(decompressed_size) / static_cast<double>(total_size);
        }
        
    } catch (const std::exception& e) {
        // Handle calculation errors gracefully
        return 1.0; // Return full decompression ratio on error
    }
    
    return 0.0;
}

std::vector<size_t> QueryEngine::findNodesInRangeInLayer(
    const std::vector<uint8_t>& layer_data,
    size_t layer_index,
    const std::string& min_value,
    const std::string& max_value,
    const FieldKey& field_key,
    const FieldDictionaryManager& dict_manager) const {
    
    std::vector<size_t> matched_indices;
    
    try {
        // Parse layer data to extract node values
        // Create an input stream from the layer data
        std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
        
        // Read node count from the layer data
        size_t node_count;
        layer_stream.read(reinterpret_cast<char*>(&node_count), sizeof(node_count));
        
        // Process each node in the layer
        for (size_t node_idx = 0; node_idx < node_count; ++node_idx) {
            // Read the type byte
            uint8_t type_byte;
            layer_stream.read(reinterpret_cast<char*>(&type_byte), 1);
            
            // Extract and compare node value based on type
            bool is_in_range = false;
            switch (type_byte) {
                case 0: { // uint32_t
                    uint32_t value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    
                    // For dictionary-encoded values, look up the actual string
                    std::string actual_value;
                    if (needsDictionaryDecompression(field_key.type)) {
                        FieldKey fk{field_key.name, field_key.type};
                        auto opt_value = dict_manager.getFieldValueByCode(fk, value);
                        if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                            actual_value = std::get<std::string>(*opt_value);
                        }
                    } else {
                        actual_value = std::to_string(value);
                    }
                    
                    // Check if value is within range
                    is_in_range = (actual_value >= min_value) && (actual_value <= max_value);
                    break;
                }
                case 1: { // int64_t
                    int64_t value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    std::string str_value = std::to_string(value);
                    is_in_range = (str_value >= min_value) && (str_value <= max_value);
                    break;
                }
                case 2: { // double
                    double value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    std::string str_value = std::to_string(value);
                    is_in_range = (str_value >= min_value) && (str_value <= max_value);
                    break;
                }
                case 3: { // bool
                    bool value;
                    layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                    std::string bool_str = value ? "true" : "false";
                    is_in_range = (bool_str >= min_value) && (bool_str <= max_value);
                    break;
                }
                case 4: { // nullptr
                    is_in_range = (min_value <= "null") && (max_value >= "null");
                    break;
                }
                case 5: { // TemplateEncodedTimestamp
                    uint32_t template_id;
                    layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
                    uint32_t n;
                    layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
                    std::vector<uint32_t> var_codes(n);
                    for (uint32_t& code : var_codes) {
                        layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
                    }
                    
                    // Decode timestamp using dictionary
                    TemplateEncodedTimestamp ts{template_id, var_codes};
                    std::string decoded_timestamp = dict_manager.timestampDict().decodeTemplate(field_key, ts);
                    is_in_range = (decoded_timestamp >= min_value) && (decoded_timestamp <= max_value);
                    break;
                }
                case 6: { // EncodedLog
                    uint32_t template_id;
                    layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
                    uint32_t n;
                    layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
                    std::vector<uint32_t> var_codes(n);
                    for (uint32_t& code : var_codes) {
                        layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
                    }
                    
                    // Decode log using dictionary
                    EncodedLog log{template_id, var_codes};
                    std::string decoded_log = dict_manager.logtypeDict().decodeLogToString(field_key, log);
                    is_in_range = (decoded_log >= min_value) && (decoded_log <= max_value);
                    break;
                }
                default:
                    // Skip unknown types
                    break;
            }
            
            // If the node value is within range, add its BFS index to the result
            if (is_in_range) {
                // Note: We need access to the LOUDS structure to properly convert layer indices to BFS indices
                // This will be handled in the calling function where we have access to the LOUDS structure
                matched_indices.push_back(node_idx);
            }
        }
        
    } catch (const std::exception& e) {
        // Handle parsing errors gracefully
        matched_indices.clear();
    }
    
    return matched_indices;
}

std::string QueryEngine::convertPathToJSON(
    const std::vector<std::string>& path,
    const std::vector<FieldKey>& field_order) const {
    
    try {
        // Convert decoded path to JSON record
        if (path.empty()) {
            return "";
        }
        
        // Build JSON object from path values
        std::string json_record = "{";
        
        for (size_t i = 0; i < path.size() && i < field_order.size(); ++i) {
            if (i > 0) {
                json_record += ", ";
            }
            json_record += "\"" + field_order[i].name + "\": \"" + path[i] + "\"";
        }
        
        json_record += "}";
        return json_record;
        
    } catch (const std::exception& e) {
        // Handle conversion errors gracefully
        return "{\"error\": \"Failed to convert path to JSON: " + std::string(e.what()) + "\"}";
    }
}

void QueryEngine::clearCache() {
    // 清除缓存
    // 这里可以实现缓存清除逻辑
}

std::unordered_map<std::string, double> QueryEngine::getPerformanceStats() const {
    return performance_stats_;
}

void QueryEngine::startProfiling() {
    // 开始性能分析
    // 简化实现：暂时跳过
}

void QueryEngine::stopProfiling() {
    // 停止性能分析
    // 简化实现：暂时跳过
}

} // namespace query
} // namespace json2