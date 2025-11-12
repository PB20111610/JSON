#include "../include/query/query_engine.h"
#include "../include/query/selective_decompressor.h"
#include "../include/field_dictionary_manager.h"
#include "../include/timestamp_dictionary.h"
#include "../include/logtype_dictionary.h"
#include "../test/test_config_utils.h"
#include "../include/compression/type_aware/type_aware_compressor.h"
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
    const GranularCompressedData& granular_data) {
    
    FieldFilterResult result;
    result.exists = false;
    result.type_matches = false;
    result.actual_type = FieldType::STRING; // Default value
    
    try {
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
        
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        
        // Step 2: Check if field exists and matches type
        for (const auto& field_key : field_order) {
            if (field_key.name == field_name) {
                result.exists = true;
                result.actual_type = field_key.type;
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
    }
    
    return result;
}

// Implementation of filterLayerValues
json2::query::ValueFilterResult QueryEngine::filterLayerValues(
    const std::string& field_name,
    FieldType field_type,
    const std::string& target_value,
    const std::string& comparison_op,
    const GranularCompressedData& granular_data) {
    
    ValueFilterResult result;
    result.match_found = false;
    result.count = 0;
    
    try {
        // Debug output for function entry
        std::cout << "DEBUG: filterLayerValues called for field '" << field_name 
                  << "' with type " << static_cast<int>(field_type) << std::endl;
        
        // Get the field index from metadata
        if (granular_data.metadata.empty()) {
            result.error_message = "Metadata is empty";
            std::cout << "DEBUG: Metadata is empty" << std::endl;
            return result;
        }
        
        // Decompress metadata using the proper type-aware decompression function
        std::vector<uint8_t> metadata_raw = decompressWithConfigWrapper(granular_data.metadata, 
                                                                      compression::FieldType::STRING, 
                                                                      DEFAULT_COMPRESSION_CONFIG);
        
        if (metadata_raw.empty()) {
            result.error_message = "Failed to decompress metadata";
            std::cout << "DEBUG: Failed to decompress metadata" << std::endl;
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
            std::cout << "DEBUG: Field not found in metadata: " << field_name << std::endl;
            return result;
        }
        
        std::cout << "DEBUG: Field '" << field_name << "' found at index " << field_index 
                  << " with type " << static_cast<int>(field_order[field_index].type) << std::endl;
        
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
                std::cout << "DEBUG: Unsupported field type: " << static_cast<int>(field_type) << std::endl;
                return result;
        }
        
        std::cout << "DEBUG: Mapped to compression field type " << static_cast<int>(compression_field_type) << std::endl;
        
        // Get the compressed layer data based on storage method
        std::vector<uint8_t> compressed_layer;
        if (granular_data.use_layer_separation) {
            // Layers are stored separately in layer_data_by_level
            if (static_cast<size_t>(field_index) >= granular_data.layer_data_by_level.size()) {
                result.error_message = "Layer data not available for field: " + field_name + 
                                     " (field_index: " + std::to_string(field_index) + 
                                     ", available layers: " + std::to_string(granular_data.layer_data_by_level.size()) + ")";
                std::cout << "DEBUG: Layer data not available for field: " << field_name << std::endl;
                return result;
            }
            compressed_layer = granular_data.layer_data_by_level[field_index];
        } else {
            // Layers are stored combined in layer_data_combined
            if (granular_data.layer_data_combined.empty()) {
                result.error_message = "Combined layer data is empty for field: " + field_name;
                std::cout << "DEBUG: Combined layer data is empty for field: " << field_name << std::endl;
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
                    std::cout << "DEBUG: Layer index out of range in combined data: " << field_name << std::endl;
                    return result;
                }
                
                // Read layer sizes
                std::vector<uint32_t> layer_sizes(layer_count);
                if (!layer_stream.read(reinterpret_cast<char*>(layer_sizes.data()), 
                                      layer_count * sizeof(uint32_t))) {
                    result.error_message = "Failed to read layer sizes from combined data";
                    std::cout << "DEBUG: Failed to read layer sizes from combined data" << std::endl;
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
                    std::cout << "DEBUG: Layer data size mismatch in combined data" << std::endl;
                    return result;
                }
                
                compressed_layer = std::vector<uint8_t>(
                    granular_data.layer_data_combined.begin() + offset,
                    granular_data.layer_data_combined.begin() + offset + layer_sizes[field_index]);
            } catch (const std::exception& e) {
                result.error_message = "Failed to extract layer from combined data: " + std::string(e.what());
                std::cout << "DEBUG: Failed to extract layer from combined data: " << e.what() << std::endl;
                return result;
            }
        }
        
        if (compressed_layer.empty()) {
            result.error_message = "Compressed layer data is empty for field: " + field_name;
            std::cout << "DEBUG: Compressed layer data is empty for field: " << field_name << std::endl;
            return result;
        }
        
        std::cout << "DEBUG: Successfully extracted compressed layer data, size: " << compressed_layer.size() << std::endl;
        
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
        
        std::cout << "DEBUG: Layer size determined as: " << layer_size << std::endl;
        
        // Perform accurate filtering by iterating through layer values
        size_t match_count = 0;
        std::vector<std::string> matched_records;
        std::vector<size_t> matched_indices; // Store matched indices
        
        // For dictionary-encoded fields, encode the target value once before the loop
        std::optional<NodeValue> encoded_target = encodeTargetValueWithDictionary(target_value, field_type, granular_data);
        
        if (field_type == FieldType::TIMESTAMP || field_type == FieldType::LOGTYPE) {
            if (encoded_target.has_value()) {
                std::cout << "DEBUG: Successfully encoded target value for field type " << static_cast<int>(field_type) << std::endl;
                // Additional debug output for TIMESTAMP to show encoding type
                if (field_type == FieldType::TIMESTAMP) {
                    if (std::holds_alternative<TemplateEncodedTimestamp>(*encoded_target)) {
                        std::cout << "DEBUG: TIMESTAMP encoded as TemplateEncodedTimestamp (template format)" << std::endl;
                    }
                }
                // Additional debug output for LOGTYPE to show encoding type
                if (field_type == FieldType::LOGTYPE) {
                    if (std::holds_alternative<EncodedLog>(*encoded_target)) {
                        std::cout << "DEBUG: LOGTYPE encoded as EncodedLog (template format)" << std::endl;
                    }
                }
            } else {
                std::cout << "DEBUG: Failed to encode target value for field type " << static_cast<int>(field_type) << std::endl;
            }
        }
        
        // Use the actual layer size if available, otherwise use a reasonable default
        size_t max_values_to_check = layer_size > 0 ? layer_size : 100000; // Increased limit for better accuracy
        
        std::cout << "DEBUG: Starting iteration through layer values, max_values_to_check: " << max_values_to_check << std::endl;
        
        // For all types, we'll iterate with bounds checking
        for (size_t i = 0; i < max_values_to_check; ++i) {
            try {
                // Try to get the value at index i
                // For TIMESTAMP and LOGTYPE fields, use direct random access instead of the non-existent decompressLayerValueAtAsInt64s
                NodeValue current_value;
                
                // Use direct random access for all field types
                current_value = decompressor.decompressLayerValueAt(
                    compressed_layer, i, compression_field_type, DEFAULT_COMPRESSION_CONFIG, layer_size);
                
                // Debug output for timestamp and logtype fields
                if (field_type == FieldType::TIMESTAMP || field_type == FieldType::LOGTYPE) {
                    std::cout << "DEBUG: Field '" << field_name << "' at index " << i << " - ";
                    if (field_type == FieldType::TIMESTAMP && std::holds_alternative<TemplateEncodedTimestamp>(current_value)) {
                        const auto& ts_value = std::get<TemplateEncodedTimestamp>(current_value);
                        std::cout << "Timestamp value: template_id=" << ts_value.template_id;
                        std::cout << ", var_codes=[";
                        for (size_t j = 0; j < ts_value.var_codes.size(); ++j) {
                            if (j > 0) std::cout << ",";
                            std::cout << ts_value.var_codes[j];
                        }
                        std::cout << "]" << std::endl;
                    } else if (field_type == FieldType::LOGTYPE && std::holds_alternative<EncodedLog>(current_value)) {
                        const auto& log_value = std::get<EncodedLog>(current_value);
                        std::cout << "LogType value: template_id=" << log_value.template_id;
                        std::cout << ", var_codes=[";
                        for (size_t j = 0; j < log_value.var_codes.size(); ++j) {
                            if (j > 0) std::cout << ",";
                            std::cout << log_value.var_codes[j];
                        }
                        std::cout << "]" << std::endl;
                    } else {
                        std::cout << "Unknown value type or variant mismatch" << std::endl;
                    }
                    
                    // If we have an encoded target, also show its value
                    if (encoded_target.has_value()) {
                        if (field_type == FieldType::TIMESTAMP && std::holds_alternative<TemplateEncodedTimestamp>(*encoded_target)) {
                            const auto& ts_value = std::get<TemplateEncodedTimestamp>(*encoded_target);
                            std::cout << "DEBUG: Target Timestamp value: template_id=" << ts_value.template_id;
                            std::cout << ", var_codes=[";
                            for (size_t j = 0; j < ts_value.var_codes.size(); ++j) {
                                if (j > 0) std::cout << ",";
                                std::cout << ts_value.var_codes[j];
                            }
                            std::cout << "]" << std::endl;
                        } else if (field_type == FieldType::LOGTYPE && std::holds_alternative<EncodedLog>(*encoded_target)) {
                            const auto& log_value = std::get<EncodedLog>(*encoded_target);
                            std::cout << "DEBUG: Target LogType value: template_id=" << log_value.template_id;
                            std::cout << ", var_codes=[";
                            for (size_t j = 0; j < log_value.var_codes.size(); ++j) {
                                if (j > 0) std::cout << ",";
                                std::cout << log_value.var_codes[j];
                            }
                            std::cout << "]" << std::endl;
                        }
                    }
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
                            
                            std::cout << "DEBUG: Detailed TIMESTAMP comparison at index " << i << std::endl;
                            std::cout << "DEBUG: Comparison result: " << (is_match ? "MATCH" : "NO MATCH") << std::endl;
                        }
                        break;
                    }
                    case FieldType::LOGTYPE: {
                        // For LogType fields, use encoded value comparison for better performance
                        if (std::holds_alternative<EncodedLog>(current_value) && encoded_target.has_value() && std::holds_alternative<EncodedLog>(*encoded_target)) {
                            // Direct comparison of EncodedLog structures
                            is_match = (std::get<EncodedLog>(current_value) == std::get<EncodedLog>(*encoded_target));
                            
                            std::cout << "DEBUG: Detailed LOGTYPE comparison at index " << i << std::endl;
                            std::cout << "DEBUG: Comparison result: " << (is_match ? "MATCH" : "NO MATCH") << std::endl;
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
                std::cout << "DEBUG: Reached end of layer data at index " << i << std::endl;
                break;
            } catch (const std::exception& e) {
                // Continue with next index in case of other errors
                std::cout << "DEBUG: Exception at index " << i << ": " << e.what() << std::endl;
                continue;
            }
        }
         
        std::cout << "DEBUG: Finished iteration, match_count: " << match_count << std::endl;
        
        result.match_found = (match_count > 0);
        result.count = match_count;
        result.matched_records = matched_records;
        result.matched_indices = matched_indices; // Store matched indices directly in result
        
    } catch (const std::exception& e) {
        result.error_message = "Error filtering layer values: " + std::string(e.what());
        std::cout << "DEBUG: Exception in filterLayerValues: " << e.what() << std::endl;
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

} // namespace query
} // namespace json2