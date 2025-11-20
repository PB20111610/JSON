#include "../include/query/query_engine.h"
#include "../include/compress_type_aware.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include <chrono>
#include <future>
#include <algorithm>

namespace json2 {
namespace query {

int QueryEngine::getFieldIndex(const std::string& field_name, const std::vector<FieldKey>& field_order) const {
    for (size_t i = 0; i < field_order.size(); ++i) {
        if (field_order[i].name == field_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Helper function to decode node value to string
std::string QueryEngine::decodeNodeValueToString(
    const NodeValue& node_value,
    const FieldKey& field_key,
    const FieldDictionaryManager& dict_manager) const {
    
    try {
        // Use the decodeNodeValueToFieldValue function from loudsTotrie.cpp
        return json2::decodeNodeValueToFieldValue(field_key, node_value, dict_manager);
    } catch (const std::exception& e) {
        return "ERROR";
    }
}

// Helper function to decode node value to double
double QueryEngine::decodeNodeValueToDouble(
    const NodeValue& node_value,
    const FieldKey& field_key,
    const FieldDictionaryManager& dict_manager) const {
    
    try {
        // First decode to string
        std::string str_value = decodeNodeValueToString(node_value, field_key, dict_manager);
        
        // Convert to double based on field type
        switch (field_key.type) {
            case FieldType::INT64:
                return static_cast<double>(std::stoll(str_value));
            case FieldType::DOUBLE:
                return std::stod(str_value);
            case FieldType::BOOL:
                return str_value == "true" ? 1.0 : 0.0;
            default:
                // For non-numeric types, try to convert to double
                try {
                    return std::stod(str_value);
                } catch (...) {
                    return 0.0;
                }
        }
    } catch (const std::exception& e) {
        return 0.0;
    }
}

AggregateQueryResult QueryEngine::executeAggregateQuery(
    AggregateFunction aggregate_func,
    const std::string& field_name,
    FieldType expected_type,
    const GranularCompressedData& granular_data) {
    
    AggregateQueryResult result;
    result.value = 0.0;
    result.count = 0;
    result.field_name = field_name;
    result.chunks_accessed = 0;
    result.is_complete = false;
    
    try {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Set aggregate type string for result
        switch (aggregate_func) {
            case AggregateFunction::COUNT:
                result.aggregate_type = "COUNT";
                break;
            case AggregateFunction::SUM:
                result.aggregate_type = "SUM";
                break;
            case AggregateFunction::AVG:
                result.aggregate_type = "AVG";
                break;
            case AggregateFunction::MAX:
                result.aggregate_type = "MAX";
                break;
            case AggregateFunction::MIN:
                result.aggregate_type = "MIN";
                break;
        }
        
        // Create a default config for decompression that matches the compression config used
        compression::TypeAwareCompressionConfig config = DEFAULT_COMPRESSION_CONFIG;
        
        // For COUNT with empty field name, we only need to count records without decompressing fields
        if (aggregate_func == AggregateFunction::COUNT && field_name.empty()) {
            // Only decompress metadata to get field order
            std::vector<uint8_t> metadata_raw = decompressWithConfig(
                granular_data.metadata, compression::FieldType::STRING, config);
            
            if (metadata_raw.empty()) {
                result.error_message = "Metadata is empty";
                return result;
            }
            
            std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
            
            // Create LOUDS trie to count nodes in the last layer
            std::unique_ptr<LOUDSTrie> louds_trie = std::make_unique<LOUDSTrie>();
            louds_trie->setFieldOrder(field_order);
            
            // Load trie structure
            std::vector<uint8_t> trie_raw = decompressWithConfig(
                granular_data.trie_bitmap, compression::FieldType::BOOL, config);
            
            if (!trie_raw.empty()) {
                std::istringstream trie_stream(std::string(trie_raw.begin(), trie_raw.end()));
                louds_trie->deserializeBitmap(trie_stream);
            }
            
            // Count all nodes in the last layer (complete records)
            if (!louds_trie->layerCount()) {
                result.error_message = "No layers found in trie";
                return result;
            }
            
            size_t last_layer_idx = louds_trie->layerCount() - 1;
            result.value = static_cast<double>(louds_trie->getLayer(last_layer_idx).size());
            result.count = static_cast<size_t>(result.value);
            result.chunks_accessed = 1;
            result.is_complete = true;
            
            auto end_time = std::chrono::high_resolution_clock::now();
            result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
            return result;
        }
        
        // For other cases, we need to decompress necessary components
        // Set up decompression options based on what we need
        Compressor::PartialDecompressionOptions decompress_options;
        decompress_options.load_metadata = true;
        decompress_options.load_trie = true;
        
        // Determine which dictionaries we need based on field type
        switch (expected_type) {
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
        
        // We always need layers for field value extraction
        decompress_options.load_layers = true;
        
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
            
            // Load dictionaries based on field type
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
        
        // Find the target field
        int field_index = getFieldIndex(field_name, field_order);
        
        if (field_index == -1) {
            result.error_message = "Field '" + field_name + "' not found in the data";
            return result;
        }
        
        // Determine the field type
        FieldType target_field_type = field_order[field_index].type;
        FieldKey target_field_key = field_order[field_index];
        
        // Search through all nodes in the trie to find matching paths
        result.chunks_accessed = 1; // We're accessing one chunk
        
        // For COUNT on a specific field, count non-null values
        if (aggregate_func == AggregateFunction::COUNT) {
            size_t count = 0;
            
            // We'll search in the specific layer corresponding to our field
            if (static_cast<size_t>(field_index) < louds_trie->layerCount()) {
                const auto& layer = louds_trie->getLayer(field_index);
                
                for (size_t node_idx = 0; node_idx < layer.size(); ++node_idx) {
                    // Get the actual node value directly from the layer
                    const NodeValue& node_value = louds_trie->getLayerNodeValue(field_index, node_idx);
                    
                    // Decode the node value to string
                    std::string decoded_value = decodeNodeValueToString(node_value, target_field_key, *dict_manager);
                    
                    // Count non-null values
                    if (decoded_value != "null" && !decoded_value.empty()) {
                        count++;
                    }
                }
            }
            
            result.value = static_cast<double>(count);
            result.count = count;
            result.is_complete = true;
        } else {
            // For SUM, AVG, MAX, MIN, we need to process numeric values
            std::vector<double> values;
            
            // We'll search in the specific layer corresponding to our field
            if (static_cast<size_t>(field_index) < louds_trie->layerCount()) {
                const auto& layer = louds_trie->getLayer(field_index);
                
                for (size_t node_idx = 0; node_idx < layer.size(); ++node_idx) {
                    // Get the actual node value directly from the layer
                    const NodeValue& node_value = louds_trie->getLayerNodeValue(field_index, node_idx);
                    
                    // Decode the node value to double
                    double decoded_value = decodeNodeValueToDouble(node_value, target_field_key, *dict_manager);
                    
                    // Collect values for aggregation
                    values.push_back(decoded_value);
                }
            }
            
            result.count = values.size();
            
            if (!values.empty()) {
                switch (aggregate_func) {
                    case AggregateFunction::SUM: {
                        double sum = 0.0;
                        for (double val : values) {
                            sum += val;
                        }
                        result.value = sum;
                        break;
                    }
                    case AggregateFunction::AVG: {
                        double sum = 0.0;
                        for (double val : values) {
                            sum += val;
                        }
                        result.value = sum / values.size();
                        break;
                    }
                    case AggregateFunction::MAX: {
                        double max_val = values[0];
                        for (double val : values) {
                            if (val > max_val) max_val = val;
                        }
                        result.value = max_val;
                        break;
                    }
                    case AggregateFunction::MIN: {
                        double min_val = values[0];
                        for (double val : values) {
                            if (val < min_val) min_val = val;
                        }
                        result.value = min_val;
                        break;
                    }
                    default:
                        result.error_message = "Unsupported aggregate function";
                        return result;
                }
            }
            
            result.is_complete = true;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing aggregate query: " + std::string(e.what());
    }
    
    return result;
}

// Multi-block implementation
AggregateQueryResult QueryEngine::executeAggregateQueryMultiBlock(
    AggregateFunction aggregate_func,
    const std::string& field_name,
    FieldType expected_type,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    AggregateQueryResult final_result;
    final_result.value = 0.0;
    final_result.count = 0;
    final_result.field_name = field_name;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // Set aggregate type string for result
    switch (aggregate_func) {
        case AggregateFunction::COUNT:
            final_result.aggregate_type = "COUNT";
            break;
        case AggregateFunction::SUM:
            final_result.aggregate_type = "SUM";
            break;
        case AggregateFunction::AVG:
            final_result.aggregate_type = "AVG";
            break;
        case AggregateFunction::MAX:
            final_result.aggregate_type = "MAX";
            break;
        case AggregateFunction::MIN:
            final_result.aggregate_type = "MIN";
            break;
    }
    
    // Process each chunk
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        // Extract granular data from chunk
        GranularCompressedData granular_data = extractGranularDataFromChunk(chunk, i);
        
        // Execute query on this chunk
        AggregateQueryResult chunk_result = executeAggregateQuery(aggregate_func, field_name, expected_type, granular_data);
        
        // Merge results based on aggregate type
        if (final_result.aggregate_type.empty()) {
            // First result, just copy values
            final_result.value = chunk_result.value;
            final_result.count = chunk_result.count;
            final_result.aggregate_type = chunk_result.aggregate_type;
        } else {
            // Combine based on aggregate type
            if (final_result.aggregate_type == "COUNT" || final_result.aggregate_type == "SUM") {
                final_result.value += chunk_result.value;
                final_result.count += chunk_result.count;
            } else if (final_result.aggregate_type == "AVG") {
                // For average, we need to recompute based on total count and sum
                double total_sum = final_result.value * final_result.count + chunk_result.value * chunk_result.count;
                final_result.count += chunk_result.count;
                if (final_result.count > 0) {
                    final_result.value = total_sum / final_result.count;
                }
            } else if (final_result.aggregate_type == "MAX") {
                if (chunk_result.value > final_result.value) {
                    final_result.value = chunk_result.value;
                }
                final_result.count += chunk_result.count;
            } else if (final_result.aggregate_type == "MIN") {
                if (chunk_result.value < final_result.value || final_result.count == 0) {
                    final_result.value = chunk_result.value;
                }
                final_result.count += chunk_result.count;
            }
        }
        
        // Update chunks accessed
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
AggregateQueryResult QueryEngine::executeAggregateQueryMultiBlockParallel(
    AggregateFunction aggregate_func,
    const std::string& field_name,
    FieldType expected_type,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeAggregateQueryMultiBlock(aggregate_func, field_name, expected_type, chunks);
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
    std::vector<std::future<AggregateQueryResult>> futures;
    for (size_t group_idx = 0; group_idx < thread_count; ++group_idx) {
        if (chunk_groups[group_idx].empty()) continue;
        
        // Launch async task with chunk indices
        futures.push_back(std::async(std::launch::async, [this, &aggregate_func, &field_name, &expected_type, &chunks, group_indices = chunk_groups[group_idx]]() {
            AggregateQueryResult group_result;
            group_result.value = 0.0;
            group_result.count = 0;
            group_result.field_name = field_name;
            group_result.chunks_accessed = 0;
            group_result.is_complete = true;
            
            // Set aggregate type string for result
            switch (aggregate_func) {
                case AggregateFunction::COUNT:
                    group_result.aggregate_type = "COUNT";
                    break;
                case AggregateFunction::SUM:
                    group_result.aggregate_type = "SUM";
                    break;
                case AggregateFunction::AVG:
                    group_result.aggregate_type = "AVG";
                    break;
                case AggregateFunction::MAX:
                    group_result.aggregate_type = "MAX";
                    break;
                case AggregateFunction::MIN:
                    group_result.aggregate_type = "MIN";
                    break;
            }
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                AggregateQueryResult chunk_result = executeAggregateQuery(aggregate_func, field_name, expected_type, granular_data);
                
                // Merge results based on aggregate type
                if (group_result.aggregate_type.empty()) {
                    // First result, just copy values
                    group_result.value = chunk_result.value;
                    group_result.count = chunk_result.count;
                    group_result.aggregate_type = chunk_result.aggregate_type;
                } else {
                    // Combine based on aggregate type
                    if (group_result.aggregate_type == "COUNT" || group_result.aggregate_type == "SUM") {
                        group_result.value += chunk_result.value;
                        group_result.count += chunk_result.count;
                    } else if (group_result.aggregate_type == "AVG") {
                        // For average, we need to recompute based on total count and sum
                        double total_sum = group_result.value * group_result.count + chunk_result.value * chunk_result.count;
                        group_result.count += chunk_result.count;
                        if (group_result.count > 0) {
                            group_result.value = total_sum / group_result.count;
                        }
                    } else if (group_result.aggregate_type == "MAX") {
                        if (chunk_result.value > group_result.value) {
                            group_result.value = chunk_result.value;
                        }
                        group_result.count += chunk_result.count;
                    } else if (group_result.aggregate_type == "MIN") {
                        if (chunk_result.value < group_result.value || group_result.count == 0) {
                            group_result.value = chunk_result.value;
                        }
                        group_result.count += chunk_result.count;
                    }
                }
                
                // Update chunks accessed
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
    AggregateQueryResult final_result;
    final_result.value = 0.0;
    final_result.count = 0;
    final_result.field_name = field_name;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true;
    
    // Set aggregate type string for result
    switch (aggregate_func) {
        case AggregateFunction::COUNT:
            final_result.aggregate_type = "COUNT";
            break;
        case AggregateFunction::SUM:
            final_result.aggregate_type = "SUM";
            break;
        case AggregateFunction::AVG:
            final_result.aggregate_type = "AVG";
            break;
        case AggregateFunction::MAX:
            final_result.aggregate_type = "MAX";
            break;
        case AggregateFunction::MIN:
            final_result.aggregate_type = "MIN";
            break;
    }
    
    for (auto& future : futures) {
        try {
            AggregateQueryResult group_result = future.get();
            
            // Merge results based on aggregate type
            if (final_result.aggregate_type.empty()) {
                // First result, just copy values
                final_result.value = group_result.value;
                final_result.count = group_result.count;
                final_result.aggregate_type = group_result.aggregate_type;
            } else {
                // Combine based on aggregate type
                if (final_result.aggregate_type == "COUNT" || final_result.aggregate_type == "SUM") {
                    final_result.value += group_result.value;
                    final_result.count += group_result.count;
                } else if (final_result.aggregate_type == "AVG") {
                    // For average, we need to recompute based on total count and sum
                    double total_sum = final_result.value * final_result.count + group_result.value * group_result.count;
                    final_result.count += group_result.count;
                    if (final_result.count > 0) {
                        final_result.value = total_sum / final_result.count;
                    }
                } else if (final_result.aggregate_type == "MAX") {
                    if (group_result.value > final_result.value) {
                        final_result.value = group_result.value;
                    }
                    final_result.count += group_result.count;
                } else if (final_result.aggregate_type == "MIN") {
                    if (group_result.value < final_result.value || final_result.count == 0) {
                        final_result.value = group_result.value;
                    }
                    final_result.count += group_result.count;
                }
            }
            
            // Update chunks accessed
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