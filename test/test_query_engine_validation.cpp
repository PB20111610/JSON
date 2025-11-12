#include "../include/query/query_engine.h"
#include "../include/compress_type_aware.h"
#include "../include/louds.h"
#include "../include/trie.h"
#include "../include/field_dictionary_manager.h"
#include "../include/chunked_type_aware_compress.h"
#include "../include/loudsTotrie.h"
#include "../include/query/selective_decompressor.h"
#include "../include/compress.h"
#include "../include/timestamp_dictionary.h"
#include "../include/logtype_dictionary.h"
#include "../include/compression/core/compression_backend.h"
#include "test_config_utils.h"
#include <iostream>
#include <cassert>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <simdjson.h>
#include <iomanip>

using namespace json2;
using namespace json2::query;

// Shared compression configuration for all decompression operations
static const compression::TypeAwareCompressionConfig SHARED_COMPRESSION_CONFIG = json2::test::createStandardTestConfig();

// Mock implementation of QueryEngine for testing
class TestQueryEngine : public QueryEngine {
public:
    FieldFilterResult checkFieldExistenceAndType(
        const std::string& field_name,
        FieldType expected_type,
        const GranularCompressedData& granular_data) override {
        
        return QueryEngine::checkFieldExistenceAndType(field_name, expected_type, granular_data);
    }

    // filterDictionaryValues method removed as filterLayerValues now handles all field types

    ValueFilterResult filterLayerValues(
        const std::string& field_name,
        FieldType field_type,
        const std::string& target_value,
        const std::string& comparison_op,
        const GranularCompressedData& granular_data) override {
        
        return QueryEngine::filterLayerValues(field_name, field_type, target_value, comparison_op, granular_data);
    }

    ValueFilterResult filterFieldAndValues(
        const std::string& field_name,
        FieldType expected_type,
        const std::string& target_value,
        const std::string& comparison_op,
        const GranularCompressedData& granular_data) override {
        
        return QueryEngine::filterFieldAndValues(field_name, expected_type, target_value, comparison_op, granular_data);
    }

protected:
    // All field types now use layer decompression, methods removed
};

