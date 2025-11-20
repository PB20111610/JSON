#include "../include/query/query_engine.h"
#include "../include/compress_type_aware.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include <chrono>
#include <algorithm>
#include <map>
#include <future>

namespace json2 {
namespace query {

GroupQueryResult QueryEngine::executeGroupQuery(
    const std::vector<std::string>& group_fields,
    const std::vector<FieldType>& group_field_types,
    const GranularCompressedData& granular_data) {
    
    GroupQueryResult result;
    result.count = 0;
    result.chunks_accessed = 0;
    result.is_complete = false;
    
    try {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Create a default config for decompression that matches the compression config used
        compression::TypeAwareCompressionConfig config = DEFAULT_COMPRESSION_CONFIG;
        
        // Set up decompression options - we need metadata, trie, and layers for grouping
        Compressor::PartialDecompressionOptions decompress_options;
        decompress_options.load_metadata = true;
        decompress_options.load_trie = true;
        decompress_options.load_layers = true;
        
        // Determine which dictionaries we need based on group field types
        for (FieldType field_type : group_field_types) {
            switch (field_type) {
                case FieldType::STRING:
                    decompress_options.load_string_dict = true;
                    break;
                case FieldType::TIMESTAMP:
                    decompress_options.load_timestamp_dict = true;
                    break;
                case FieldType::LOGTYPE:
                    decompress_options.load_logtype_dict = true;
                    break;
                default:
                    // For numeric types, we don't need dictionaries
                    break;
            }
        }
        
        // Decompress the granular data
        std::unique_ptr<LOUDSTrie> louds_trie = nullptr;
        std::unique_ptr<FieldDictionaryManager> dict_manager = nullptr;
        
        try {
            // Decompress directly to LOUDSTrie
            louds_trie = std::make_unique<LOUDSTrie>();
            
            // Load metadata first to get field order
            if (decompress_options.load_metadata && !granular_data.metadata.empty()) {
                std::vector<uint8_t> metadata_raw = decompressWithConfig(
                    granular_data.metadata, compression::FieldType::STRING, config);
                if (!metadata_raw.empty()) {
                    auto field_order = Compressor::deserializeMetadata(metadata_raw);
                    louds_trie->setFieldOrder(field_order);
                }
            }
            
            // Load trie structure
            if (decompress_options.load_trie && !granular_data.trie_bitmap.empty()) {
                std::vector<uint8_t> trie_raw = decompressWithConfig(
                    granular_data.trie_bitmap, compression::FieldType::BOOL, config);
                if (!trie_raw.empty()) {
                    std::istringstream trie_stream(std::string(trie_raw.begin(), trie_raw.end()));
                    louds_trie->deserializeBitmap(trie_stream);
                }
            }
            
            // Create dictionary manager
            dict_manager = std::make_unique<FieldDictionaryManager>();
            
            // Load dictionaries based on field types
            if (decompress_options.load_string_dict && !granular_data.string_dict.empty()) {
                std::vector<uint8_t> string_dict_raw = decompressWithConfig(
                    granular_data.string_dict, compression::FieldType::STRING, config);
                if (!string_dict_raw.empty()) {
                    Compressor::deserializeStringDictionary(string_dict_raw, *dict_manager);
                }
            }
            
            if (decompress_options.load_timestamp_dict && !granular_data.timestamp_dict.empty()) {
                std::vector<uint8_t> timestamp_dict_raw = decompressWithConfig(
                    granular_data.timestamp_dict, compression::FieldType::TIMESTAMP, config);
                if (!timestamp_dict_raw.empty()) {
                    Compressor::deserializeTimestampDictionary(timestamp_dict_raw, *dict_manager);
                }
            }
            
            if (decompress_options.load_logtype_dict && !granular_data.logtype_dict.empty()) {
                std::vector<uint8_t> logtype_dict_raw = decompressWithConfig(
                    granular_data.logtype_dict, compression::FieldType::LOGTYPE, config);
                if (!logtype_dict_raw.empty()) {
                    Compressor::deserializeLogTypeDictionary(logtype_dict_raw, *dict_manager);
                }
            }
            
            // Load layers
            if (decompress_options.load_layers && granular_data.use_layer_separation) {
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
                    const auto& field_order = louds_trie->getFieldOrder();
                    if (i < field_order.size()) {
                        field_type = mapFieldTypeToCompressionType(field_order[i].type);
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
            result.error_message = "Failed to decompress granular data: " + std::string(e.what());
            return result;
        }
        
        if (!louds_trie || !dict_manager) {
            result.error_message = "Failed to decompress granular data";
            return result;
        }
        
        // Get field order
        const auto& field_order = louds_trie->getFieldOrder();
        
        // Find indices for group fields
        std::vector<int> group_field_indices;
        for (const auto& group_field : group_fields) {
            int index = getFieldIndex(group_field, field_order);
            if (index == -1) {
                result.error_message = "Group field '" + group_field + "' not found in the data";
                return result;
            }
            group_field_indices.push_back(index);
        }
        
        // Group the data by group fields
        // Map to store grouped records: key is group values, value is vector of records
        std::map<std::vector<std::string>, std::vector<std::string>> grouped_data;
        
        // Process all records and group them
        result.chunks_accessed = 1; // We're accessing one chunk
        
        // We need to traverse all complete paths in the trie
        if (louds_trie->layerCount() > 0) {
            size_t last_layer_idx = louds_trie->layerCount() - 1;
            const auto& last_layer = louds_trie->getLayer(last_layer_idx);
            
            for (size_t node_idx = 0; node_idx < last_layer.size(); ++node_idx) {
                // Reconstruct the path for this node
                std::vector<std::string> path_values(field_order.size());
                
                // Get the path from root to this node
                // First convert layer index and node index to BFS index
                size_t bfs_idx = louds_trie->getLayeredStorage().layerIndexToBFS(last_layer_idx, node_idx);
                // Then reconstruct the path to root
                std::vector<size_t> bfs_path = louds_trie->reconstructPathToRoot(bfs_idx);
                
                // Convert BFS path to layer indices path
                std::vector<size_t> path_indices;
                path_indices.reserve(bfs_path.size());
                for (size_t bfs_index : bfs_path) {
                    auto [layer_idx, node_idx_in_layer] = louds_trie->getLayeredStorage().bfsToLayerIndex(bfs_index);
                    path_indices.push_back(node_idx_in_layer);
                }
                
                // Decode each field value in the path
                for (size_t i = 0; i < std::min(path_indices.size(), field_order.size()); ++i) {
                    size_t layer_idx = i;
                    size_t layer_node_idx = path_indices[i];
                    
                    if (layer_idx < louds_trie->layerCount()) {
                        // Get the actual node value directly from the layer
                        const NodeValue& node_value = louds_trie->getLayerNodeValue(layer_idx, layer_node_idx);
                        
                        // Decode the node value to string
                        std::string decoded_value = decodeNodeValueToString(node_value, field_order[layer_idx], *dict_manager);
                        path_values[layer_idx] = decoded_value;
                    }
                }
                
                // Extract group values
                std::vector<std::string> group_values(group_fields.size());
                for (size_t i = 0; i < group_fields.size(); ++i) {
                    if (group_field_indices[i] >= 0 && 
                        static_cast<size_t>(group_field_indices[i]) < path_values.size()) {
                        group_values[i] = path_values[group_field_indices[i]];
                    } else {
                        group_values[i] = "null";
                    }
                }
                
                // Convert path to JSON-like string representation for the record
                std::string record = "{";
                for (size_t i = 0; i < std::min(path_values.size(), field_order.size()); ++i) {
                    if (i > 0) record += ", ";
                    record += "\"" + field_order[i].name + "\": ";
                    
                    // Add quotes for string types
                    bool needs_quotes = (field_order[i].type == FieldType::STRING || 
                                       field_order[i].type == FieldType::TIMESTAMP || 
                                       field_order[i].type == FieldType::LOGTYPE);
                    if (needs_quotes) {
                        record += "\"" + path_values[i] + "\"";
                    } else {
                        record += path_values[i];
                    }
                }
                record += "}";
                
                // Add record to grouped data
                grouped_data[group_values].push_back(record);
                result.count++;
            }
        }
        
        // Set the grouped records in the result
        result.grouped_records = std::move(grouped_data);
        result.is_complete = true;
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing group query: " + std::string(e.what());
    }
    
    return result;
}

// Multi-block implementation
GroupQueryResult QueryEngine::executeGroupQueryMultiBlock(
    const std::vector<std::string>& group_fields,
    const std::vector<FieldType>& group_field_types,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    GroupQueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // Process each chunk
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        // Extract granular data from chunk
        GranularCompressedData granular_data = extractGranularDataFromChunk(chunk, i);
        
        // Execute query on this chunk
        GroupQueryResult chunk_result = executeGroupQuery(group_fields, group_field_types, granular_data);
        
        // Merge grouped records from this chunk
        for (const auto& group_entry : chunk_result.grouped_records) {
            const std::vector<std::string>& group_key = group_entry.first;
            const std::vector<std::string>& records = group_entry.second;
            
            // Add records to existing group or create new group
            final_result.grouped_records[group_key].insert(
                final_result.grouped_records[group_key].end(),
                records.begin(),
                records.end()
            );
        }
        
        // Update counts
        final_result.count += chunk_result.count;
        final_result.chunks_accessed += chunk_result.chunks_accessed;
        
        // Update query time
        final_result.query_time_ms += chunk_result.query_time_ms;
        
        // Update completion status
        final_result.is_complete = final_result.is_complete && chunk_result.is_complete;
        
        // Propagate error message if chunk has one and final doesn't
        if (!chunk_result.error_message.empty() && final_result.error_message.empty() && chunk_result.count == 0) {
            final_result.error_message = chunk_result.error_message;
        }
    }
    
    return final_result;
}

// Parallel multi-block implementation
GroupQueryResult QueryEngine::executeGroupQueryMultiBlockParallel(
    const std::vector<std::string>& group_fields,
    const std::vector<FieldType>& group_field_types,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeGroupQueryMultiBlock(group_fields, group_field_types, chunks);
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
    std::vector<std::future<GroupQueryResult>> futures;
    for (size_t group_idx = 0; group_idx < thread_count; ++group_idx) {
        if (chunk_groups[group_idx].empty()) continue;
        
        // Launch async task with chunk indices
        futures.push_back(std::async(std::launch::async, [this, &group_fields, &group_field_types, &chunks, group_indices = chunk_groups[group_idx]]() {
            GroupQueryResult group_result;
            group_result.count = 0;
            group_result.chunks_accessed = 0;
            group_result.is_complete = true;
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                GroupQueryResult chunk_result = executeGroupQuery(group_fields, group_field_types, granular_data);
                
                // Merge grouped records from this chunk
                for (const auto& group_entry : chunk_result.grouped_records) {
                    const std::vector<std::string>& group_key = group_entry.first;
                    const std::vector<std::string>& records = group_entry.second;
                    
                    // Add records to existing group or create new group
                    group_result.grouped_records[group_key].insert(
                        group_result.grouped_records[group_key].end(),
                        records.begin(),
                        records.end()
                    );
                }
                
                // Update counts
                group_result.count += chunk_result.count;
                group_result.chunks_accessed += chunk_result.chunks_accessed;
                
                // Update query time
                group_result.query_time_ms += chunk_result.query_time_ms;
                
                // Update completion status
                group_result.is_complete = group_result.is_complete && chunk_result.is_complete;
                
                // Propagate error message if chunk has one and group doesn't
                if (!chunk_result.error_message.empty() && group_result.error_message.empty() && chunk_result.count == 0) {
                    group_result.error_message = chunk_result.error_message;
                }
            }
            
            return group_result;
        }));
    }
    
    // Collect results from all threads
    GroupQueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true;
    
    for (auto& future : futures) {
        try {
            GroupQueryResult group_result = future.get();
            
            // Merge grouped records from this group
            for (const auto& group_entry : group_result.grouped_records) {
                const std::vector<std::string>& group_key = group_entry.first;
                const std::vector<std::string>& records = group_entry.second;
                
                // Add records to existing group or create new group
                final_result.grouped_records[group_key].insert(
                    final_result.grouped_records[group_key].end(),
                    records.begin(),
                    records.end()
                );
            }
            
            // Update counts
            final_result.count += group_result.count;
            final_result.chunks_accessed += group_result.chunks_accessed;
            
            // Update query time
            final_result.query_time_ms += group_result.query_time_ms;
            
            // Update completion status
            final_result.is_complete = final_result.is_complete && group_result.is_complete;
            
            // Propagate error message if group has one and final doesn't
            if (!group_result.error_message.empty() && final_result.error_message.empty() && group_result.count == 0) {
                final_result.error_message = group_result.error_message;
            }
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