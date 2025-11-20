#include "../include/query/query_engine.h"
#include "../include/query/selective_decompressor.h"
#include "../include/field_dictionary_manager.h"
#include "../include/timestamp_dictionary.h"
#include "../include/logtype_dictionary.h"
#include "../test/test_config_utils.h"
#include <iostream>
#include <optional>
#include <variant>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>

#ifdef _MSC_VER
#include <time.h>
#else
#include <time.h>
#include <sys/time.h>
#endif

namespace json2 {
namespace query {

// Define the static compression configuration - reference the same config as the test
const compression::TypeAwareCompressionConfig QueryEngine::DEFAULT_COMPRESSION_CONFIG = json2::getTestConfig().type_aware_config;

// Constructor
QueryEngine::QueryEngine(const QueryConfig& config) : config_(config) {}

// Set the data directory for granular data extraction
void QueryEngine::setDataDirectory(const std::string& data_dir) {
    data_dir_ = data_dir;
}

// Helper function to decompress data with config
std::vector<uint8_t> decompressWithConfigWrapper(const std::vector<uint8_t>& compressed_data, 
                                                 compression::FieldType field_type,
                                                 const compression::TypeAwareCompressionConfig& config) {
    if (compressed_data.empty()) {
        return std::vector<uint8_t>();
    }
    
    try {
        return decompressWithConfig(compressed_data, field_type, config);
    } catch (...) {
        return std::vector<uint8_t>();
    }
}

// Helper function to encode target value using existing dictionary without modifying it
/*static*/ std::optional<NodeValue> QueryEngine::encodeTargetValueWithDictionary(
    const std::string& target_value,
    FieldType field_type,
    const FieldDictionaryManager& dict_manager) {
    
    try {
        switch (field_type) {
            case FieldType::STRING: {
                // For String fields, look up the value in the variable dictionary
                const auto& var_dict = dict_manager.variableDict();
                std::vector<std::string> all_values = var_dict.getAllStringValues();
                for (uint32_t i = 0; i < all_values.size(); ++i) {
                    if (all_values[i] == target_value) {
                        return NodeValue(static_cast<uint32_t>(i + 1)); // VariableDictionary uses 1-based indexing
                    }
                }
                // Value not found in dictionary
                return std::nullopt;
            }
            case FieldType::TIMESTAMP: {
                // For Timestamp fields, try to encode using existing dictionary
                // Use the timestamp dictionary's encodeTemplate method directly
                FieldKey key{"", FieldType::TIMESTAMP}; // Empty key for encoding
                // We need a non-const reference to the dictionary
                TimestampDictionary& dict = const_cast<TimestampDictionary&>(dict_manager.timestampDict());
                TemplateEncodedTimestamp encoded = dict.encodeTemplate(key, target_value);
                // Check if encoding was successful by verifying template_id
                if (encoded.template_id > 0) {
                    return NodeValue(encoded);
                }
                return std::nullopt;
            }
            case FieldType::LOGTYPE: {
                // For LogType fields, try to encode using existing dictionary
                // Use the logtype dictionary's encodeLog method directly
                FieldKey key{"", FieldType::LOGTYPE}; // Empty key for encoding
                // We need a non-const reference to the dictionary
                LogTypeDictionary& dict = const_cast<LogTypeDictionary&>(dict_manager.logtypeDict());
                // Use the public two-parameter version of encodeLog by extracting template and variables
                auto [tmpl, vars] = dict.extractTemplateAndVars(target_value);
                EncodedLog encoded = dict.encodeLog(tmpl, vars);
                // Check if encoding was successful by verifying template_id
                if (encoded.template_id > 0) {
                    return NodeValue(encoded);
                }
                return std::nullopt;
            }
            default:
                // For non-dictionary types, no encoding is needed
                return std::nullopt;
        }
    } catch (const std::exception& e) {
        // If any error occurs during encoding, return nullopt
        return std::nullopt;
    }
}

// Helper function to encode target value using dictionary from granular data
std::optional<NodeValue> QueryEngine::encodeTargetValueWithDictionary(
    const std::string& target_value,
    FieldType field_type,
    const GranularCompressedData& granular_data) {
    
    // Only process dictionary-encoded fields
    if (field_type != FieldType::STRING && field_type != FieldType::TIMESTAMP && field_type != FieldType::LOGTYPE) {
        return std::nullopt;
    }
    
    try {
        // Create dictionary manager and decompress dictionary if needed
        FieldDictionaryManager dict_manager;
        
        if (field_type == FieldType::STRING && !granular_data.string_dict.empty()) {
            auto decompressed_dict = decompressWithConfigWrapper(granular_data.string_dict, 
                                                               compression::FieldType::STRING, 
                                                               DEFAULT_COMPRESSION_CONFIG);
            if (!decompressed_dict.empty()) {
                Compressor::deserializeStringDictionary(decompressed_dict, dict_manager);
                return encodeTargetValueWithDictionary(target_value, field_type, dict_manager);
            }
        } else if (field_type == FieldType::TIMESTAMP && !granular_data.timestamp_dict.empty()) {
            auto decompressed_dict = decompressWithConfigWrapper(granular_data.timestamp_dict, 
                                                               compression::FieldType::TIMESTAMP, 
                                                               DEFAULT_COMPRESSION_CONFIG);
            if (!decompressed_dict.empty()) {
                Compressor::deserializeTimestampDictionary(decompressed_dict, dict_manager);
                return encodeTargetValueWithDictionary(target_value, field_type, dict_manager);
            }
        } else if (field_type == FieldType::LOGTYPE && !granular_data.logtype_dict.empty()) {
            auto decompressed_dict = decompressWithConfigWrapper(granular_data.logtype_dict, 
                                                               compression::FieldType::LOGTYPE, 
                                                               DEFAULT_COMPRESSION_CONFIG);
            if (!decompressed_dict.empty()) {
                Compressor::deserializeLogTypeDictionary(decompressed_dict, dict_manager);
                return encodeTargetValueWithDictionary(target_value, field_type, dict_manager);
            }
        }
        
        // If we couldn't decompress or encode, return nullopt
        return std::nullopt;
    } catch (const std::exception& e) {
        // If any error occurs during encoding, return nullopt
        return std::nullopt;
    }
}

// Implementation of checkFieldExistenceAndType from the backup implementation
json2::query::FieldFilterResult QueryEngine::checkFieldExistenceAndType(
    const std::string& field_name,
    FieldType expected_type,
    const GranularCompressedData& granular_data,
    const std::vector<FieldKey>* field_order) {
    
    FieldFilterResult result;
    result.exists = false;
    result.type_matches = false;
    result.actual_type = FieldType::STRING; // Default value
    result.field_index = -1; // Initialize field index
    
    try {
        std::vector<FieldKey> local_field_order;
        
        // Use provided field_order or decompress metadata
        if (field_order != nullptr) {
            local_field_order = *field_order;
        } else {
            // Step 1: Only decompress metadata to check field existence and type
            if (granular_data.metadata.empty()) {
                result.error_message = "Metadata is empty";
                return result;
            }
            
            // Decompress metadata using the proper type-aware decompression function
            std::vector<uint8_t> metadata_raw = decompressWithConfigWrapper(granular_data.metadata, 
                                                                          compression::FieldType::STRING, 
                                                                          DEFAULT_COMPRESSION_CONFIG);
            
            if (metadata_raw.empty()) {
                result.error_message = "Failed to decompress metadata";
                return result;
            }
            
            local_field_order = Compressor::deserializeMetadata(metadata_raw);
        }
        
        // Step 2: Check if field exists and matches type
        for (size_t i = 0; i < local_field_order.size(); ++i) {
            if (local_field_order[i].name == field_name) {
                result.exists = true;
                result.actual_type = local_field_order[i].type;
                result.type_matches = (local_field_order[i].type == expected_type);
                result.field_index = static_cast<int>(i); // Set field index
                result.field_key = local_field_order[i];  // Set field key
                break;
            }
        }
        
        // Performance optimization: Early return if field doesn't exist
        if (!result.exists) {
            return result;
        }
        
    } catch (const std::exception& e) {
        result.error_message = "Error checking field existence: " + std::string(e.what());
        return result;
    }
    
    return result;
}

// Implementation of filterLayerValues
json2::query::ValueFilterResult QueryEngine::filterLayerValues(
    const std::string& field_name,
    FieldType field_type,
    const std::string& target_value,
    const std::string& comparison_op,
    const GranularCompressedData& granular_data,
    const FieldDictionaryManager* dict_manager) {
    
    ValueFilterResult result;
    result.match_found = false;
    result.count = 0;
    
    try {
        // Get the field index from metadata
        if (granular_data.metadata.empty()) {
            result.error_message = "Metadata is empty";
            return result;
        }
        
        // Decompress metadata using the proper type-aware decompression function
        std::vector<uint8_t> metadata_raw = decompressWithConfigWrapper(granular_data.metadata, 
                                                                      compression::FieldType::STRING, 
                                                                      DEFAULT_COMPRESSION_CONFIG);
        
        if (metadata_raw.empty()) {
            result.error_message = "Failed to decompress metadata";
            return result;
        }
        
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        
        // Get the field index
        int field_index = -1;
        for (size_t i = 0; i < field_order.size(); ++i) {
            if (field_order[i].name == field_name) {
                field_index = static_cast<int>(i);
                break;
            }
        }
        
        if (field_index == -1) {
            result.error_message = "Field not found in metadata: " + field_name;
            return result;
        }
        
        // Map field type to compression type
        compression::FieldType compression_field_type;
        switch (field_type) {
            case FieldType::INT64:
                compression_field_type = compression::FieldType::INT64;
                break;
            case FieldType::DOUBLE:
                compression_field_type = compression::FieldType::DOUBLE;
                break;
            case FieldType::BOOL:
                compression_field_type = compression::FieldType::BOOL;
                break;
            case FieldType::STRING:
                compression_field_type = compression::FieldType::STRING;
                break;
            case FieldType::TIMESTAMP:
                compression_field_type = compression::FieldType::TIMESTAMP;
                break;
            case FieldType::LOGTYPE:
                compression_field_type = compression::FieldType::LOGTYPE;
                break;
            default:
                result.error_message = "Unsupported field type for layer filtering";
                return result;
        }
        
        // Get the compressed layer data based on storage method
        std::vector<uint8_t> compressed_layer;
        if (granular_data.use_layer_separation) {
            // Layers are stored separately in layer_data_by_level
            if (static_cast<size_t>(field_index) >= granular_data.layer_data_by_level.size()) {
                result.error_message = "Layer data not available for field: " + field_name + 
                                     " (field_index: " + std::to_string(field_index) + 
                                     ", available layers: " + std::to_string(granular_data.layer_data_by_level.size()) + ")";
                return result;
            }
            compressed_layer = granular_data.layer_data_by_level[field_index];
        } else {
            // Layers are stored combined in layer_data_combined
            if (granular_data.layer_data_combined.empty()) {
                result.error_message = "Combined layer data is empty for field: " + field_name;
                return result;
            }
            
            // For combined storage, we need to extract the specific layer
            // This requires parsing the combined data structure
            try {
                std::istringstream layer_stream(std::string(granular_data.layer_data_combined.begin(), 
                                                           granular_data.layer_data_combined.end()));
                
                // Read layer count
                uint32_t layer_count;
                layer_stream.read(reinterpret_cast<char*>(&layer_count), sizeof(layer_count));
                
                if (static_cast<size_t>(field_index) >= layer_count) {
                    result.error_message = "Layer index out of range in combined data: " + field_name;
                    return result;
                }
                
                // Read layer sizes
                std::vector<uint32_t> layer_sizes(layer_count);
                if (!layer_stream.read(reinterpret_cast<char*>(layer_sizes.data()), 
                                      layer_count * sizeof(uint32_t))) {
                    result.error_message = "Failed to read layer sizes from combined data";
                    return result;
                }
                
                // Skip to the target layer
                size_t offset = sizeof(layer_count) + layer_count * sizeof(uint32_t);
                for (int i = 0; i < field_index; ++i) {
                    offset += layer_sizes[i];
                }
                
                // Read the target layer data
                if (offset + layer_sizes[field_index] > granular_data.layer_data_combined.size()) {
                    result.error_message = "Layer data size mismatch in combined data";
                    return result;
                }
                
                compressed_layer = std::vector<uint8_t>(
                    granular_data.layer_data_combined.begin() + offset,
                    granular_data.layer_data_combined.begin() + offset + layer_sizes[field_index]);
            } catch (const std::exception& e) {
                result.error_message = "Failed to extract layer from combined data: " + std::string(e.what());
                return result;
            }
        }
        
        if (compressed_layer.empty()) {
            result.error_message = "Compressed layer data is empty for field: " + field_name;
            return result;
        }
        
        // Create selective decompressor
        SelectiveDecompressor decompressor;
        
        // Determine layer size for decompression
        size_t layer_size = 0;
        if (!granular_data.layer_sizes.empty()) {
            try {
                std::vector<uint8_t> layer_sizes_raw = decompressWithConfigWrapper(
                    granular_data.layer_sizes, compression::FieldType::STRING, DEFAULT_COMPRESSION_CONFIG);
                if (layer_sizes_raw.size() >= sizeof(uint32_t) * (field_index + 1)) {
                    const uint32_t* layer_sizes_data = reinterpret_cast<const uint32_t*>(layer_sizes_raw.data());
                    layer_size = layer_sizes_data[field_index];
                }
            } catch (...) {
                // If we can't get layer size, we'll estimate it during decompression
            }
        }
        
        // Perform accurate filtering by iterating through layer values
        size_t match_count = 0;
        std::vector<std::string> matched_records;
        std::vector<size_t> matched_indices; // Store matched indices
        
        // For dictionary-encoded fields, encode the target value once before the loop
        std::optional<NodeValue> encoded_target;
        if (dict_manager != nullptr) {
            encoded_target = encodeTargetValueWithDictionary(target_value, field_type, *dict_manager);
        } else {
            encoded_target = encodeTargetValueWithDictionary(target_value, field_type, granular_data);
        }

        // Use the actual layer size if available, otherwise use a reasonable default
        size_t max_values_to_check = layer_size > 0 ? layer_size : 100000; // Increased limit for better accuracy;
        
        // For all types, we'll iterate with bounds checking
        for (size_t i = 0; i < max_values_to_check; ++i) {
            try {
                // Try to get the value at index i
                // For TIMESTAMP and LOGTYPE fields, use direct random access instead of the non-existent decompressLayerValueAtAsInt64s
                NodeValue current_value;
                
                // Use direct random access for all field types
                current_value = decompressor.decompressLayerValueAt(
                    compressed_layer, i, compression_field_type, DEFAULT_COMPRESSION_CONFIG, layer_size);
                
                // Process timestamp and logtype fields
                if (field_type == FieldType::TIMESTAMP || field_type == FieldType::LOGTYPE) {
                }
                
                // Compare with target value based on comparison operator
                bool is_match = false;
                
                // Handle different NodeValue types based on field type
                switch (field_type) {
                    case FieldType::INT64:
                    case FieldType::DOUBLE:
                    case FieldType::BOOL: {
                        // Parse target value based on field type
                        NodeValue target_node_value;
                        try {
                            switch (field_type) {
                                case FieldType::INT64: {
                                    int64_t int_val = std::stoll(target_value);
                                    target_node_value = int_val;
                                    break;
                                }
                                case FieldType::DOUBLE: {
                                    double double_val = std::stod(target_value);
                                    target_node_value = double_val;
                                    break;
                                }
                                case FieldType::BOOL: {
                                    bool bool_val = (target_value == "true");
                                    target_node_value = bool_val;
                                    break;
                                }
                                default:
                                    result.error_message = "Unsupported field type for value parsing";
                                    return result;
                            }
                        } catch (const std::exception& e) {
                            result.error_message = "Failed to parse target value: " + std::string(e.what());
                            return result;
                        }
                        
                        // Perform comparison based on the actual types
                        if (comparison_op == "=" || comparison_op.empty()) {
                            is_match = (current_value == target_node_value);
                        } else if (comparison_op == "!=") {
                            is_match = (current_value != target_node_value);
                        } else if (comparison_op == "<") {
                            // Numeric comparison
                            if (std::holds_alternative<int64_t>(current_value) && std::holds_alternative<int64_t>(target_node_value)) {
                                is_match = (std::get<int64_t>(current_value) < std::get<int64_t>(target_node_value));
                            } else if (std::holds_alternative<double>(current_value) && std::holds_alternative<double>(target_node_value)) {
                                is_match = (std::get<double>(current_value) < std::get<double>(target_node_value));
                            } else if (std::holds_alternative<bool>(current_value) && std::holds_alternative<bool>(target_node_value)) {
                                is_match = (static_cast<int>(std::get<bool>(current_value)) < static_cast<int>(std::get<bool>(target_node_value)));
                            }
                        } else if (comparison_op == ">") {
                            if (std::holds_alternative<int64_t>(current_value) && std::holds_alternative<int64_t>(target_node_value)) {
                                is_match = (std::get<int64_t>(current_value) > std::get<int64_t>(target_node_value));
                            } else if (std::holds_alternative<double>(current_value) && std::holds_alternative<double>(target_node_value)) {
                                is_match = (std::get<double>(current_value) > std::get<double>(target_node_value));
                            } else if (std::holds_alternative<bool>(current_value) && std::holds_alternative<bool>(target_node_value)) {
                                is_match = (static_cast<int>(std::get<bool>(current_value)) > static_cast<int>(std::get<bool>(target_node_value)));
                            }
                        } else if (comparison_op == "<=") {
                            if (std::holds_alternative<int64_t>(current_value) && std::holds_alternative<int64_t>(target_node_value)) {
                                is_match = (std::get<int64_t>(current_value) <= std::get<int64_t>(target_node_value));
                            } else if (std::holds_alternative<double>(current_value) && std::holds_alternative<double>(target_node_value)) {
                                is_match = (std::get<double>(current_value) <= std::get<double>(target_node_value));
                            } else if (std::holds_alternative<bool>(current_value) && std::holds_alternative<bool>(target_node_value)) {
                                is_match = (static_cast<int>(std::get<bool>(current_value)) <= static_cast<int>(std::get<bool>(target_node_value)));
                            }
                        } else if (comparison_op == ">=") {
                            if (std::holds_alternative<int64_t>(current_value) && std::holds_alternative<int64_t>(target_node_value)) {
                                is_match = (std::get<int64_t>(current_value) >= std::get<int64_t>(target_node_value));
                            } else if (std::holds_alternative<double>(current_value) && std::holds_alternative<double>(target_node_value)) {
                                is_match = (std::get<double>(current_value) >= std::get<double>(target_node_value));
                            } else if (std::holds_alternative<bool>(current_value) && std::holds_alternative<bool>(target_node_value)) {
                                is_match = (static_cast<int>(std::get<bool>(current_value)) >= static_cast<int>(std::get<bool>(target_node_value)));
                            }
                        }
                        break;
                    }
                    case FieldType::STRING: {
                        // For String fields, use encoded value comparison for better performance
                        if (std::holds_alternative<uint32_t>(current_value) && encoded_target.has_value() && std::holds_alternative<uint32_t>(*encoded_target)) {
                            // Compare encoded values directly
                            is_match = (std::get<uint32_t>(current_value) == std::get<uint32_t>(*encoded_target));
                        }
                        break;
                    }
                    case FieldType::TIMESTAMP: {
                        // For Timestamp fields, use encoded value comparison for better performance
                        if (std::holds_alternative<TemplateEncodedTimestamp>(current_value) && encoded_target.has_value() && std::holds_alternative<TemplateEncodedTimestamp>(*encoded_target)) {
                            // Direct comparison of TemplateEncodedTimestamp structures
                            is_match = (std::get<TemplateEncodedTimestamp>(current_value) == std::get<TemplateEncodedTimestamp>(*encoded_target));
                            
                        }
                        break;
                    }
                    case FieldType::LOGTYPE: {
                        // For LogType fields, use encoded value comparison for better performance
                        if (std::holds_alternative<EncodedLog>(current_value) && encoded_target.has_value() && std::holds_alternative<EncodedLog>(*encoded_target)) {
                            // Direct comparison of EncodedLog structures
                            is_match = (std::get<EncodedLog>(current_value) == std::get<EncodedLog>(*encoded_target));
                            
                        }
                        break;
                    }

                }
                
                if (is_match) {
                    match_count++;
                    matched_indices.push_back(i); // Store the index of the match
                    // For demonstration, we'll add a record with the index
                    matched_records.push_back("{\"field\": \"" + field_name + "\", \"index\": " + std::to_string(i) + 
                                            ", \"value\": \"" + target_value + "\", \"op\": \"" + comparison_op + "\"}");
                }
                
            } catch (const std::out_of_range& e) {
                // Reached end of layer data
                break;
            } catch (const std::exception& e) {
                // Continue with next index in case of other errors
                continue;
            }
        }
         
        result.match_found = (match_count > 0);
        result.count = match_count;
        result.matched_records = matched_records;
        result.matched_indices = matched_indices; // Store matched indices directly in result
        
    } catch (const std::exception& e) {
        result.error_message = "Error filtering layer values: " + std::string(e.what());
    }
    
    return result;
}

// Implementation of filterLayerValuesInRange for efficient range queries
json2::query::ValueFilterResult QueryEngine::filterLayerValuesInRange(
    const std::string& field_name,
    FieldType field_type,
    const std::string& min_value,
    const std::string& max_value,
    const GranularCompressedData& granular_data,
    const FieldDictionaryManager* dict_manager) {
    
    ValueFilterResult result;
    result.match_found = false;
    result.count = 0;
    
    // Only support numeric types for range filtering
    if (field_type != FieldType::INT64 && field_type != FieldType::DOUBLE && field_type != FieldType::BOOL) {
        result.error_message = "Range filtering only supported for numeric types (INT64, DOUBLE, BOOL)";
        return result;
    }
    
    try {
        // Get the field index from metadata
        if (granular_data.metadata.empty()) {
            result.error_message = "Metadata is empty";
            return result;
        }
        
        // Decompress metadata using the proper type-aware decompression function
        std::vector<uint8_t> metadata_raw = decompressWithConfigWrapper(granular_data.metadata, 
                                                                      compression::FieldType::STRING, 
                                                                      DEFAULT_COMPRESSION_CONFIG);
        
        if (metadata_raw.empty()) {
            result.error_message = "Failed to decompress metadata";
            return result;
        }
        
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        
        // Get the field index
        int field_index = -1;
        for (size_t i = 0; i < field_order.size(); ++i) {
            if (field_order[i].name == field_name) {
                field_index = static_cast<int>(i);
                break;
            }
        }
        
        if (field_index == -1) {
            result.error_message = "Field not found in metadata: " + field_name;
            return result;
        }
        
        // Map field type to compression type
        compression::FieldType compression_field_type;
        switch (field_type) {
            case FieldType::INT64:
                compression_field_type = compression::FieldType::INT64;
                break;
            case FieldType::DOUBLE:
                compression_field_type = compression::FieldType::DOUBLE;
                break;
            case FieldType::BOOL:
                compression_field_type = compression::FieldType::BOOL;
                break;
            default:
                result.error_message = "Unsupported field type for layer filtering";
                return result;
        }
        
        // Get the compressed layer data based on storage method
        std::vector<uint8_t> compressed_layer;
        if (granular_data.use_layer_separation) {
            // Layers are stored separately in layer_data_by_level
            if (static_cast<size_t>(field_index) >= granular_data.layer_data_by_level.size()) {
                result.error_message = "Layer data not available for field: " + field_name + 
                                     " (field_index: " + std::to_string(field_index) + 
                                     ", available layers: " + std::to_string(granular_data.layer_data_by_level.size()) + ")";
                return result;
            }
            compressed_layer = granular_data.layer_data_by_level[field_index];
        } else {
            // Layers are stored combined in layer_data_combined
            if (granular_data.layer_data_combined.empty()) {
                result.error_message = "Combined layer data is empty for field: " + field_name;
                return result;
            }
            
            // For combined storage, we need to extract the specific layer
            // This requires parsing the combined data structure
            try {
                std::istringstream layer_stream(std::string(granular_data.layer_data_combined.begin(), 
                                                           granular_data.layer_data_combined.end()));
                
                // Read layer count
                uint32_t layer_count;
                layer_stream.read(reinterpret_cast<char*>(&layer_count), sizeof(layer_count));
                
                if (static_cast<size_t>(field_index) >= layer_count) {
                    result.error_message = "Layer index out of range in combined data: " + field_name;
                    return result;
                }
                
                // Read layer sizes
                std::vector<uint32_t> layer_sizes(layer_count);
                if (!layer_stream.read(reinterpret_cast<char*>(layer_sizes.data()), 
                                      layer_count * sizeof(uint32_t))) {
                    result.error_message = "Failed to read layer sizes from combined data";
                    return result;
                }
                
                // Skip to the target layer
                size_t offset = sizeof(layer_count) + layer_count * sizeof(uint32_t);
                for (int i = 0; i < field_index; ++i) {
                    offset += layer_sizes[i];
                }
                
                // Read the target layer data
                if (offset + layer_sizes[field_index] > granular_data.layer_data_combined.size()) {
                    result.error_message = "Layer data size mismatch in combined data";
                    return result;
                }
                
                compressed_layer = std::vector<uint8_t>(
                    granular_data.layer_data_combined.begin() + offset,
                    granular_data.layer_data_combined.begin() + offset + layer_sizes[field_index]);
            } catch (const std::exception& e) {
                result.error_message = "Failed to extract layer from combined data: " + std::string(e.what());
                return result;
            }
        }
        
        if (compressed_layer.empty()) {
            result.error_message = "Compressed layer data is empty for field: " + field_name;
            return result;
        }
        
        // Create selective decompressor
        SelectiveDecompressor decompressor;
        
        // Determine layer size for decompression
        size_t layer_size = 0;
        if (!granular_data.layer_sizes.empty()) {
            try {
                std::vector<uint8_t> layer_sizes_raw = decompressWithConfigWrapper(
                    granular_data.layer_sizes, compression::FieldType::STRING, DEFAULT_COMPRESSION_CONFIG);
                if (layer_sizes_raw.size() >= sizeof(uint32_t) * (field_index + 1)) {
                    const uint32_t* layer_sizes_data = reinterpret_cast<const uint32_t*>(layer_sizes_raw.data());
                    layer_size = layer_sizes_data[field_index];
                }
            } catch (...) {
                // If we can't get layer size, we'll estimate it during decompression
            }
        }
        
        // Perform accurate filtering by iterating through layer values in a single pass
        size_t match_count = 0;
        std::vector<std::string> matched_records;
        std::vector<size_t> matched_indices; // Store matched indices
        
        // Parse target values based on field type
        NodeValue min_node_value;
        NodeValue max_node_value;
        try {
            switch (field_type) {
                case FieldType::INT64: {
                    int64_t min_val = std::stoll(min_value);
                    int64_t max_val = std::stoll(max_value);
                    min_node_value = min_val;
                    max_node_value = max_val;
                    break;
                }
                case FieldType::DOUBLE: {
                    double min_val = std::stod(min_value);
                    double max_val = std::stod(max_value);
                    min_node_value = min_val;
                    max_node_value = max_val;
                    break;
                }
                case FieldType::BOOL: {
                    bool min_val = (min_value == "true");
                    bool max_val = (max_value == "true");
                    min_node_value = min_val;
                    max_node_value = max_val;
                    break;
                }
                default:
                    result.error_message = "Unsupported field type for value parsing";
                    return result;
            }
        } catch (const std::exception& e) {
            result.error_message = "Failed to parse target values: " + std::string(e.what());
            return result;
        }

        // Use the actual layer size if available, otherwise use a reasonable default
        size_t max_values_to_check = layer_size > 0 ? layer_size : 100000; // Increased limit for better accuracy;
        
        // For all types, we'll iterate with bounds checking
        for (size_t i = 0; i < max_values_to_check; ++i) {
            try {
                // Try to get the value at index i
                NodeValue current_value;
                
                // Use direct random access for all field types
                current_value = decompressor.decompressLayerValueAt(
                    compressed_layer, i, compression_field_type, DEFAULT_COMPRESSION_CONFIG, layer_size);
                
                // Compare with target value based on comparison operator
                bool is_match = false;
                
                // Perform range comparison based on the actual types
                if (std::holds_alternative<int64_t>(current_value) && std::holds_alternative<int64_t>(min_node_value) && std::holds_alternative<int64_t>(max_node_value)) {
                    int64_t current_val = std::get<int64_t>(current_value);
                    int64_t min_val = std::get<int64_t>(min_node_value);
                    int64_t max_val = std::get<int64_t>(max_node_value);
                    is_match = (current_val >= min_val && current_val <= max_val);
                } else if (std::holds_alternative<double>(current_value) && std::holds_alternative<double>(min_node_value) && std::holds_alternative<double>(max_node_value)) {
                    double current_val = std::get<double>(current_value);
                    double min_val = std::get<double>(min_node_value);
                    double max_val = std::get<double>(max_node_value);
                    is_match = (current_val >= min_val && current_val <= max_val);
                } else if (std::holds_alternative<bool>(current_value) && std::holds_alternative<bool>(min_node_value) && std::holds_alternative<bool>(max_node_value)) {
                    int current_val = static_cast<int>(std::get<bool>(current_value));
                    int min_val = static_cast<int>(std::get<bool>(min_node_value));
                    int max_val = static_cast<int>(std::get<bool>(max_node_value));
                    is_match = (current_val >= min_val && current_val <= max_val);
                }
                
                if (is_match) {
                    match_count++;
                    matched_indices.push_back(i); // Store the index of the match
                    // For demonstration, we'll add a record with the index
                    matched_records.push_back("{\"field\": \"" + field_name + "\", \"index\": " + std::to_string(i) + 
                                            ", \"min_value\": \"" + min_value + "\", \"max_value\": \"" + max_value + "\"}");
                }
                
            } catch (const std::out_of_range& e) {
                // Reached end of layer data
                break;
            } catch (const std::exception& e) {
                // Continue with next index in case of other errors
                continue;
            }
        }
         
        result.match_found = (match_count > 0);
        result.count = match_count;
        result.matched_records = matched_records;
        result.matched_indices = matched_indices; // Store matched indices directly in result
        
    } catch (const std::exception& e) {
        result.error_message = "Error filtering layer values in range: " + std::string(e.what());
    }
    
    return result;
}

// Implementation of filterFieldAndValues
json2::query::ValueFilterResult QueryEngine::filterFieldAndValues(
    const std::string& field_name,
    FieldType expected_type,
    const std::string& target_value,
    const std::string& comparison_op,
    const GranularCompressedData& granular_data) {
    
    // First check field existence and type
    auto field_result = checkFieldExistenceAndType(field_name, expected_type, granular_data);
    
    if (!field_result.exists) {
        ValueFilterResult result;
        result.match_found = false;
        result.error_message = "Field does not exist: " + field_name;
        return result;
    }
    
    if (!field_result.type_matches) {
        ValueFilterResult result;
        result.match_found = false;
        result.error_message = "Field type mismatch for: " + field_name;
        return result;
    }
    
    // Then filter values based on field type
    return filterLayerValues(field_name, expected_type, target_value, comparison_op, granular_data);
}

// Helper functions to merge RecordQueryResult objects from multiple blocks
void QueryEngine::mergeRecordQueryResults(RecordQueryResult& target, const RecordQueryResult& source) const {
    // Append records from source to target
    target.records.insert(target.records.end(), source.records.begin(), source.records.end());
    
    // Update counts
    target.count += source.count;
    target.chunks_accessed += source.chunks_accessed;
    
    // Update decompression ratio (weighted average)
    if (target.chunks_accessed + source.chunks_accessed > 0) {
        target.decompression_ratio = (target.decompression_ratio * target.chunks_accessed + 
                                     source.decompression_ratio * source.chunks_accessed) / 
                                     (target.chunks_accessed + source.chunks_accessed);
    }
    
    // Update query time
    target.query_time_ms += source.query_time_ms;
    
    // Update completion status - only complete if all sources are complete and we haven't hit limits
    target.is_complete = target.is_complete && source.is_complete;
    
    // Propagate error message if source has one and target doesn't
    // Only propagate error if no records were found in the source
    if (!source.error_message.empty() && target.error_message.empty() && source.count == 0) {
        target.error_message = source.error_message;
    }
}

// Helper function to extract granular data from a chunk
GranularCompressedData QueryEngine::extractGranularDataFromChunk(const ChunkedTypeAwareBlock& chunk, size_t chunk_index) const {
    GranularCompressedData gdata;
    
    // If data directory is not set, return empty data
    if (data_dir_.empty()) {
        return gdata;
    }
    
    try {
        // Construct the block directory path
        char chunk_dir_buf[256];
        snprintf(chunk_dir_buf, sizeof(chunk_dir_buf), "%s/chunks/chunk_%06zu", data_dir_.c_str(), chunk_index);
        std::string block_dir = chunk_dir_buf;
        
        if (!std::filesystem::exists(block_dir)) {
            return gdata;
        }
        
        // 读取块元数据
        std::ifstream block_metadata(block_dir + "/block_metadata.json2", std::ios::binary);
        if (block_metadata.is_open()) {
            size_t original_size;
            double placeholder_ratio;
            uint32_t config_compression_level;
            uint8_t flags;
            
            block_metadata.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
            block_metadata.read(reinterpret_cast<char*>(&placeholder_ratio), sizeof(placeholder_ratio));
            block_metadata.read(reinterpret_cast<char*>(&config_compression_level), sizeof(config_compression_level));
            block_metadata.read(reinterpret_cast<char*>(&flags), sizeof(flags));
            block_metadata.close();
            
            gdata.original_size = original_size;
            gdata.use_layer_separation = (flags & 2) != 0;
        }
        
        // 读取Trie位图
        std::ifstream trie_file(block_dir + "/louds.json2", std::ios::binary);
        if (trie_file.is_open()) {
            uint32_t trie_size;
            trie_file.read(reinterpret_cast<char*>(&trie_size), sizeof(trie_size));
            gdata.trie_bitmap.resize(trie_size);
            trie_file.read(reinterpret_cast<char*>(gdata.trie_bitmap.data()), trie_size);
            trie_file.close();
        }
        
        // 读取字典数据
        std::string dict_dir = block_dir + "/dictionaries";
        
        // 字符串字典
        std::ifstream string_file(dict_dir + "/variables.json2", std::ios::binary);
        if (string_file.is_open()) {
            uint32_t string_size;
            string_file.read(reinterpret_cast<char*>(&string_size), sizeof(string_size));
            gdata.string_dict.resize(string_size);
            string_file.read(reinterpret_cast<char*>(gdata.string_dict.data()), string_size);
            string_file.close();
        }
        
        // 时间戳字典
        std::ifstream ts_file(dict_dir + "/timestamps.json2", std::ios::binary);
        if (ts_file.is_open()) {
            uint32_t ts_size;
            ts_file.read(reinterpret_cast<char*>(&ts_size), sizeof(ts_size));
            gdata.timestamp_dict.resize(ts_size);
            ts_file.read(reinterpret_cast<char*>(gdata.timestamp_dict.data()), ts_size);
            ts_file.close();
        }
        
        // LogType字典
        std::ifstream log_file(dict_dir + "/logtypes.json2", std::ios::binary);
        if (log_file.is_open()) {
            uint32_t log_size;
            log_file.read(reinterpret_cast<char*>(&log_size), sizeof(log_size));
            gdata.logtype_dict.resize(log_size);
            log_file.read(reinterpret_cast<char*>(gdata.logtype_dict.data()), log_size);
            log_file.close();
        }
        
        // 细粒度压缩的元数据文件
        std::ifstream granular_metadata_file(block_dir + "/metadata.json2", std::ios::binary);
        if (granular_metadata_file.is_open()) {
            uint32_t metadata_size;
            granular_metadata_file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
            gdata.metadata.resize(metadata_size);
            granular_metadata_file.read(reinterpret_cast<char*>(gdata.metadata.data()), metadata_size);
            granular_metadata_file.close();
        }
        
        // 读取层大小信息（如果存在）
        std::ifstream layer_sizes_file(block_dir + "/layer_sizes.json2", std::ios::binary);
        if (layer_sizes_file.is_open()) {
            uint32_t layer_sizes_size;
            layer_sizes_file.read(reinterpret_cast<char*>(&layer_sizes_size), sizeof(layer_sizes_size));
            gdata.layer_sizes.resize(layer_sizes_size);
            layer_sizes_file.read(reinterpret_cast<char*>(gdata.layer_sizes.data()), layer_sizes_size);
            layer_sizes_file.close();
        }
        
        // 读取层数据
        if (gdata.use_layer_separation) {
            // 按层分别读取
            size_t layer_idx = 0;
            while (true) {
                char layer_file_buf[256];
                snprintf(layer_file_buf, sizeof(layer_file_buf), "%s/layer_%zu.json2", block_dir.c_str(), layer_idx);
                std::ifstream layer_stream(layer_file_buf, std::ios::binary);
                if (!layer_stream.is_open()) {
                    break;
                }
                uint32_t layer_size;
                if (!layer_stream.read(reinterpret_cast<char*>(&layer_size), sizeof(layer_size))) {
                    break;
                }
                std::vector<uint8_t> layer_data(layer_size);
                if (!layer_stream.read(reinterpret_cast<char*>(layer_data.data()), layer_size)) {
                    break;
                }
                gdata.layer_data_by_level.push_back(std::move(layer_data));
                layer_stream.close();
                layer_idx++;
            }
        } else {
            // 整体读取
            std::ifstream layers_file(block_dir + "/layers.json2", std::ios::binary);
            if (layers_file.is_open()) {
                uint32_t layers_size;
                layers_file.read(reinterpret_cast<char*>(&layers_size), sizeof(layers_size));
                gdata.layer_data_combined.resize(layers_size);
                layers_file.read(reinterpret_cast<char*>(gdata.layer_data_combined.data()), layers_size);
                layers_file.close();
            }
        }
        
        // Calculate compressed size
        gdata.compressed_size = 0;
        gdata.compressed_size += gdata.trie_bitmap.size();
        gdata.compressed_size += gdata.string_dict.size();
        gdata.compressed_size += gdata.timestamp_dict.size();
        gdata.compressed_size += gdata.logtype_dict.size();
        
        if (gdata.use_layer_separation) {
            for (const auto& layer_data : gdata.layer_data_by_level) {
                gdata.compressed_size += layer_data.size();
            }
        } else {
            gdata.compressed_size += gdata.layer_data_combined.size();
        }
        
        gdata.compressed_size += gdata.layer_sizes.size();
        gdata.compressed_size += gdata.metadata.size();
        
    } catch (const std::exception& e) {
        // If there's an error extracting data, return empty granular data
        GranularCompressedData empty_data;
        return empty_data;
    }
    
    return gdata;
}

// Helper function to convert a path to a JSON string representation
std::string QueryEngine::pathToJSONString(const std::vector<std::string>& path, 
                                         const std::vector<FieldKey>& field_order) const {
    std::string record = "{";
    bool first_field = true;
    
    for (size_t i = 0; i < std::min(path.size(), field_order.size()); ++i) {
        // Skip fields with "null" values
        if (path[i] == "null") {
            continue;
        }
        
        if (!first_field) record += ", ";
        first_field = false;
        record += "\"" + field_order[i].name + "\": ";
        
        // Add quotes for string types
        bool needs_quotes = (field_order[i].type == FieldType::STRING || 
                           field_order[i].type == FieldType::TIMESTAMP || 
                           field_order[i].type == FieldType::LOGTYPE ||
                           field_order[i].type == FieldType::ARRAY);
        if (needs_quotes) {
            record += "\"" + path[i] + "\"";
        } else {
            record += path[i];
        }
    }
    
    record += "}";
    return record;
}

// Helper function to map json2::FieldType to compression::FieldType
compression::FieldType QueryEngine::mapFieldTypeToCompressionType(FieldType json_field_type) const {
    switch (json_field_type) {
        case FieldType::INT64:
            return compression::FieldType::INT64;
        case FieldType::DOUBLE:
            return compression::FieldType::DOUBLE;
        case FieldType::BOOL:
            return compression::FieldType::BOOL;
        case FieldType::STRING:
            return compression::FieldType::STRING;
        case FieldType::TIMESTAMP:
            return compression::FieldType::TIMESTAMP;
        case FieldType::LOGTYPE:
            return compression::FieldType::LOGTYPE;
        case FieldType::ARRAY:
            return compression::FieldType::ARRAY;
        default:
            return compression::FieldType::STRING; // Default fallback
    }
}

} // namespace query
} // namespace json2