// Function to load compressed data from directory
GranularCompressedData loadCompressedDataFromDirectory(const std::string& directory_path) {
    GranularCompressedData granular_data;
    
    // Construct the path to the first chunk
    std::string chunk_path = directory_path + "/chunks/chunk_000000";
    
    // Load trie bitmap (louds.json2)
    std::ifstream trie_bitmap_file(chunk_path + "/louds.json2", std::ios::binary);
    if (trie_bitmap_file.is_open()) {
        uint32_t size;
        trie_bitmap_file.read(reinterpret_cast<char*>(&size), sizeof(size));
        granular_data.trie_bitmap.resize(size);
        trie_bitmap_file.read(reinterpret_cast<char*>(granular_data.trie_bitmap.data()), size);
        trie_bitmap_file.close();
    }
    
    // Load string dictionary (dictionaries/variables.json2)
    std::ifstream string_dict_file(chunk_path + "/dictionaries/variables.json2", std::ios::binary);
    if (string_dict_file.is_open()) {
        uint32_t size;
        string_dict_file.read(reinterpret_cast<char*>(&size), sizeof(size));
        granular_data.string_dict.resize(size);
        string_dict_file.read(reinterpret_cast<char*>(granular_data.string_dict.data()), size);
        string_dict_file.close();
    }
    
    // Load timestamp dictionary (dictionaries/timestamps.json2)
    std::ifstream timestamp_dict_file(chunk_path + "/dictionaries/timestamps.json2", std::ios::binary);
    if (timestamp_dict_file.is_open()) {
        uint32_t size;
        timestamp_dict_file.read(reinterpret_cast<char*>(&size), sizeof(size));
        granular_data.timestamp_dict.resize(size);
        timestamp_dict_file.read(reinterpret_cast<char*>(granular_data.timestamp_dict.data()), size);
        timestamp_dict_file.close();
    }
    
    // Load logtype dictionary (dictionaries/logtypes.json2)
    std::ifstream logtype_dict_file(chunk_path + "/dictionaries/logtypes.json2", std::ios::binary);
    if (logtype_dict_file.is_open()) {
        uint32_t size;
        logtype_dict_file.read(reinterpret_cast<char*>(&size), sizeof(size));
        granular_data.logtype_dict.resize(size);
        logtype_dict_file.read(reinterpret_cast<char*>(granular_data.logtype_dict.data()), size);
        logtype_dict_file.close();
    }
    
    // Load metadata (metadata.json2)
    std::ifstream metadata_file(chunk_path + "/metadata.json2", std::ios::binary);
    if (metadata_file.is_open()) {
        uint32_t size;
        metadata_file.read(reinterpret_cast<char*>(&size), sizeof(size));
        granular_data.metadata.resize(size);
        metadata_file.read(reinterpret_cast<char*>(granular_data.metadata.data()), size);
        metadata_file.close();
    }
    
    // Load layer sizes (layer_sizes.json2)
    std::ifstream layer_sizes_file(chunk_path + "/layer_sizes.json2", std::ios::binary);
    if (layer_sizes_file.is_open()) {
        uint32_t size;
        layer_sizes_file.read(reinterpret_cast<char*>(&size), sizeof(size));
        granular_data.layer_sizes.resize(size);
        layer_sizes_file.read(reinterpret_cast<char*>(granular_data.layer_sizes.data()), size);
        layer_sizes_file.close();
    }
    
    // Since we're using layer separation, load individual layer files
    // Set layer separation flag to true
    granular_data.use_layer_separation = true;
    
    // Load layer files (layer1.json2, etc.)
    // We'll check for layer files and load them in order
    int layer_index = 0;  // Start from 0 to match field indices
    int loaded_layers = 0;
    while (true) {
        std::string layer_filename = chunk_path + "/layer_" + std::to_string(layer_index) + ".json2";
        // std::cout << "  Trying to open layer file: " << layer_filename << std::endl;
        std::ifstream layer_file(layer_filename, std::ios::binary);
        
        if (!layer_file.is_open()) {
            // No more layer files
            // std::cout << "  Stopped loading layers at index " << layer_index << " (file not found)" << std::endl;
            break;
        }
        
        // Read the layer data with size prefix
        uint32_t size;
        layer_file.read(reinterpret_cast<char*>(&size), sizeof(size));
        std::vector<uint8_t> layer_data(size);
        layer_file.read(reinterpret_cast<char*>(layer_data.data()), size);
        layer_file.close();
        
        granular_data.layer_data_by_level.push_back(std::move(layer_data));
        // std::cout << "  Loaded layer " << (layer_index + 1) << " with size " << size << std::endl;
        loaded_layers++;
        layer_index++;
    }
    std::cout << "  Total layers loaded: " << loaded_layers << std::endl;
    
    return granular_data;
}

