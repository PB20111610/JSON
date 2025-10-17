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
#include <filesystem>
#include <set>

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

void QueryEngine::setDataDirectory(const std::string& data_dir) {
    data_dir_ = data_dir;
}

// ========== 字段存在性和字典查询方法 ==========

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
                if (!granular_data.string_dict.empty()) {
                    // Decompress using the proper type-aware decompression function
                    dict_data = decompressWithConfig(granular_data.string_dict, 
                                                   compression::FieldType::STRING, config);
                    dict_available = true;
                }
                break;
            case FieldType::Timestamp:
                if (!granular_data.timestamp_dict.empty()) {
                    // Decompress using the proper type-aware decompression function
                    dict_data = decompressWithConfig(granular_data.timestamp_dict, 
                                                   compression::FieldType::TIMESTAMP, config);
                    dict_available = true;
                }
                break;
            case FieldType::LogType:
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
    }
    
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

// Helper function to check if a value exists in a dictionary without full decompression
bool QueryEngine::checkValueInDictionary(const std::string& field_name,
                                       FieldType field_type,
                                       const std::string& target_value,
                                       const GranularCompressedData& granular_data) const {
    // For numeric types (Int, Double, Bool), skip this optimization since they don't use dictionaries
    if (field_type == FieldType::Int || field_type == FieldType::Double || field_type == FieldType::Bool) {
        return true; // These types store raw values, no dictionary needed
    }
    
    // Only check dictionaries for string-based types
    if (field_type != FieldType::String && field_type != FieldType::Timestamp && field_type != FieldType::LogType) {
        return true; // For other types, assume value exists
    }
    
    try {
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
        
        // Determine which dictionary to check based on field type
        const std::vector<uint8_t>* dict_data = nullptr;
        compression::FieldType compression_field_type;
        
        switch (field_type) {
            case FieldType::String:
                dict_data = &granular_data.string_dict;
                compression_field_type = compression::FieldType::STRING;
                break;
            case FieldType::Timestamp:
                dict_data = &granular_data.timestamp_dict;
                compression_field_type = compression::FieldType::TIMESTAMP;
                break;
            case FieldType::LogType:
                dict_data = &granular_data.logtype_dict;
                compression_field_type = compression::FieldType::LOGTYPE;
                break;
            default:
                return true; // For other types, assume value exists
        }
        
        // If dictionary is empty, value can't exist
        if (dict_data->empty()) {
            return false;
        }
        
        // Partially decompress the dictionary to check if the value exists
        // For string values, we can check the global string dictionary
        if (field_type == FieldType::String) {
            // Decompress using the proper type-aware decompression function
            std::vector<uint8_t> decompressed_dict = decompressWithConfig(*dict_data, compression_field_type, config);
            
            // Create a temporary dictionary manager to check if value exists
            FieldDictionaryManager temp_dict_manager;
            Compressor::deserializeStringDictionary(decompressed_dict, temp_dict_manager);
            
            // Check if the value exists in the dictionary
            // This is a simplified check - in a real implementation, we would need to
            // search through the dictionary more efficiently
            auto& var_dict = temp_dict_manager.variableDict();
            std::vector<std::string> all_values = var_dict.getAllStringValues();
            
            // Check if target value exists in the dictionary
            for (const auto& value : all_values) {
                if (value == target_value) {
                    return true;
                }
            }
            
            return false;
        }
        
        // For other dictionary types, we assume the value might exist
        // A more sophisticated implementation would check the specific dictionary format
        return true;
        
    } catch (const std::exception& e) {
        // If there's an error checking the dictionary, assume the value might exist
        return true;
    }
}