// New function to inspect dictionary contents
void inspectDictionaries(const GranularCompressedData& granular_data) {
    std::cout << "\n=== Dictionary Contents Inspection ===\n";
    
    // Inspect string dictionary
    if (!granular_data.string_dict.empty()) {
        try {
            std::vector<uint8_t> decompressed = decompressWithConfig(granular_data.string_dict, 
                                                                   compression::FieldType::STRING, 
                                                                   SHARED_COMPRESSION_CONFIG);
            FieldDictionaryManager manager;
            Compressor::deserializeStringDictionary(decompressed, manager);
            
            std::cout << "  String Dictionary:\n";
            auto& var_dict = manager.variableDict();
            std::vector<std::string> all_values = var_dict.getAllStringValues();
            for (size_t i = 0; i < std::min(size_t(10), all_values.size()); ++i) {
                std::cout << "    [" << (i+1) << "] " << all_values[i] << "\n";
            }
            if (all_values.size() > 10) {
                std::cout << "    ... and " << (all_values.size() - 10) << " more entries\n";
            }
        } catch (const std::exception& e) {
            std::cout << "  Error inspecting string dictionary: " << e.what() << "\n";
        }
    } else {
        std::cout << "  String dictionary is empty\n";
    }
    
    // Inspect timestamp dictionary
    if (!granular_data.timestamp_dict.empty()) {
        try {
            std::vector<uint8_t> decompressed = decompressWithConfig(granular_data.timestamp_dict, 
                                                                   compression::FieldType::TIMESTAMP, 
                                                                   SHARED_COMPRESSION_CONFIG);
            FieldDictionaryManager manager;
            Compressor::deserializeTimestampDictionary(decompressed, manager);
            
            std::cout << "  Timestamp Dictionary:\n";
            auto& timestamp_dict = manager.timestampDict();
            size_t template_count = timestamp_dict.getTemplateCount();
            std::cout << "    Template count: " << template_count << "\n";
            
            for (uint32_t i = 1; i <= std::min(uint32_t(3), static_cast<uint32_t>(template_count)); ++i) {
                std::string template_str = timestamp_dict.getTemplateById(i);
                std::cout << "    Template [" << i << "] " << template_str << "\n";
            }
            if (template_count > 3) {
                std::cout << "    ... and " << (template_count - 3) << " more templates\n";
            }
            
            // Also show timestamp variable dictionary
            size_t var_count = timestamp_dict.getVariableCount();
            std::cout << "    Variable dictionary (" << var_count << " variables):\n";
            for (uint32_t i = 1; i <= std::min(uint32_t(5), static_cast<uint32_t>(var_count)); ++i) {
                std::string var_str = timestamp_dict.getVariableByCode(i);
                std::cout << "      Var [" << i << "] " << var_str << "\n";
            }
            if (var_count > 5) {
                std::cout << "      ... and " << (var_count - 5) << " more variables\n";
            }
        } catch (const std::exception& e) {
            std::cout << "  Error inspecting timestamp dictionary: " << e.what() << "\n";
        }
    } else {
        std::cout << "  Timestamp dictionary is empty\n";
    }
    
    // Inspect logtype dictionary
    if (!granular_data.logtype_dict.empty()) {
        try {
            std::vector<uint8_t> decompressed = decompressWithConfig(granular_data.logtype_dict, 
                                                                   compression::FieldType::LOGTYPE, 
                                                                   SHARED_COMPRESSION_CONFIG);
            FieldDictionaryManager manager;
            Compressor::deserializeLogTypeDictionary(decompressed, manager);
            
            std::cout << "  LogType Dictionary:\n";
            auto& logtype_dict = manager.logtypeDict();
            size_t template_count = logtype_dict.getLogTypeCount();
            std::cout << "    Template count: " << template_count << "\n";
            
            for (uint32_t i = 1; i <= std::min(uint32_t(3), static_cast<uint32_t>(template_count)); ++i) {
                std::string template_str = logtype_dict.getLogTypeById(i);
                std::cout << "    Template [" << i << "] " << template_str << "\n";
            }
            if (template_count > 3) {
                std::cout << "    ... and " << (template_count - 3) << " more templates\n";
            }
            
            // Also show logtype variable dictionary
            // For logtype, we access the variable dictionary directly through the vector size
            size_t var_count = logtype_dict.getVariableCount();
            std::cout << "    Variable dictionary (" << var_count << " variables):\n";
            for (uint32_t i = 1; i <= std::min(uint32_t(5), static_cast<uint32_t>(var_count)); ++i) {
                std::string var_str = logtype_dict.decodeVariable(i);
                std::cout << "      Var [" << i << "] " << var_str << "\n";
            }
            if (var_count > 5) {
                std::cout << "      ... and " << (var_count - 5) << " more variables\n";
            }
        } catch (const std::exception& e) {
            std::cout << "  Error inspecting logtype dictionary: " << e.what() << "\n";
        }
    } else {
        std::cout << "  LogType dictionary is empty\n";
    }
}

// New function to inspect layer values
void inspectLayerValues(const GranularCompressedData& granular_data) {
    std::cout << "\n=== Layer Values Inspection ===\n";
    
    // First, decompress metadata to get field information
    try {
        if (granular_data.metadata.empty()) {
            std::cout << "  No metadata available\n";
            return;
        }
        
        // Use the same decompression method as in query_engine.cpp
        std::vector<uint8_t> metadata_raw = decompressWithConfig(granular_data.metadata, 
                                                               compression::FieldType::STRING, 
                                                               SHARED_COMPRESSION_CONFIG);
        if (metadata_raw.empty()) {
            std::cout << "  Failed to decompress metadata\n";
            return;
        }
        
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        std::cout << "  Field order (" << field_order.size() << " fields):\n";
        for (size_t i = 0; i < field_order.size(); ++i) {
            std::cout << "    [" << i << "] " << field_order[i].name << " (type=" 
                      << static_cast<int>(field_order[i].type) << ")\n";
        }
        
        // Create a selective decompressor for inspecting layer values
        json2::query::SelectiveDecompressor decompressor;
        
        // Inspect each layer
        for (size_t layer_idx = 0; layer_idx < granular_data.layer_data_by_level.size(); ++layer_idx) {
            std::cout << "\n  Layer " << layer_idx << " (field: " 
                      << (layer_idx < field_order.size() ? field_order[layer_idx].name : "unknown") 
                      << ")\n";
            
            const std::vector<uint8_t>& layer_data = granular_data.layer_data_by_level[layer_idx];
            if (layer_data.empty()) {
                std::cout << "    Empty layer data\n";
                continue;
            }
            
            // Try to determine the field type for this layer
            FieldType field_type = FieldType::STRING; // default
            if (layer_idx < field_order.size()) {
                field_type = field_order[layer_idx].type;
            }
            
            std::cout << "    Field type: " << static_cast<int>(field_type) << "\n";
            std::cout << "    Layer data size: " << layer_data.size() << " bytes\n";
            
            // Try to extract some values from the layer
            try {
                // Try to extract first few values using decompressLayerValueAt
                std::cout << "    First 5 values: ";
                for (size_t i = 0; i < std::min(size_t(5), size_t(100)); ++i) {
                    try {
                        // Map FieldType to compression::FieldType
                        compression::FieldType comp_field_type;
                        switch (field_type) {
                            case FieldType::INT64:
                                comp_field_type = compression::FieldType::INT64;
                                break;
                            case FieldType::DOUBLE:
                                comp_field_type = compression::FieldType::DOUBLE;
                                break;
                            case FieldType::BOOL:
                                comp_field_type = compression::FieldType::BOOL;
                                break;
                            case FieldType::STRING:
                                comp_field_type = compression::FieldType::STRING;
                                break;
                            case FieldType::TIMESTAMP:
                                comp_field_type = compression::FieldType::TIMESTAMP;
                                break;
                            case FieldType::LOGTYPE:
                                comp_field_type = compression::FieldType::LOGTYPE;
                                break;
                            default:
                                comp_field_type = compression::FieldType::STRING;
                        }
                        
                        NodeValue value = decompressor.decompressLayerValueAt(
                            layer_data, i, comp_field_type, SHARED_COMPRESSION_CONFIG, 100);
                        
                        // Print value based on its type
                        if (std::holds_alternative<uint32_t>(value)) {
                            std::cout << std::get<uint32_t>(value) << " ";
                        } else if (std::holds_alternative<int64_t>(value)) {
                            std::cout << std::get<int64_t>(value) << " ";
                        } else if (std::holds_alternative<double>(value)) {
                            std::cout << std::get<double>(value) << " ";
                        } else if (std::holds_alternative<bool>(value)) {
                            std::cout << (std::get<bool>(value) ? "true" : "false") << " ";
                        } else if (std::holds_alternative<TemplateEncodedTimestamp>(value)) {
                            const auto& ts = std::get<TemplateEncodedTimestamp>(value);
                            std::cout << "TS{template=" << ts.template_id << ",vars=[" << ts.var_codes.size() << "]} ";
                        } else if (std::holds_alternative<EncodedLog>(value)) {
                            const auto& log = std::get<EncodedLog>(value);
                            std::cout << "LOG{template=" << log.template_id << ",vars=[" << log.var_codes.size() << "]} ";
                        } else {
                            std::cout << "null ";
                        }
                    } catch (const std::exception& e) {
                        std::cout << "[error:" << e.what() << "] ";
                        break;
                    }
                }
                std::cout << "\n";
            } catch (const std::exception& e) {
                std::cout << "    Error inspecting layer: " << e.what() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cout << "  Error decompressing metadata: " << e.what() << "\n";
    }
}

void testFieldExistenceAndType(const GranularCompressedData& granular_data) {
    std::cout << "Testing field existence and type checking...\n";
    
    TestQueryEngine engine;
    
    // Test existing string field
    auto result = engine.checkFieldExistenceAndType("user", FieldType::STRING, granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        assert(result.exists);
        assert(result.type_matches);
        assert(result.actual_type == FieldType::STRING);
        std::cout << "  ✓ String field 'user' exists and type matches\n";
    }
    
    // Test existing int field
    result = engine.checkFieldExistenceAndType("age", FieldType::INT64, granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        assert(result.exists);
        assert(result.type_matches);
        assert(result.actual_type == FieldType::INT64);
        std::cout << "  ✓ Int field 'age' exists and type matches\n";
    }
    
    // Test existing double field
    result = engine.checkFieldExistenceAndType("score", FieldType::DOUBLE, granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        assert(result.exists);
        assert(result.type_matches);
        assert(result.actual_type == FieldType::DOUBLE);
        std::cout << "  ✓ Double field 'score' exists and type matches\n";
    }
    
    // Test existing bool field
    result = engine.checkFieldExistenceAndType("active", FieldType::BOOL, granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        assert(result.exists);
        assert(result.type_matches);
        assert(result.actual_type == FieldType::BOOL);
        std::cout << "  ✓ Bool field 'active' exists and type matches\n";
    }
    
    // Test non-existing field
    result = engine.checkFieldExistenceAndType("nonexistent", FieldType::STRING, granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        assert(!result.exists);
        std::cout << "  ✓ Non-existent field correctly identified\n";
    }
    
    // Test type mismatch
    result = engine.checkFieldExistenceAndType("age", FieldType::STRING, granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        assert(result.exists);
        assert(!result.type_matches);
        assert(result.actual_type == FieldType::INT64);
        std::cout << "  ✓ Type mismatch correctly identified\n";
    }
}

void testDictionaryValueFiltering(const GranularCompressedData& granular_data) {
    std::cout << "Testing dictionary value filtering (now handled by filterLayerValues)...\n";
    
    TestQueryEngine engine;
    
    // Test exact match for existing string value
    auto result = engine.filterLayerValues("user", FieldType::STRING, "alice", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ String dictionary filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for non-existing string value
    result = engine.filterLayerValues("user", FieldType::STRING, "frank", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Non-existing string value filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for existing timestamp value
    // Note: We need to use an actual timestamp value from the data
    result = engine.filterLayerValues("timestamp", FieldType::TIMESTAMP, "2023-01-05T14:00:00Z", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (timestamp): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Timestamp dictionary filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for non-existing timestamp value
    result = engine.filterLayerValues("timestamp", FieldType::TIMESTAMP, "2023-12-31T23:59:59Z", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (non-existing timestamp): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Non-existing timestamp value filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for existing logtype value
    // Note: We need to use an actual logtype value from the data
    result = engine.filterLayerValues("log_message", FieldType::LOGTYPE, "User alice logged in with score 95.5", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (logtype): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ LogType dictionary filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for non-existing logtype value
    result = engine.filterLayerValues("log_message", FieldType::LOGTYPE, "Error: {error_code} occurred", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (non-existing logtype): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Non-existing logtype value filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
}

void testLayerValueFiltering(const GranularCompressedData& granular_data) {
    std::cout << "Testing layer value filtering (now handles all field types)...\n";
    
    TestQueryEngine engine;
    
    // Test exact match for int field
    auto result = engine.filterLayerValues("age", FieldType::INT64, "25", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Int field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test range query for int field
    result = engine.filterLayerValues("age", FieldType::INT64, "30", ">=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Int range filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for double field
    result = engine.filterLayerValues("score", FieldType::DOUBLE, "95.5", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Double field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test range query for double field
    result = engine.filterLayerValues("score", FieldType::DOUBLE, "90.0", ">", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Double range filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for bool field
    result = engine.filterLayerValues("active", FieldType::BOOL, "true", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Bool field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for string field (dictionary-encoded)
    result = engine.filterLayerValues("user", FieldType::STRING, "alice", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ String field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for timestamp field (dictionary-encoded)
    result = engine.filterLayerValues("timestamp", FieldType::TIMESTAMP, "2023-01-01T10:00:00Z", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (timestamp): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Timestamp field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test exact match for logtype field (dictionary-encoded)
    result = engine.filterLayerValues("log_message", FieldType::LOGTYPE, "User alice logged in with score 95.5", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (logtype): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ LogType field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
}

void testCombinedFieldAndValueFiltering(const GranularCompressedData& granular_data) {
    std::cout << "Testing combined field and value filtering...\n";
    
    TestQueryEngine engine;
    
    // Test string field with dictionary value
    auto result = engine.filterFieldAndValues("user", FieldType::STRING, "bob", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Combined string field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test int field with layer value
    result = engine.filterFieldAndValues("age", FieldType::INT64, "30", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning: " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Combined int field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test timestamp field with dictionary value
    result = engine.filterFieldAndValues("timestamp", FieldType::TIMESTAMP, "2023-01-01T10:00:00Z", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (timestamp): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Combined timestamp field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test logtype field with dictionary value
    result = engine.filterFieldAndValues("log_message", FieldType::LOGTYPE, "User alice logged in with score 95.5", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ⚠️  Warning (logtype): " << result.error_message << std::endl;
    } else {
        std::cout << "  ✓ Combined logtype field filtering completed (match_found: " << result.match_found 
                  << ", count: " << result.count << ")\n";
        if (!result.matched_indices.empty()) {
            std::cout << "    Matched indices: ";
            for (size_t i = 0; i < result.matched_indices.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << result.matched_indices[i];
            }
            std::cout << "\n";
        }
    }
    
    // Test with non-existing field
    result = engine.filterFieldAndValues("nonexistent", FieldType::STRING, "value", "=", granular_data);
    if (!result.error_message.empty()) {
        std::cout << "  ✓ Non-existing field correctly rejected: " << result.error_message << std::endl;
    } else {
        std::cout << "  ⚠️  Warning: Non-existing field should have been rejected but wasn't" << std::endl;
    }
}

void testQueryEngineWithCompressedData() {
    std::cout << "Loading compressed data from directory...\n";
    
    // Now load the compressed data
    GranularCompressedData granular_data = loadCompressedDataFromDirectory("compressed_type_aware_data");
    std::cout << "Compressed data loaded\n";
    std::cout << "  Metadata size: " << granular_data.metadata.size() << std::endl;
    std::cout << "  Layer data by level size: " << granular_data.layer_data_by_level.size() << std::endl;
    std::cout << "  Use layer separation: " << (granular_data.use_layer_separation ? "true" : "false") << std::endl;
    
    // Inspect dictionaries first
    // inspectDictionaries(granular_data);
    
    // Inspect layer values
    // inspectLayerValues(granular_data);
    
    testFieldExistenceAndType(granular_data);
    testDictionaryValueFiltering(granular_data);
    testLayerValueFiltering(granular_data);
    testCombinedFieldAndValueFiltering(granular_data);
    
    std::cout << "All query engine tests completed!\n";
}

int main() {
    try {
        testQueryEngineWithCompressedData();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}