// 新增函数：解码节点值为数值
double QueryEngine::decodeNodeValueToDouble(
    const NodeValue& node_value,
    const FieldKey& field_key,
    const FieldDictionaryManager& dict_manager) const {
    
    try {
        // First decode to string
        std::string str_value = decodeNodeValueToString(node_value, field_key, dict_manager);
        
        // Convert to double based on field type
        switch (field_key.type) {
            case FieldType::Int:
                return static_cast<double>(std::stoll(str_value));
            case FieldType::Double:
                return std::stod(str_value);
            case FieldType::Bool:
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

// ========== 聚合查询实现 ==========

AggregateQueryResult QueryEngine::executeAggregateQuery(
    AggregateFunction aggregate_func,
    const std::string& field_name,
    const GranularCompressedData& granular_data) {
    
    AggregateQueryResult result;
    result.value = 0.0;
    result.count = 0;
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
        result.field_name = field_name;
        
        // Step 1: Decompress the granular data to get the trie and dictionary manager
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
        
        // Step 2: Set up decompression options to load all necessary components
        Compressor::PartialDecompressionOptions decompress_options;
        decompress_options.load_trie = true;
        decompress_options.load_layers = true;
        decompress_options.load_string_dict = true;
        decompress_options.load_timestamp_dict = true;
        decompress_options.load_logtype_dict = true;
        decompress_options.load_metadata = true;
        
        // Step 3: Decompress the granular data
        // We need to directly decompress to LOUDSTrie instead of Trie to access layer information
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
            
            // Load dictionaries
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
        
        // Step 4: Get field order
        const auto& field_order = louds_trie->getFieldOrder();
        
        // For COUNT with empty field name, count all records (COUNT *)
        if (aggregate_func == AggregateFunction::COUNT && field_name.empty()) {
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
        
        // Step 5: For all other cases, find the target field
        int field_index = getFieldIndex(field_name, field_order);
        
        if (field_index == -1) {
            result.error_message = "Field '" + field_name + "' not found in the data";
            return result;
        }
        
        // Step 6: Determine the field type
        FieldType target_field_type = field_order[field_index].type;
        FieldKey target_field_key = field_order[field_index];
        
        // Step 7: Search through all nodes in the trie to find matching paths
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
                    
                    // Since we've already decompressed all content, we can directly decode the value
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
                    
                    // Since we've already decompressed all content, we can directly decode the value
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

// ========== 分组聚合查询实现 ==========

GroupedAggregateQueryResult QueryEngine::executeGroupedAggregateQuery(
    const std::vector<AggregateFunction>& aggregate_funcs,
    const std::vector<std::string>& aggregate_fields,
    const std::vector<std::string>& group_fields,
    const GranularCompressedData& granular_data) {
    
    GroupedAggregateQueryResult result;
    result.groups_count = 0;
    result.total_count = 0;
    result.chunks_accessed = 0;
    result.is_complete = false;
    
    try {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Set group fields in result
        result.group_fields = group_fields;
        result.aggregate_fields = aggregate_fields;
        
        // Step 1: Decompress the granular data to get the trie and dictionary manager
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
        
        // Step 2: Set up decompression options to load all necessary components
        Compressor::PartialDecompressionOptions decompress_options;
        decompress_options.load_trie = true;
        decompress_options.load_layers = true;
        decompress_options.load_string_dict = true;
        decompress_options.load_timestamp_dict = true;
        decompress_options.load_logtype_dict = true;
        decompress_options.load_metadata = true;
        
        // Step 3: Decompress the granular data
        // We need to directly decompress to LOUDSTrie instead of Trie to access layer information
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
            
            // Load dictionaries
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
        
        // Step 4: Get field order
        const auto& field_order = louds_trie->getFieldOrder();
        
        // Step 5: Find indices for group fields and aggregate fields
        std::vector<int> group_field_indices;
        std::vector<int> aggregate_field_indices;
        
        for (const auto& group_field : group_fields) {
            int index = getFieldIndex(group_field, field_order);
            if (index == -1) {
                result.error_message = "Group field '" + group_field + "' not found in the data";
                return result;
            }
            group_field_indices.push_back(index);
        }
        
        for (const auto& aggregate_field : aggregate_fields) {
            // For COUNT(*), we allow empty field name
            if (aggregate_field.empty()) {
                aggregate_field_indices.push_back(-1); // Special case for COUNT(*)
            } else {
                int index = getFieldIndex(aggregate_field, field_order);
                if (index == -1) {
                    result.error_message = "Aggregate field '" + aggregate_field + "' not found in the data";
                    return result;
                }
                aggregate_field_indices.push_back(index);
            }
        }
        
        // Step 6: Group the data by group fields
        // Map to store grouped values: key is group values, value is map of aggregate results
        std::map<std::vector<std::string>, std::map<std::string, std::vector<double>>> grouped_data;
        
        // Step 7: Process all records and group them
        result.chunks_accessed = 1; // We're accessing one chunk
        
        // We need to traverse all complete paths in the trie
        if (louds_trie->layerCount() > 0) {
            size_t last_layer_idx = louds_trie->layerCount() - 1;
            const auto& last_layer = louds_trie->getLayer(last_layer_idx);
            
            // For COUNT with empty field name, count all records (COUNT *)
            if (aggregate_funcs.size() == 1 && aggregate_funcs[0] == AggregateFunction::COUNT && 
                aggregate_fields.size() == 1 && aggregate_fields[0].empty()) {
                // Count all nodes in the last layer (complete records)
                result.total_count = last_layer.size();
                result.is_complete = true;
                
                // For grouped COUNT(*), we still need to group by the specified fields
                // We'll iterate through all records and group them
                // Don't increment total_count in the loop as we set it above
                bool is_count_star = true;
                
                for (size_t node_idx = 0; node_idx < last_layer.size(); ++node_idx) {
                    // Reconstruct the path for this node
                    std::vector<std::string> path_values(field_order.size());
                    bool path_valid = true;
                    
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
                    
                    if (path_valid) {
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
                        
                        // Extract aggregate values
                        std::vector<double> agg_values(aggregate_fields.size());
                        for (size_t i = 0; i < aggregate_fields.size(); ++i) {
                            // Special case for COUNT(*)
                            if (aggregate_field_indices[i] == -1) {
                                agg_values[i] = 1.0; // Each record contributes 1 to COUNT(*)
                            } else if (aggregate_field_indices[i] >= 0 && 
                                       static_cast<size_t>(aggregate_field_indices[i]) < path_values.size()) {
                                // Convert to double for aggregation
                                const std::string& str_value = path_values[aggregate_field_indices[i]];
                                try {
                                    if (!str_value.empty() && str_value != "null") {
                                        // Try to convert to double based on field type
                                        FieldType field_type = field_order[aggregate_field_indices[i]].type;
                                        if (field_type == FieldType::Int || field_type == FieldType::Double) {
                                            agg_values[i] = std::stod(str_value);
                                        } else if (field_type == FieldType::Bool) {
                                            agg_values[i] = (str_value == "true") ? 1.0 : 0.0;
                                        } else {
                                            // For non-numeric types in aggregation, use 0.0 as default
                                            agg_values[i] = 0.0;
                                        }
                                    } else {
                                        agg_values[i] = 0.0;
                                    }
                                } catch (const std::exception&) {
                                    agg_values[i] = 0.0;
                                }
                            } else {
                                agg_values[i] = 0.0;
                            }
                        }
                        
                        // Add to grouped data
                        grouped_data[group_values][aggregate_fields.size() > 0 ? aggregate_fields[0] : "count"].push_back(agg_values.size() > 0 ? agg_values[0] : 1.0);
                        // Don't increment total_count for COUNT(*) as we set it above
                    }
                }
            } else {
                // For non-COUNT(*) queries, process normally
                for (size_t node_idx = 0; node_idx < last_layer.size(); ++node_idx) {
                    // Reconstruct the path for this node
                    std::vector<std::string> path_values(field_order.size());
                    bool path_valid = true;
                    
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
                    
                    if (path_valid) {
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
                        
                        // Extract aggregate values
                        std::vector<double> agg_values(aggregate_fields.size());
                        for (size_t i = 0; i < aggregate_fields.size(); ++i) {
                            // Special case for COUNT(*)
                            if (aggregate_field_indices[i] == -1) {
                                agg_values[i] = 1.0; // Each record contributes 1 to COUNT(*)
                            } else if (aggregate_field_indices[i] >= 0 && 
                                       static_cast<size_t>(aggregate_field_indices[i]) < path_values.size()) {
                                // Convert to double for aggregation
                                const std::string& str_value = path_values[aggregate_field_indices[i]];
                                try {
                                    if (!str_value.empty() && str_value != "null") {
                                        // Try to convert to double based on field type
                                        FieldType field_type = field_order[aggregate_field_indices[i]].type;
                                        if (field_type == FieldType::Int || field_type == FieldType::Double) {
                                            agg_values[i] = std::stod(str_value);
                                        } else if (field_type == FieldType::Bool) {
                                            agg_values[i] = (str_value == "true") ? 1.0 : 0.0;
                                        } else {
                                            // For non-numeric types in aggregation, use 0.0 as default
                                            agg_values[i] = 0.0;
                                        }
                                    } else {
                                        agg_values[i] = 0.0;
                                    }
                                } catch (const std::exception&) {
                                    agg_values[i] = 0.0;
                                }
                            } else {
                                agg_values[i] = 0.0;
                            }
                        }
                        
                        // Add to grouped data
                        grouped_data[group_values][aggregate_fields.size() > 0 ? aggregate_fields[0] : "count"].push_back(agg_values.size() > 0 ? agg_values[0] : 1.0);
                        result.total_count++;
                    }
                }
            }
        }
        
        // Step 8: Calculate aggregate values for each group
        for (auto& group_entry : grouped_data) {
            const std::vector<std::string>& group_values = group_entry.first;
            auto& agg_results = group_entry.second;
            
            std::map<std::string, double> final_agg_values;
            
            // Apply aggregate functions
            for (size_t i = 0; i < std::min(aggregate_funcs.size(), aggregate_fields.size()); ++i) {
                const std::string& agg_field = aggregate_fields[i];
                AggregateFunction agg_func = aggregate_funcs[i];
                const auto& values = agg_results[agg_field];
                
                double agg_result = 0.0;
                
                if (!values.empty()) {
                    switch (agg_func) {
                        case AggregateFunction::COUNT:
                            if (agg_field.empty()) {
                                // COUNT(*) - count all records in this group
                                agg_result = static_cast<double>(values.size());
                            } else {
                                // COUNT(field) - count non-null values
                                size_t count = 0;
                                for (double val : values) {
                                    if (val != 0.0) count++; // Assuming 0.0 represents null/invalid
                                }
                                agg_result = static_cast<double>(count);
                            }
                            break;
                            
                        case AggregateFunction::SUM:
                            for (double val : values) {
                                agg_result += val;
                            }
                            break;
                            
                        case AggregateFunction::AVG: {
                            double sum = 0.0;
                            for (double val : values) {
                                sum += val;
                            }
                            agg_result = sum / values.size();
                            break;
                        }
                        
                        case AggregateFunction::MAX: {
                            double max_val = values[0];
                            for (double val : values) {
                                if (val > max_val) max_val = val;
                            }
                            agg_result = max_val;
                            break;
                        }
                        
                        case AggregateFunction::MIN: {
                            double min_val = values[0];
                            for (double val : values) {
                                if (val < min_val) min_val = val;
                            }
                            agg_result = min_val;
                            break;
                        }
                    }
                }
                
                final_agg_values[agg_field + "_" + std::to_string(static_cast<int>(agg_func))] = agg_result;
            }
            
            result.grouped_values[group_values] = final_agg_values;
        }
        
        result.groups_count = grouped_data.size();
        result.is_complete = true;
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing grouped aggregate query: " + std::string(e.what());
    }
    
    return result;
}

// ========== 精确匹配查询实现 ==========

RecordQueryResult QueryEngine::executeExactMatchQuery(
    const std::string& field_name,
    const std::string& exact_value,
    const GranularCompressedData& granular_data) {
    
    RecordQueryResult result;
    result.count = 0;
    result.chunks_accessed = 0;
    result.decompression_ratio = 0.0;
    result.is_complete = false;
    
    try {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Step 1: Decompress the granular data to get the trie and dictionary manager
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
        
        // Step 2: Set up decompression options to load all necessary components
        Compressor::PartialDecompressionOptions decompress_options;
        decompress_options.load_trie = true;
        decompress_options.load_layers = true;
        decompress_options.load_string_dict = true;
        decompress_options.load_timestamp_dict = true;
        decompress_options.load_logtype_dict = true;
        decompress_options.load_metadata = true;
        
        // Step 3: Decompress the granular data
        // We need to directly decompress to LOUDSTrie instead of Trie to access layer information
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
            
            // Load dictionaries
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
        
        // Step 4: Get field order and find the target field
        const auto& field_order = louds_trie->getFieldOrder();
        int field_index = getFieldIndex(field_name, field_order);
        
        if (field_index == -1) {
            result.error_message = "Field '" + field_name + "' not found in the data";
            return result;
        }
        
        // Step 5: Determine the field type
        FieldType target_field_type = field_order[field_index].type;
        FieldKey target_field_key = field_order[field_index];
        
        // Optimization: Check if the exact value exists in the dictionary before doing full decompression
        // For numeric types (Int, Double, Bool), skip this optimization since they don't use dictionaries
        // if (needsDictionaryDecompression(target_field_type)) {
        //     bool value_exists = checkValueInDictionary(field_name, target_field_type, exact_value, granular_data);
        //     if (!value_exists) {
        //         // If the value doesn't exist in the dictionary, we can return early
        //         result.is_complete = true;
        //         auto end_time = std::chrono::high_resolution_clock::now();
        //         result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        //         return result;
        //     }
        // }
        
        // Step 6: Search through all nodes in the trie to find matching paths
        size_t total_nodes = louds_trie->nodeCount();
        result.chunks_accessed = 1; // We're accessing one chunk
        
        // Store all matching node indices as a set to handle multiple matches in the same layer
        std::vector<std::pair<size_t, size_t>> matching_nodes; // pair of (field_index, node_idx)
        
        // We'll search in the specific layer corresponding to our field
        if (static_cast<size_t>(field_index) < louds_trie->layerCount()) {
            const auto& layer = louds_trie->getLayer(field_index);
            
            for (size_t node_idx = 0; node_idx < layer.size(); ++node_idx) {
                // Get the actual node value directly from the layer
                const NodeValue& node_value = louds_trie->getLayerNodeValue(field_index, node_idx);
                
                // Since we've already decompressed all content, we can directly decode the value
                // Decode the node value to string
                std::string decoded_value = decodeNodeValueToString(node_value, target_field_key, *dict_manager);
                
                // Compare with the exact value
                bool is_match = (decoded_value == exact_value);
                
                if (is_match) {
                    // Collect matching node for later processing
                    matching_nodes.emplace_back(field_index, node_idx);
                }
            }
            
            // Now process all matching nodes to reconstruct their paths
            for (const auto& [field_idx, node_idx] : matching_nodes) {
                // Convert layer index and node index to BFS index
                size_t bfs_idx = louds_trie->getLayeredStorage().layerIndexToBFS(field_idx, node_idx);
                
                // Reconstruct and decode all paths that go through this node
                auto decoded_paths = reconstructAndDecodePathsFromIntermediateNode(
                    *louds_trie, bfs_idx, *dict_manager);
                
                // Add matching records to result
                for (const auto& path : decoded_paths) {
                    // Convert path to JSON-like string representation
                    std::string record = "{";
                    for (size_t i = 0; i < std::min(path.size(), field_order.size()); ++i) {
                        if (i > 0) record += ", ";
                        record += "\"" + field_order[i].name + "\": ";
                        
                        // Add quotes for string types
                        bool needs_quotes = (field_order[i].type == FieldType::String || 
                                           field_order[i].type == FieldType::Timestamp || 
                                           field_order[i].type == FieldType::LogType ||
                                           field_order[i].type == FieldType::UnstructuredArray);
                        if (needs_quotes) {
                            record += "\"" + path[i] + "\"";
                        } else {
                            record += path[i];
                        }
                    }
                    record += "}";
                    result.records.push_back(record);
                    result.count++;
                    
                    // Check if we've reached the maximum results limit
                    if (result.count >= config_.max_results) {
                        result.is_complete = false; // Not complete because we hit the limit
                        auto end_time = std::chrono::high_resolution_clock::now();
                        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
                        return result;
                    }
                }
            }
        }
        
        // Ensure we mark the query as complete if we haven't hit the limit
        result.is_complete = (result.count < config_.max_results);
        auto end_time = std::chrono::high_resolution_clock::now();
        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing exact match query: " + std::string(e.what());
    }
    
    return result;
}

// ========== 范围查询实现 ==========

RecordQueryResult QueryEngine::executeRangeQuery(
    const std::string& field_name,
    const std::string& min_value,
    const std::string& max_value,
    const GranularCompressedData& granular_data) {
    
    RecordQueryResult result;
    result.count = 0;
    result.chunks_accessed = 0;
    result.decompression_ratio = 0.0;
    result.is_complete = false;
    
    try {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Step 1: Decompress the granular data to get the trie and dictionary manager
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
        
        // Step 2: Set up decompression options to load all necessary components
        Compressor::PartialDecompressionOptions decompress_options;
        decompress_options.load_trie = true;
        decompress_options.load_layers = true;
        decompress_options.load_string_dict = true;
        decompress_options.load_timestamp_dict = true;
        decompress_options.load_logtype_dict = true;
        decompress_options.load_metadata = true;
        
        // Step 3: Decompress the granular data
        // We need to directly decompress to LOUDSTrie instead of Trie to access layer information
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
            
            // Load dictionaries
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
        
        // Step 4: Get field order and find the target field
        const auto& field_order = louds_trie->getFieldOrder();
        int field_index = getFieldIndex(field_name, field_order);
        
        if (field_index == -1) {
            result.error_message = "Field '" + field_name + "' not found in the data";
            return result;
        }
        
        // Step 5: Determine the field type
        FieldType target_field_type = field_order[field_index].type;
        FieldKey target_field_key = field_order[field_index];
        
        // Step 6: Search through all nodes in the trie to find matching paths
        result.chunks_accessed = 1; // We're accessing one chunk
        
        // Store all matching node indices as a set to handle multiple matches in the same layer
        std::vector<std::pair<size_t, size_t>> matching_nodes; // pair of (field_index, node_idx)
        
        // We'll search in the specific layer corresponding to our field
        if (static_cast<size_t>(field_index) < louds_trie->layerCount()) {
            const auto& layer = louds_trie->getLayer(field_index);
            
            for (size_t node_idx = 0; node_idx < layer.size(); ++node_idx) {
                // Get the actual node value directly from the layer
                const NodeValue& node_value = louds_trie->getLayerNodeValue(field_index, node_idx);
                
                // Since we've already decompressed all content, we can directly decode the value
                // Decode the node value to string
                std::string decoded_value = decodeNodeValueToString(node_value, target_field_key, *dict_manager);
                
                // Compare with the range
                bool is_match = false;
                
                // For numeric types, we do numeric comparison
                if (target_field_type == FieldType::Int || target_field_type == FieldType::Double) {
                    try {
                        // Try to convert to numbers for comparison
                        if (target_field_type == FieldType::Int) {
                            int64_t node_val = std::stoll(decoded_value);
                            int64_t min_val = std::stoll(min_value);
                            int64_t max_val = std::stoll(max_value);
                            is_match = (node_val >= min_val && node_val <= max_val);
                        } else { // Double
                            double node_val = std::stod(decoded_value);
                            double min_val = std::stod(min_value);
                            double max_val = std::stod(max_value);
                            is_match = (node_val >= min_val && node_val <= max_val);
                        }
                    } catch (const std::exception& e) {
                        // If conversion fails, fall back to string comparison
                        is_match = (decoded_value >= min_value && decoded_value <= max_value);
                    }
                } else {
                    // For non-numeric types, use string comparison
                    is_match = (decoded_value >= min_value && decoded_value <= max_value);
                }
                
                if (is_match) {
                    // Collect matching node for later processing
                    matching_nodes.emplace_back(field_index, node_idx);
                }
            }
            
            // Now process all matching nodes to reconstruct their paths
            for (const auto& [field_idx, node_idx] : matching_nodes) {
                // Convert layer index and node index to BFS index
                size_t bfs_idx = louds_trie->getLayeredStorage().layerIndexToBFS(field_idx, node_idx);
                
                // Reconstruct and decode all paths that go through this node
                auto decoded_paths = reconstructAndDecodePathsFromIntermediateNode(
                    *louds_trie, bfs_idx, *dict_manager);
                
                // Add matching records to result
                for (const auto& path : decoded_paths) {
                    // Convert path to JSON-like string representation
                    std::string record = "{";
                    for (size_t i = 0; i < std::min(path.size(), field_order.size()); ++i) {
                        if (i > 0) record += ", ";
                        record += "\"" + field_order[i].name + "\": ";
                        
                        // Add quotes for string types
                        bool needs_quotes = (field_order[i].type == FieldType::String || 
                                           field_order[i].type == FieldType::Timestamp || 
                                           field_order[i].type == FieldType::LogType ||
                                           field_order[i].type == FieldType::UnstructuredArray);
                        if (needs_quotes) {
                            record += "\"" + path[i] + "\"";
                        } else {
                            record += path[i];
                        }
                    }
                    record += "}";
                    result.records.push_back(record);
                    result.count++;
                    
                    // Check if we've reached the maximum results limit
                    if (result.count >= config_.max_results) {
                        result.is_complete = false; // Not complete because we hit the limit
                        auto end_time = std::chrono::high_resolution_clock::now();
                        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
                        return result;
                    }
                }
            }
        }
        
        // Ensure we mark the query as complete if we haven't hit the limit
        result.is_complete = (result.count < config_.max_results);
        auto end_time = std::chrono::high_resolution_clock::now();
        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing range query: " + std::string(e.what());
    }
    
    return result;
}

// ========== 复杂查询实现 ==========

QueryResult QueryEngine::executeComplexQuery(
    const std::string& complex_query,
    const GranularCompressedData& granular_data) {
    
    QueryResult result;
    result.count = 0;
    result.chunks_accessed = 0;
    result.dict_hits = 0;
    result.trie_nodes_visited = 0;
    result.matching_blocks = 0;
    result.decompression_ratio = 0.0;
    result.selection_ratio = 0.0;
    result.query_time_ms = 0.0;
    result.is_complete = false;
    
    try {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Parse the complex query into an AST
        auto query_ast = parser_->parse(complex_query);
        
        // Evaluate the AST recursively
        auto eval_result = evaluateQueryNode(query_ast.get(), granular_data);
        
        // Copy results
        result.records = std::move(eval_result.records);
        result.count = result.records.size();
        result.chunks_accessed = eval_result.chunks_accessed;
        result.dict_hits = eval_result.dict_hits;
        result.trie_nodes_visited = eval_result.trie_nodes_visited;
        result.matching_blocks = eval_result.matching_blocks;
        result.decompression_ratio = eval_result.decompression_ratio;
        result.selection_ratio = eval_result.selection_ratio;
        result.is_complete = eval_result.is_complete;
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.query_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.error_message = "Error executing complex query: " + std::string(e.what());
    }
    
    return result;
}

// Helper function to evaluate a query node recursively
QueryResult QueryEngine::evaluateQueryNode(
    const QueryNode* node,
    const GranularCompressedData& granular_data) {
    
    QueryResult result;
    
    if (!node) {
        result.error_message = "Invalid query node";
        return result;
    }
    
    switch (node->getType()) {
        case QueryNodeType::FIELD:
            return evaluateFieldNode(node, granular_data);
            
        case QueryNodeType::LOGICAL:
            return evaluateLogicalNode(node, granular_data);
            
        case QueryNodeType::AGGREGATE:
            return evaluateAggregateNode(node, granular_data);
            
        case QueryNodeType::OPERATOR:
            result.error_message = "Unexpected operator node at top level";
            return result;
            
        case QueryNodeType::VALUE:
            result.error_message = "Unexpected value node at top level";
            return result;
    }
    
    result.error_message = "Unknown query node type";
    return result;
}

// Helper function to evaluate an aggregate node
QueryResult QueryEngine::evaluateAggregateNode(
    const QueryNode* node,
    const GranularCompressedData& granular_data) {
    
    QueryResult result;
    
    // Execute the aggregate query
    auto aggregate_result = executeAggregateQuery(
        node->getAggregateFunction(),
        node->getContent(), // field name
        granular_data);
    
    if (!aggregate_result.error_message.empty()) {
        result.error_message = aggregate_result.error_message;
        return result;
    }
    
    // Format the result as a JSON-like string
    std::string record = "{";
    record += "\"aggregate\": \"" + aggregate_result.aggregate_type + "\", ";
    if (!aggregate_result.field_name.empty()) {
        record += "\"field\": \"" + aggregate_result.field_name + "\", ";
    }
    record += "\"value\": " + std::to_string(aggregate_result.value) + ", ";
    record += "\"count\": " + std::to_string(aggregate_result.count);
    record += "}";
    
    result.records.push_back(record);
    result.count = 1;
    result.chunks_accessed = aggregate_result.chunks_accessed;
    result.query_time_ms = aggregate_result.query_time_ms;
    result.is_complete = aggregate_result.is_complete;
    
    return result;
}

// Helper function to evaluate a field node
QueryResult QueryEngine::evaluateFieldNode(
    const QueryNode* node,
    const GranularCompressedData& granular_data) {
    
    QueryResult result;
    
    if (node->getChildren().empty()) {
        result.error_message = "Field node has no children";
        return result;
    }
    
    const std::string& field_name = node->getContent();
    FieldType field_type = node->getFieldType();
    
    // Get the operator and value (if any)
    const auto& children = node->getChildren();
    if (children.size() < 1) {
        result.error_message = "Field node missing operator";
        return result;
    }
    
    const QueryNode* op_node = children[0].get();
    QueryOperator op = op_node->getOperator();
    
    switch (op) {
        case QueryOperator::EXISTS: {
            // Field existence check
            auto existence_result = checkFieldExistenceAndType(field_name, field_type, granular_data);
            if (!existence_result.error_message.empty()) {
                result.error_message = existence_result.error_message;
                return result;
            }
            
            if (existence_result.exists) {
                // For existence queries, we return a simple result indicating the field exists
                result.count = 1;
                result.records.push_back("{\"field_exists\": \"" + field_name + "\"}");
                result.is_complete = true;
            }
            break;
        }
        
        case QueryOperator::EQUALS: {
            if (children.size() < 2) {
                result.error_message = "Equals operator missing value";
                return result;
            }
            
            const std::string& value = children[1]->getContent();
            auto exact_result = executeExactMatchQuery(field_name, value, granular_data);
            
            if (!exact_result.error_message.empty()) {
                result.error_message = exact_result.error_message;
                return result;
            }
            
            result.records = std::move(exact_result.records);
            result.count = exact_result.count;
            result.chunks_accessed = exact_result.chunks_accessed;
            result.decompression_ratio = exact_result.decompression_ratio;
            result.is_complete = exact_result.is_complete;
            break;
        }
        
        case QueryOperator::RANGE: {
            // Parse range values from content like "min TO max"
            std::string range_content = op_node->getContent();
            size_t to_pos = range_content.find(" TO ");
            if (to_pos == std::string::npos) {
                result.error_message = "Invalid range format";
                return result;
            }
            
            std::string min_value = range_content.substr(0, to_pos);
            std::string max_value = range_content.substr(to_pos + 4);
            
            auto range_result = executeRangeQuery(field_name, min_value, max_value, granular_data);
            
            if (!range_result.error_message.empty()) {
                result.error_message = range_result.error_message;
                return result;
            }
            
            result.records = std::move(range_result.records);
            result.count = range_result.count;
            result.chunks_accessed = range_result.chunks_accessed;
            result.decompression_ratio = range_result.decompression_ratio;
            result.is_complete = range_result.is_complete;
            break;
        }
        
        case QueryOperator::GREATER:
        case QueryOperator::GREATER_EQUAL:
        case QueryOperator::LESS:
        case QueryOperator::LESS_EQUAL: {
            if (children.size() < 2) {
                result.error_message = "Comparison operator missing value";
                return result;
            }
            
            const std::string& value = children[1]->getContent();
            // For simplicity, we'll treat all comparison operators as range queries
            // In a more sophisticated implementation, we would have specific handling
            std::string min_value, max_value;
            
            switch (op) {
                case QueryOperator::GREATER:
                    min_value = std::to_string(std::stoll(value) + 1);
                    max_value = "999999999999999999"; // A large number as upper bound
                    break;
                case QueryOperator::GREATER_EQUAL:
                    min_value = value;
                    max_value = "999999999999999999"; // A large number as upper bound
                    break;
                case QueryOperator::LESS:
                    min_value = "0"; // A small number as lower bound
                    max_value = std::to_string(std::stoll(value) - 1);
                    break;
                case QueryOperator::LESS_EQUAL:
                    min_value = "0"; // A small number as lower bound
                    max_value = value;
                    break;
                default:
                    result.error_message = "Unexpected operator";
                    return result;
            }
            
            auto range_result = executeRangeQuery(field_name, min_value, max_value, granular_data);
            
            if (!range_result.error_message.empty()) {
                result.error_message = range_result.error_message;
                return result;
            }
            
            result.records = std::move(range_result.records);
            result.count = range_result.count;
            result.chunks_accessed = range_result.chunks_accessed;
            result.decompression_ratio = range_result.decompression_ratio;
            result.is_complete = range_result.is_complete;
            break;
        }
        
        default:
            result.error_message = "Unsupported operator for field query";
            return result;
    }
    
    return result;
}

// Helper function to evaluate a logical node
QueryResult QueryEngine::evaluateLogicalNode(
    const QueryNode* node,
    const GranularCompressedData& granular_data) {
    
    QueryResult result;
    
    QueryOperator op = node->getOperator();
    const auto& children = node->getChildren();
    
    if (children.empty()) {
        result.error_message = "Logical node has no children";
        return result;
    }
    
    switch (op) {
        case QueryOperator::AND: {
            if (children.size() != 2) {
                result.error_message = "AND operator requires exactly 2 operands";
                return result;
            }
            
            auto left_result = evaluateQueryNode(children[0].get(), granular_data);
            if (!left_result.error_message.empty()) {
                return left_result;
            }
            
            auto right_result = evaluateQueryNode(children[1].get(), granular_data);
            if (!right_result.error_message.empty()) {
                return right_result;
            }
            
            // Use the merge function for AND operation
            return mergeResultsAND(left_result, right_result);
        }
        
        case QueryOperator::OR: {
            if (children.size() != 2) {
                result.error_message = "OR operator requires exactly 2 operands";
                return result;
            }
            
            auto left_result = evaluateQueryNode(children[0].get(), granular_data);
            if (!left_result.error_message.empty()) {
                return left_result;
            }
            
            auto right_result = evaluateQueryNode(children[1].get(), granular_data);
            if (!right_result.error_message.empty()) {
                return right_result;
            }
            
            // Use the merge function for OR operation
            return mergeResultsOR(left_result, right_result);
        }
        
        case QueryOperator::NOT: {
            if (children.size() != 1) {
                result.error_message = "NOT operator requires exactly 1 operand";
                return result;
            }
            
            auto operand_result = evaluateQueryNode(children[0].get(), granular_data);
            if (!operand_result.error_message.empty()) {
                return operand_result;
            }
            
            // Use the negate function for NOT operation
            return negateResult(operand_result);
        }
        
        default:
            result.error_message = "Unsupported logical operator";
            return result;
    }
    
    return result;
}

// 评估分组节点
QueryResult QueryEngine::evaluateGroupByNode(
    const QueryNode* node,
    const GranularCompressedData& granular_data) {
    
    QueryResult result;
    
    if (!node) {
        result.error_message = "Invalid group by node";
        return result;
    }
    
    // Get the group fields
    const std::vector<std::string>& group_fields = node->getGroupFields();
    
    // Handle multiple aggregate functions
    if (node->getChildren().empty()) {
        result.error_message = "Group by node has no children";
        return result;
    }
    
    // Collect all aggregate functions and fields
    std::vector<AggregateFunction> agg_funcs;
    std::vector<std::string> agg_fields;
    
    // Process all children that are aggregate nodes
    for (const auto& child : node->getChildren()) {
        if (child->getType() == QueryNodeType::AGGREGATE) {
            agg_funcs.push_back(child->getAggregateFunction());
            agg_fields.push_back(child->getContent());
        }
    }
    
    // If no aggregate functions found, return error
    if (agg_funcs.empty()) {
        result.error_message = "Group by node has no aggregate children";
        return result;
    }
    
    // Execute grouped aggregate query with all aggregate functions
    auto grouped_result = executeGroupedAggregateQuery(
        agg_funcs, agg_fields, group_fields, granular_data);
    
    if (!grouped_result.error_message.empty()) {
        result.error_message = grouped_result.error_message;
        return result;
    }
    
    // Format the grouped results as JSON-like strings
    for (const auto& group_entry : grouped_result.grouped_values) {
        const std::vector<std::string>& group_values = group_entry.first;
        const auto& agg_values = group_entry.second;
        
        std::string record = "{";
        
        // Add group fields
        for (size_t i = 0; i < std::min(group_fields.size(), group_values.size()); ++i) {
            if (i > 0) record += ", ";
            record += "\"" + group_fields[i] + "\": ";
            
            // For group values, we'll assume they need quotes (most common case)
            // In a more sophisticated implementation, we would check the actual field type
            record += "\"" + group_values[i] + "\"";
        }
        
        // Add aggregate values
        // For aggregate values, we typically don't need quotes as they are numeric
        bool first_agg = true;
        for (const auto& agg_entry : agg_values) {
            if (!group_fields.empty() || !first_agg) record += ", ";
            // The key format from executeGroupedAggregateQuery is "field_funcType"
            // We'll simplify the key name for better readability
            std::string key_name = agg_entry.first;
            size_t underscore_pos = key_name.find_last_of('_');
            if (underscore_pos != std::string::npos) {
                // Extract just the field name part
                key_name = key_name.substr(0, underscore_pos);
            }
            record += "\"" + key_name + "\": " + std::to_string(agg_entry.second);
            first_agg = false;
        }
        
        record += "}";
        result.records.push_back(record);
    }
    
    result.count = result.records.size();
    result.chunks_accessed = grouped_result.chunks_accessed;
    result.query_time_ms = grouped_result.query_time_ms;
    result.is_complete = grouped_result.is_complete;
    
    return result;
}

// 合并两个查询结果（用于AND操作）
QueryResult QueryEngine::mergeResultsAND(const QueryResult& left, const QueryResult& right) const {
    QueryResult result;
    
    try {
        // 对于AND操作，我们需要找到两个结果集的交集
        std::set<std::string> left_records(left.records.begin(), left.records.end());
        std::vector<std::string> intersection;
        
        for (const auto& record : right.records) {
            if (left_records.find(record) != left_records.end()) {
                intersection.push_back(record);
            }
        }
        
        result.records = intersection;
        result.count = intersection.size();
        result.chunks_accessed = left.chunks_accessed + right.chunks_accessed;
        result.decompression_ratio = (left.decompression_ratio + right.decompression_ratio) / 2.0;
        result.query_time_ms = left.query_time_ms + right.query_time_ms;
        result.is_complete = left.is_complete && right.is_complete;
        
    } catch (const std::exception& e) {
        result.error_message = "Error merging results with AND: " + std::string(e.what());
    }
    
    return result;
}

// 合并两个查询结果（用于OR操作）
QueryResult QueryEngine::mergeResultsOR(const QueryResult& left, const QueryResult& right) const {
    QueryResult result;
    
    try {
        // 对于OR操作，我们需要合并两个结果集并去重
        std::set<std::string> unique_records;
        unique_records.insert(left.records.begin(), left.records.end());
        unique_records.insert(right.records.begin(), right.records.end());
        
        result.records.assign(unique_records.begin(), unique_records.end());
        result.count = unique_records.size();
        result.chunks_accessed = left.chunks_accessed + right.chunks_accessed;
        result.decompression_ratio = (left.decompression_ratio + right.decompression_ratio) / 2.0;
        result.query_time_ms = left.query_time_ms + right.query_time_ms;
        result.is_complete = left.is_complete && right.is_complete;
        
    } catch (const std::exception& e) {
        result.error_message = "Error merging results with OR: " + std::string(e.what());
    }
    
    return result;
}

// 对查询结果进行NOT操作
QueryResult QueryEngine::negateResult(const QueryResult& result) const {
    QueryResult negated_result;
    
    try {
        // NOT操作的简化实现
        // 在实际应用中，NOT操作需要访问所有记录然后排除匹配的记录
        // 这里我们只是简单地清空记录来表示否定操作
        negated_result.records.clear();
        negated_result.count = 0;
        negated_result.chunks_accessed = result.chunks_accessed;
        negated_result.decompression_ratio = result.decompression_ratio;
        negated_result.query_time_ms = result.query_time_ms;
        negated_result.is_complete = result.is_complete;
        
        // 在实际实现中，NOT操作需要一个完整的记录集来对比
        // 这里我们添加一个标记来表示这是NOT操作的结果
        // 添加更详细的说明信息
        negated_result.records.push_back("{\"operation\": \"NOT\", \"note\": \"simplified implementation - in a full implementation, this would return all records that do not match the operand\"}");
        negated_result.count = 1;
        
    } catch (const std::exception& e) {
        negated_result.error_message = "Error negating result: " + std::string(e.what());
    }
    
    return negated_result;
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
    target.is_complete = target.is_complete && source.is_complete && (target.count < config_.max_results);
    
    // Propagate error message if source has one and target doesn't
    if (!source.error_message.empty() && target.error_message.empty()) {
        target.error_message = source.error_message;
    }
}

// Helper functions to merge AggregateQueryResult objects from multiple blocks
void QueryEngine::mergeAggregateQueryResults(AggregateQueryResult& target, const AggregateQueryResult& source) const {
    // For aggregate functions, we need to combine values appropriately based on the aggregate type
    if (target.aggregate_type.empty()) {
        // First result, just copy values
        target.value = source.value;
        target.count = source.count;
        target.aggregate_type = source.aggregate_type;
        target.field_name = source.field_name;
    } else {
        // Combine based on aggregate type
        if (target.aggregate_type == "COUNT" || target.aggregate_type == "SUM") {
            target.value += source.value;
            target.count += source.count;
        } else if (target.aggregate_type == "AVG") {
            // For average, we need to recompute based on total count and sum
            double total_sum = target.value * target.count + source.value * source.count;
            target.count += source.count;
            if (target.count > 0) {
                target.value = total_sum / target.count;
            }
        } else if (target.aggregate_type == "MAX") {
            if (source.value > target.value) {
                target.value = source.value;
            }
            target.count += source.count;
        } else if (target.aggregate_type == "MIN") {
            if (source.value < target.value) {
                target.value = source.value;
            }
            target.count += source.count;
        }
    }
    
    // Update chunks accessed
    target.chunks_accessed += source.chunks_accessed;
    
    // Update query time
    target.query_time_ms += source.query_time_ms;
    
    // Update completion status
    target.is_complete = target.is_complete && source.is_complete;
    
    // Propagate error message if source has one and target doesn't
    if (!source.error_message.empty() && target.error_message.empty()) {
        target.error_message = source.error_message;
    }
}

// Helper functions to merge GroupedAggregateQueryResult objects from multiple blocks
void QueryEngine::mergeGroupedAggregateQueryResults(GroupedAggregateQueryResult& target, const GroupedAggregateQueryResult& source) const {
    // Merge grouped values
    for (const auto& source_group : source.grouped_values) {
        const std::vector<std::string>& group_key = source_group.first;
        const auto& source_agg_values = source_group.second;
        
        // Check if this group already exists in target
        auto target_it = target.grouped_values.find(group_key);
        if (target_it == target.grouped_values.end()) {
            // Group doesn't exist in target, add it
            target.grouped_values[group_key] = source_agg_values;
        } else {
            // Group exists, merge the aggregate values
            auto& target_agg_values = target_it->second;
            for (const auto& source_agg_entry : source_agg_values) {
                const std::string& agg_key = source_agg_entry.first;
                double source_value = source_agg_entry.second;
                
                auto target_agg_it = target_agg_values.find(agg_key);
                if (target_agg_it == target_agg_values.end()) {
                    // Aggregate key doesn't exist in target group, add it
                    target_agg_values[agg_key] = source_value;
                } else {
                    // Aggregate key exists, combine values
                    double& target_value = target_agg_it->second;
                    
                    // For grouped aggregates, we assume they're all SUM/COUNT operations that can be added
                    target_value += source_value;
                }
            }
        }
    }
    
    // Update group fields if not set
    if (target.group_fields.empty()) {
        target.group_fields = source.group_fields;
    }
    
    // Update aggregate fields if not set
    if (target.aggregate_fields.empty()) {
        target.aggregate_fields = source.aggregate_fields;
    }
    
    // Update counts
    target.total_count += source.total_count;
    target.groups_count = target.grouped_values.size(); // Recalculate groups count
    target.chunks_accessed += source.chunks_accessed;
    
    // Update query time
    target.query_time_ms += source.query_time_ms;
    
    // Update completion status
    target.is_complete = target.is_complete && source.is_complete;
    
    // Propagate error message if source has one and target doesn't
    if (!source.error_message.empty() && target.error_message.empty()) {
        target.error_message = source.error_message;
    }
}

// ========== 多块遍历接口实现 ==========

RecordQueryResult QueryEngine::executeExactMatchQueryMultiBlock(
    const std::string& field_name,
    const std::string& exact_value,
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
        RecordQueryResult chunk_result = executeExactMatchQuery(field_name, exact_value, granular_data);
        
        // Merge results
        mergeRecordQueryResults(final_result, chunk_result);
        
        // Check if we've reached the maximum results limit
        if (final_result.count >= config_.max_results) {
            final_result.is_complete = false; // Not complete because we hit the limit
            break;
        }
    }
    
    return final_result;
}

RecordQueryResult QueryEngine::executeRangeQueryMultiBlock(
    const std::string& field_name,
    const std::string& min_value,
    const std::string& max_value,
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
        RecordQueryResult chunk_result = executeRangeQuery(field_name, min_value, max_value, granular_data);
        
        // Merge results
        mergeRecordQueryResults(final_result, chunk_result);
        
        // Check if we've reached the maximum results limit
        if (final_result.count >= config_.max_results) {
            final_result.is_complete = false; // Not complete because we hit the limit
            break;
        }
    }
    
    return final_result;
}

AggregateQueryResult QueryEngine::executeAggregateQueryMultiBlock(
    AggregateFunction aggregate_func,
    const std::string& field_name,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    AggregateQueryResult final_result;
    final_result.value = 0.0;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // Process each chunk
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        // Extract granular data from chunk
        GranularCompressedData granular_data = extractGranularDataFromChunk(chunk, i);
        
        // Execute query on this chunk
        AggregateQueryResult chunk_result = executeAggregateQuery(aggregate_func, field_name, granular_data);
        
        // Merge results
        mergeAggregateQueryResults(final_result, chunk_result);
    }
    
    return final_result;
}

GroupedAggregateQueryResult QueryEngine::executeGroupedAggregateQueryMultiBlock(
    const std::vector<AggregateFunction>& aggregate_funcs,
    const std::vector<std::string>& aggregate_fields,
    const std::vector<std::string>& group_fields,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    GroupedAggregateQueryResult final_result;
    final_result.groups_count = 0;
    final_result.total_count = 0;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // Process each chunk
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        // Extract granular data from chunk
        GranularCompressedData granular_data = extractGranularDataFromChunk(chunk, i);
        
        // Execute query on this chunk
        GroupedAggregateQueryResult chunk_result = executeGroupedAggregateQuery(
            aggregate_funcs, aggregate_fields, group_fields, granular_data);
        
        // Merge results
        mergeGroupedAggregateQueryResults(final_result, chunk_result);
    }
    
    return final_result;
}

QueryResult QueryEngine::executeComplexQueryMultiBlock(
    const std::string& complex_query,
    const std::vector<ChunkedTypeAwareBlock>& chunks) {
    
    QueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.dict_hits = 0;
    final_result.trie_nodes_visited = 0;
    final_result.matching_blocks = 0;
    final_result.decompression_ratio = 0.0;
    final_result.selection_ratio = 0.0;
    final_result.query_time_ms = 0.0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // Process each chunk
    for (size_t i = 0; i < chunks.size(); ++i) {
        const auto& chunk = chunks[i];
        // Extract granular data from chunk
        GranularCompressedData granular_data = extractGranularDataFromChunk(chunk, i);
        
        // Execute query on this chunk
        QueryResult chunk_result = executeComplexQuery(complex_query, granular_data);
        
        // Merge results using existing merge functions
        if (final_result.records.empty()) {
            final_result = chunk_result;
        } else {
            // For complex queries, we'll use a simple merge approach
            final_result.records.insert(final_result.records.end(), 
                                       chunk_result.records.begin(), 
                                       chunk_result.records.end());
            final_result.count += chunk_result.count;
            final_result.chunks_accessed += chunk_result.chunks_accessed;
            final_result.dict_hits += chunk_result.dict_hits;
            final_result.trie_nodes_visited += chunk_result.trie_nodes_visited;
            final_result.matching_blocks += chunk_result.matching_blocks;
            
            // Update decompression ratio (weighted average)
            if (final_result.chunks_accessed + chunk_result.chunks_accessed > 0) {
                final_result.decompression_ratio = 
                    (final_result.decompression_ratio * final_result.chunks_accessed + 
                     chunk_result.decompression_ratio * chunk_result.chunks_accessed) / 
                    (final_result.chunks_accessed + chunk_result.chunks_accessed);
            }
            
            // Update selection ratio (weighted average)
            if (final_result.chunks_accessed + chunk_result.chunks_accessed > 0) {
                final_result.selection_ratio = 
                    (final_result.selection_ratio * final_result.chunks_accessed + 
                     chunk_result.selection_ratio * chunk_result.chunks_accessed) / 
                    (final_result.chunks_accessed + chunk_result.chunks_accessed);
            }
            
            final_result.query_time_ms += chunk_result.query_time_ms;
            final_result.is_complete = final_result.is_complete && chunk_result.is_complete;
            
            // Propagate error message if chunk has one and final doesn't
            if (!chunk_result.error_message.empty() && final_result.error_message.empty()) {
                final_result.error_message = chunk_result.error_message;
            }
        }
        
        // Check if we've reached the maximum results limit
        if (final_result.count >= config_.max_results) {
            final_result.is_complete = false; // Not complete because we hit the limit
            break;
        }
    }
    
    return final_result;
}

// ========== 多线程多块遍历接口实现 ==========

RecordQueryResult QueryEngine::executeExactMatchQueryMultiBlockParallel(
    const std::string& field_name,
    const std::string& exact_value,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    RecordQueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.decompression_ratio = 0.0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeExactMatchQueryMultiBlock(field_name, exact_value, chunks);
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
        futures.push_back(std::async(std::launch::async, [this, &field_name, &exact_value, &chunks, group_indices = chunk_groups[group_idx]]() {
            RecordQueryResult group_result;
            group_result.count = 0;
            group_result.chunks_accessed = 0;
            group_result.decompression_ratio = 0.0;
            group_result.is_complete = true;
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                RecordQueryResult chunk_result = executeExactMatchQuery(field_name, exact_value, granular_data);
                
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
            
            // Check if we've reached the maximum results limit
            if (final_result.count >= config_.max_results) {
                final_result.is_complete = false; // Not complete because we hit the limit
                break;
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

RecordQueryResult QueryEngine::executeRangeQueryMultiBlockParallel(
    const std::string& field_name,
    const std::string& min_value,
    const std::string& max_value,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    RecordQueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.decompression_ratio = 0.0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeRangeQueryMultiBlock(field_name, min_value, max_value, chunks);
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
        futures.push_back(std::async(std::launch::async, [this, &field_name, &min_value, &max_value, &chunks, group_indices = chunk_groups[group_idx]]() {
            RecordQueryResult group_result;
            group_result.count = 0;
            group_result.chunks_accessed = 0;
            group_result.decompression_ratio = 0.0;
            group_result.is_complete = true;
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                RecordQueryResult chunk_result = executeRangeQuery(field_name, min_value, max_value, granular_data);
                
                // Merge results
                mergeRecordQueryResults(group_result, chunk_result);
                
                // Check if we've reached the maximum results limit
                if (group_result.count >= config_.max_results) {
                    group_result.is_complete = false; // Not complete because we hit the limit
                    break;
                }
            }
            
            return group_result;
        }));
    }
    
    // Collect results from all threads
    for (auto& future : futures) {
        try {
            RecordQueryResult group_result = future.get();
            mergeRecordQueryResults(final_result, group_result);
            
            // Check if we've reached the maximum results limit
            if (final_result.count >= config_.max_results) {
                final_result.is_complete = false; // Not complete because we hit the limit
                break;
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

AggregateQueryResult QueryEngine::executeAggregateQueryMultiBlockParallel(
    AggregateFunction aggregate_func,
    const std::string& field_name,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    AggregateQueryResult final_result;
    final_result.value = 0.0;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeAggregateQueryMultiBlock(aggregate_func, field_name, chunks);
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
        futures.push_back(std::async(std::launch::async, [this, aggregate_func, &field_name, &chunks, group_indices = chunk_groups[group_idx]]() {
            AggregateQueryResult group_result;
            group_result.value = 0.0;
            group_result.count = 0;
            group_result.chunks_accessed = 0;
            group_result.is_complete = true;
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                AggregateQueryResult chunk_result = executeAggregateQuery(aggregate_func, field_name, granular_data);
                
                // Merge results
                mergeAggregateQueryResults(group_result, chunk_result);
            }
            
            return group_result;
        }));
    }
    
    // Collect results from all threads
    for (auto& future : futures) {
        try {
            AggregateQueryResult group_result = future.get();
            mergeAggregateQueryResults(final_result, group_result);
        } catch (const std::exception& e) {
            // Handle exceptions from threads
            if (final_result.error_message.empty()) {
                final_result.error_message = "Error in parallel processing: " + std::string(e.what());
            }
        }
    }
    
    return final_result;
}

GroupedAggregateQueryResult QueryEngine::executeGroupedAggregateQueryMultiBlockParallel(
    const std::vector<AggregateFunction>& aggregate_funcs,
    const std::vector<std::string>& aggregate_fields,
    const std::vector<std::string>& group_fields,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    GroupedAggregateQueryResult final_result;
    final_result.groups_count = 0;
    final_result.total_count = 0;
    final_result.chunks_accessed = 0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeGroupedAggregateQueryMultiBlock(aggregate_funcs, aggregate_fields, group_fields, chunks);
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
    std::vector<std::future<GroupedAggregateQueryResult>> futures;
    for (size_t group_idx = 0; group_idx < thread_count; ++group_idx) {
        if (chunk_groups[group_idx].empty()) continue;
        
        // Launch async task with chunk indices
        futures.push_back(std::async(std::launch::async, [this, &aggregate_funcs, &aggregate_fields, &group_fields, &chunks, group_indices = chunk_groups[group_idx]]() {
            GroupedAggregateQueryResult group_result;
            group_result.groups_count = 0;
            group_result.total_count = 0;
            group_result.chunks_accessed = 0;
            group_result.is_complete = true;
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                GroupedAggregateQueryResult chunk_result = executeGroupedAggregateQuery(
                    aggregate_funcs, aggregate_fields, group_fields, granular_data);
                
                // Merge results
                mergeGroupedAggregateQueryResults(group_result, chunk_result);
            }
            
            return group_result;
        }));
    }
    
    // Collect results from all threads
    for (auto& future : futures) {
        try {
            GroupedAggregateQueryResult group_result = future.get();
            mergeGroupedAggregateQueryResults(final_result, group_result);
        } catch (const std::exception& e) {
            // Handle exceptions from threads
            if (final_result.error_message.empty()) {
                final_result.error_message = "Error in parallel processing: " + std::string(e.what());
            }
        }
    }
    
    return final_result;
}

QueryResult QueryEngine::executeComplexQueryMultiBlockParallel(
    const std::string& complex_query,
    const std::vector<ChunkedTypeAwareBlock>& chunks,
    size_t thread_count) {
    
    QueryResult final_result;
    final_result.count = 0;
    final_result.chunks_accessed = 0;
    final_result.dict_hits = 0;
    final_result.trie_nodes_visited = 0;
    final_result.matching_blocks = 0;
    final_result.decompression_ratio = 0.0;
    final_result.selection_ratio = 0.0;
    final_result.query_time_ms = 0.0;
    final_result.is_complete = true; // Start with complete, set to false if any chunk is incomplete
    
    // If thread_count is 0 or 1, fall back to sequential processing
    if (thread_count <= 1 || chunks.empty()) {
        return executeComplexQueryMultiBlock(complex_query, chunks);
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
    std::vector<std::future<QueryResult>> futures;
    for (size_t group_idx = 0; group_idx < thread_count; ++group_idx) {
        if (chunk_groups[group_idx].empty()) continue;
        
        // Launch async task with chunk indices
        futures.push_back(std::async(std::launch::async, [this, &complex_query, &chunks, group_indices = chunk_groups[group_idx]]() {
            QueryResult group_result;
            group_result.count = 0;
            group_result.chunks_accessed = 0;
            group_result.dict_hits = 0;
            group_result.trie_nodes_visited = 0;
            group_result.matching_blocks = 0;
            group_result.decompression_ratio = 0.0;
            group_result.selection_ratio = 0.0;
            group_result.query_time_ms = 0.0;
            group_result.is_complete = true;
            
            for (size_t chunk_idx : group_indices) {
                // Extract granular data from chunk using the index
                GranularCompressedData granular_data = extractGranularDataFromChunk(chunks[chunk_idx], chunk_idx);
                
                // Execute query on this chunk
                QueryResult chunk_result = executeComplexQuery(complex_query, granular_data);
                
                // Merge results using existing merge functions
                if (group_result.records.empty()) {
                    group_result = chunk_result;
                } else {
                    // For complex queries, we'll use a simple merge approach
                    group_result.records.insert(group_result.records.end(), 
                                               chunk_result.records.begin(), 
                                               chunk_result.records.end());
                    group_result.count += chunk_result.count;
                    group_result.chunks_accessed += chunk_result.chunks_accessed;
                    group_result.dict_hits += chunk_result.dict_hits;
                    group_result.trie_nodes_visited += chunk_result.trie_nodes_visited;
                    group_result.matching_blocks += chunk_result.matching_blocks;
                    
                    // Update decompression ratio (weighted average)
                    if (group_result.chunks_accessed + chunk_result.chunks_accessed > 0) {
                        group_result.decompression_ratio = 
                            (group_result.decompression_ratio * group_result.chunks_accessed + 
                             chunk_result.decompression_ratio * chunk_result.chunks_accessed) / 
                            (group_result.chunks_accessed + chunk_result.chunks_accessed);
                    }
                    
                    // Update selection ratio (weighted average)
                    if (group_result.chunks_accessed + chunk_result.chunks_accessed > 0) {
                        group_result.selection_ratio = 
                            (group_result.selection_ratio * group_result.chunks_accessed + 
                             chunk_result.selection_ratio * chunk_result.chunks_accessed) / 
                            (group_result.chunks_accessed + chunk_result.chunks_accessed);
                    }
                    
                    group_result.query_time_ms += chunk_result.query_time_ms;
                    group_result.is_complete = group_result.is_complete && chunk_result.is_complete;
                    
                    // Propagate error message if chunk has one and final doesn't
                    if (!chunk_result.error_message.empty() && group_result.error_message.empty()) {
                        group_result.error_message = chunk_result.error_message;
                    }
                }
                
                // Check if we've reached the maximum results limit
                if (group_result.count >= config_.max_results) {
                    group_result.is_complete = false; // Not complete because we hit the limit
                    break;
                }
            }
            
            return group_result;
        }));
    }
    
    // Collect results from all threads
    for (auto& future : futures) {
        try {
            QueryResult group_result = future.get();
            
            // Merge results using existing merge functions
            if (final_result.records.empty()) {
                final_result = group_result;
            } else {
                // For complex queries, we'll use a simple merge approach
                final_result.records.insert(final_result.records.end(), 
                                           group_result.records.begin(), 
                                           group_result.records.end());
                final_result.count += group_result.count;
                final_result.chunks_accessed += group_result.chunks_accessed;
                final_result.dict_hits += group_result.dict_hits;
                final_result.trie_nodes_visited += group_result.trie_nodes_visited;
                final_result.matching_blocks += group_result.matching_blocks;
                
                // Update decompression ratio (weighted average)
                if (final_result.chunks_accessed + group_result.chunks_accessed > 0) {
                    final_result.decompression_ratio = 
                        (final_result.decompression_ratio * final_result.chunks_accessed + 
                         group_result.decompression_ratio * group_result.chunks_accessed) / 
                        (final_result.chunks_accessed + group_result.chunks_accessed);
                }
                
                // Update selection ratio (weighted average)
                if (final_result.chunks_accessed + group_result.chunks_accessed > 0) {
                    final_result.selection_ratio = 
                        (final_result.selection_ratio * final_result.chunks_accessed + 
                         group_result.selection_ratio * group_result.chunks_accessed) / 
                        (final_result.chunks_accessed + group_result.chunks_accessed);
                }
                
                final_result.query_time_ms += group_result.query_time_ms;
                final_result.is_complete = final_result.is_complete && group_result.is_complete;
                
                // Propagate error message if chunk has one and final doesn't
                if (!group_result.error_message.empty() && final_result.error_message.empty()) {
                    final_result.error_message = group_result.error_message;
                }
            }
            
            // Check if we've reached the maximum results limit
            if (final_result.count >= config_.max_results) {
                final_result.is_complete = false; // Not complete because we hit the limit
                break;
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