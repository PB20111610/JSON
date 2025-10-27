#include "../include/query/query_engine.h"
#include "../include/chunked_type_aware_compress.h"
#include "../include/compress.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <getopt.h>
#include <limits>

using namespace json2;
using namespace json2::query;

// Query test class that encapsulates all query functionality
class ParameterizedQueryTest {
private:
    QueryEngine engine_;
    std::vector<ChunkedTypeAwareBlock> blocks_;
    std::vector<GranularCompressedData> granular_data_;
    
public:
    ParameterizedQueryTest() {
        // Create a custom query config with unlimited max_results
        QueryConfig config;
        config.max_results = std::numeric_limits<size_t>::max();  // Set to maximum value for unlimited results
        config.enable_parallel = false;
        config.prune_only = false;
        config.sample_limit = 10;
        
        engine_ = QueryEngine(config);
    }
    
    // Method to set the data directory for the query engine
    void setDataDirectory(const std::string& data_dir) {
        engine_.setDataDirectory(data_dir);
    }
    
    // Load compressed data from directory
    bool loadCompressedData(const std::string& data_dir) {
        try {
            ChunkedTypeAwareCompressor::SelectiveLoadOptions load_options;
            
            // Use the correct compression configuration that matches the one used during compression
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
            
            blocks_ = ChunkedTypeAwareCompressor::loadFromDirectorySelective(
                data_dir, load_options, config);
            
            if (blocks_.empty()) {
                std::cerr << "Error: No compressed blocks loaded" << std::endl;
                return false;
            }
            
            // Rebuild GranularCompressedData from compressed data
            granular_data_.clear();
            for (size_t i = 0; i < blocks_.size(); ++i) {
                GranularCompressedData gdata = loadGranularDataFromDirectory(data_dir, i);
                if (!gdata.metadata.empty()) {
                    granular_data_.push_back(std::move(gdata));
                }
            }
            
            std::cout << "Successfully loaded " << blocks_.size() << " compressed blocks" << std::endl;
            std::cout << "Successfully reconstructed " << granular_data_.size() << " granular data blocks" << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to load compressed data: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Execute parameterized query based on type and parameters
    void executeQuery(const std::string& query_type, const std::vector<std::string>& params) {
        if (blocks_.empty()) {
            std::cout << "No compressed data loaded. Please load data first." << std::endl;
            return;
        }
        
        std::cout << "\n=== Executing " << query_type << " Query ===" << std::endl;
        
        try {
            if (query_type == "field") {
                executeFieldExistenceQuery(params);
            } else if (query_type == "dict") {
                executeDictionaryQuery(params);
            } else if (query_type == "point") {
                executeExactMatchQuery(params);
            } else if (query_type == "range") {
                executeRangeQuery(params);
            } else if (query_type == "aggregate") {
                executeAggregateQuery(params);
            } else if (query_type == "complex") {
                executeComplexQuery(params);
            } else if (query_type == "group") {
                executeGroupByQuery(params);
            } else {
                std::cout << "Unknown query type: " << query_type << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Query execution failed: " << e.what() << std::endl;
        }
    }

private:
    // Load granular data from directory (same as in the original test)
    GranularCompressedData loadGranularDataFromDirectory(const std::string& data_dir, size_t block_index) {
        GranularCompressedData gdata;
        
        char chunk_dir_buf[256];
        snprintf(chunk_dir_buf, sizeof(chunk_dir_buf), "%s/chunks/chunk_%06zu", data_dir.c_str(), block_index);
        std::string block_dir = chunk_dir_buf;
        
        if (!std::filesystem::exists(block_dir)) {
            return gdata;
        }
        
        try {
            // Read block metadata
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
            
            // Read Trie bitmap
            std::ifstream trie_file(block_dir + "/louds.json2", std::ios::binary);
            if (trie_file.is_open()) {
                uint32_t trie_size;
                trie_file.read(reinterpret_cast<char*>(&trie_size), sizeof(trie_size));
                gdata.trie_bitmap.resize(trie_size);
                trie_file.read(reinterpret_cast<char*>(gdata.trie_bitmap.data()), trie_size);
                trie_file.close();
            }
            
            // Read dictionary data
            std::string dict_dir = block_dir + "/dictionaries";
            
            // String dictionary
            std::ifstream string_file(dict_dir + "/variables.json2", std::ios::binary);
            if (string_file.is_open()) {
                uint32_t string_size;
                string_file.read(reinterpret_cast<char*>(&string_size), sizeof(string_size));
                gdata.string_dict.resize(string_size);
                string_file.read(reinterpret_cast<char*>(gdata.string_dict.data()), string_size);
                string_file.close();
            }
            
            // Timestamp dictionary
            std::ifstream ts_file(dict_dir + "/timestamps.json2", std::ios::binary);
            if (ts_file.is_open()) {
                uint32_t ts_size;
                ts_file.read(reinterpret_cast<char*>(&ts_size), sizeof(ts_size));
                gdata.timestamp_dict.resize(ts_size);
                ts_file.read(reinterpret_cast<char*>(gdata.timestamp_dict.data()), ts_size);
                ts_file.close();
            }
            
            // LogType dictionary
            std::ifstream log_file(dict_dir + "/logtypes.json2", std::ios::binary);
            if (log_file.is_open()) {
                uint32_t log_size;
                log_file.read(reinterpret_cast<char*>(&log_size), sizeof(log_size));
                gdata.logtype_dict.resize(log_size);
                log_file.read(reinterpret_cast<char*>(gdata.logtype_dict.data()), log_size);
                log_file.close();
            }
            
            // Granular compression metadata file
            std::ifstream granular_metadata_file(block_dir + "/metadata.json2", std::ios::binary);
            if (granular_metadata_file.is_open()) {
                uint32_t metadata_size;
                granular_metadata_file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
                gdata.metadata.resize(metadata_size);
                granular_metadata_file.read(reinterpret_cast<char*>(gdata.metadata.data()), metadata_size);
                granular_metadata_file.close();
            }
            
            // Read layer size information (if exists)
            std::ifstream layer_sizes_file(block_dir + "/layer_sizes.json2", std::ios::binary);
            if (layer_sizes_file.is_open()) {
                uint32_t layer_sizes_size;
                layer_sizes_file.read(reinterpret_cast<char*>(&layer_sizes_size), sizeof(layer_sizes_size));
                gdata.layer_sizes.resize(layer_sizes_size);
                layer_sizes_file.read(reinterpret_cast<char*>(gdata.layer_sizes.data()), layer_sizes_size);
                layer_sizes_file.close();
            }
            
            // Read layer data
            if (gdata.use_layer_separation) {
                // Read by layer
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
                // Read as a whole
                std::ifstream layers_file(block_dir + "/layers.json2", std::ios::binary);
                if (layers_file.is_open()) {
                    uint32_t layers_size;
                    layers_file.read(reinterpret_cast<char*>(&layers_size), sizeof(layers_size));
                    gdata.layer_data_combined.resize(layers_size);
                    layers_file.read(reinterpret_cast<char*>(gdata.layer_data_combined.data()), layers_size);
                    layers_file.close();
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Failed to load granular data: " << e.what() << std::endl;
        }
        
        return gdata;
    }
    
    // Field existence query (--field field_name)
    void executeFieldExistenceQuery(const std::vector<std::string>& params) {
        if (params.empty()) {
            std::cout << "Field existence query requires a field name parameter" << std::endl;
            return;
        }
        
        std::string field_name = params[0];
        std::cout << "Checking existence of field: " << field_name << std::endl;
        
        // Process all chunks
        for (size_t chunk_idx = 0; chunk_idx < granular_data_.size(); ++chunk_idx) {
            std::cout << "\n--- Processing chunk " << chunk_idx << " ---" << std::endl;
            const auto& granular_data = granular_data_[chunk_idx];
            
            auto start = std::chrono::high_resolution_clock::now();
            
            auto result = engine_.checkFieldExistenceAndType(
                field_name, FieldType::String, granular_data);
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration<double, std::milli>(end - start);
            
            std::cout << "Field: " << field_name << std::endl;
            std::cout << "  Exists: " << (result.exists ? "Yes" : "No") << std::endl;
            if (result.exists) {
                std::cout << "  Type: " << static_cast<int>(result.field_type) << std::endl;
                std::cout << "  Type matches: " << (result.type_matches ? "Yes" : "No") << std::endl;
            }
            std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                      << duration.count() << " ms" << std::endl;
            if (!result.error_message.empty()) {
                std::cout << "  Error: " << result.error_message << std::endl;
            }
            std::cout << std::endl;
        }
    }
    
    // Dictionary query (--dict field_name)
    void executeDictionaryQuery(const std::vector<std::string>& params) {
        if (params.empty()) {
            std::cout << "Dictionary query requires a field name parameter" << std::endl;
            return;
        }
        
        std::string field_name = params[0];
        std::cout << "Querying dictionary for field: " << field_name << std::endl;
        
        // Process all chunks
        for (size_t chunk_idx = 0; chunk_idx < granular_data_.size(); ++chunk_idx) {
            std::cout << "\n--- Processing chunk " << chunk_idx << " ---" << std::endl;
            const auto& granular_data = granular_data_[chunk_idx];
            
            std::cout << "Dictionary query for " << field_name << ":" << std::endl;
            auto start = std::chrono::high_resolution_clock::now();
            auto result = engine_.queryDictionary(
                field_name, FieldType::String, granular_data, "");
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration<double, std::milli>(end - start);
            
            std::cout << "  Found: " << (result.found ? "Yes" : "No") << std::endl;
            std::cout << "  Value count: " << result.values.size() << std::endl;
            std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                      << duration.count() << " ms" << std::endl;
            if (!result.error_message.empty()) {
                std::cout << "  Error: " << result.error_message << std::endl;
            } else {
                // Output first few values for debugging
                std::cout << "  First few values: ";
                for (size_t i = 0; i < std::min(size_t(5), result.values.size()); ++i) {
                    std::cout << "\"" << result.values[i] << "\" ";
                }
                std::cout << std::endl;
            }
        }
    }
    
    // Exact match query (--point field_name value)
    void executeExactMatchQuery(const std::vector<std::string>& params) {
        if (params.size() < 2) {
            std::cout << "Exact match query requires field name and value parameters" << std::endl;
            return;
        }
        
        std::string field_name = params[0];
        std::string value = params[1];
        std::cout << "Executing exact match query: " << field_name << " = \"" << value << "\"" << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeExactMatchQueryMultiBlock(
            field_name, value, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  Records found: " << result.count << std::endl;
        std::cout << "  Chunks accessed: " << result.chunks_accessed << std::endl;
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  Error: " << result.error_message << std::endl;
        } else {
            // Show first few matching records
            std::cout << "  Matching record examples:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
    
    // Range query (--range field_name lower_bound upper_bound)
    void executeRangeQuery(const std::vector<std::string>& params) {
        if (params.size() < 3) {
            std::cout << "Range query requires field name, lower bound, and upper bound parameters" << std::endl;
            return;
        }
        
        std::string field_name = params[0];
        std::string lower_bound = params[1];
        std::string upper_bound = params[2];
        std::cout << "Executing range query: " << field_name << " in [" << lower_bound << ", " << upper_bound << "]" << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeRangeQueryMultiBlock(
            field_name, lower_bound, upper_bound, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  Records found: " << result.count << std::endl;
        std::cout << "  Chunks accessed: " << result.chunks_accessed << std::endl;
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  Error: " << result.error_message << std::endl;
        } else {
            // Show first few matching records
            std::cout << "  Matching record examples:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
    
    // Aggregate query (--aggregate function field_name)
    void executeAggregateQuery(const std::vector<std::string>& params) {
        if (params.size() < 1) {
            std::cout << "Aggregate query requires at least a function parameter" << std::endl;
            return;
        }
        
        std::string function = params[0];
        std::string field_name = (params.size() > 1) ? params[1] : "";
        
        std::cout << "Executing aggregate query: " << function << "(" << (field_name.empty() ? "*" : field_name) << ")" << std::endl;
        
        AggregateFunction agg_func;
        if (function == "COUNT") {
            agg_func = AggregateFunction::COUNT;
        } else if (function == "SUM") {
            agg_func = AggregateFunction::SUM;
        } else if (function == "AVG") {
            agg_func = AggregateFunction::AVG;
        } else if (function == "MAX") {
            agg_func = AggregateFunction::MAX;
        } else if (function == "MIN") {
            agg_func = AggregateFunction::MIN;
        } else {
            std::cout << "Unknown aggregate function: " << function << std::endl;
            return;
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeAggregateQueryMultiBlock(
            agg_func, field_name, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  Aggregate value: " << result.value << std::endl;
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  Error: " << result.error_message << std::endl;
        }
    }
    
    // Complex query (--complex query_expression)
    void executeComplexQuery(const std::vector<std::string>& params) {
        if (params.empty()) {
            std::cout << "Complex query requires a query expression parameter" << std::endl;
            return;
        }
        
        // Join all parameters to form the complex query expression
        std::string query_expression;
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) query_expression += " ";
            query_expression += params[i];
        }
        
        std::cout << "Executing complex query: " << query_expression << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeComplexQueryMultiBlock(
            query_expression, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  Records found: " << result.count << std::endl;
        std::cout << "  Chunks accessed: " << result.chunks_accessed << std::endl;
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  Error: " << result.error_message << std::endl;
        } else {
            // Show first few matching records
            std::cout << "  Matching record examples:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
    
    // Group by query (--group field_names...)
    void executeGroupByQuery(const std::vector<std::string>& params) {
        if (params.empty()) {
            std::cout << "Group by query requires at least one field name parameter" << std::endl;
            return;
        }
        
        std::cout << "Executing group by query on fields: ";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << params[i];
        }
        std::cout << std::endl;
        
        // Try the direct grouped aggregate query method first (more reliable)
        std::cout << "Using direct grouped aggregate query method..." << std::endl;
        std::vector<AggregateFunction> agg_funcs = {AggregateFunction::COUNT};
        std::vector<std::string> agg_fields = {""}; // COUNT(*) uses empty field name
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeGroupedAggregateQueryMultiBlock(agg_funcs, agg_fields, params, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  Total records: " << result.total_count << std::endl;
        std::cout << "  Groups found: " << result.groups_count << std::endl;
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  Error: " << result.error_message << std::endl;
            // Fall back to complex query method if direct method fails
            std::cout << "Falling back to complex query method..." << std::endl;
            executeGroupByQueryFallback(params);
        } else {
            // Show first few group results
            std::cout << "  Group examples:" << std::endl;
            size_t count = 0;
            for (const auto& group_entry : result.grouped_values) {
                if (count >= 5) break;
                
                const std::vector<std::string>& group_values = group_entry.first;
                const auto& agg_values = group_entry.second;
                
                std::cout << "    Group: ";
                for (size_t i = 0; i < std::min(params.size(), group_values.size()); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << params[i] << "=" << group_values[i];
                }
                
                std::cout << " => ";
                for (const auto& agg_entry : agg_values) {
                    std::cout << agg_entry.first << "=" << agg_entry.second << " ";
                }
                std::cout << std::endl;
                
                count++;
            }
        }
    }
    
    // Fallback method using complex query string
    void executeGroupByQueryFallback(const std::vector<std::string>& params) {
        // Construct the proper GROUP BY query string following the pattern from the original test
        // Format: "COUNT(*) GROUP BY field1, field2, ..."
        std::string query_str = "COUNT(*) GROUP BY ";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) query_str += ", ";
            query_str += params[i];
        }
        
        std::cout << "Constructed fallback query: " << query_str << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeComplexQueryMultiBlock(query_str, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  Groups found: " << result.count << std::endl;
        std::cout << "  Chunks accessed: " << result.chunks_accessed << std::endl;
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  Error: " << result.error_message << std::endl;
        } else {
            // Show first few group results
            std::cout << "  Group examples:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(10), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
};

// Print usage information
void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] --data-dir DIRECTORY\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --field FIELD_NAME              Check field existence\n";
    std::cout << "  --dict FIELD_NAME               Query dictionary values\n";
    std::cout << "  --point FIELD_NAME VALUE        Exact match query\n";
    std::cout << "  --range FIELD_NAME LOW HIGH     Range query\n";
    std::cout << "  --aggregate FUNCTION [FIELD]    Aggregate query (COUNT, SUM, AVG, MAX, MIN)\n";
    std::cout << "  --complex QUERY_EXPRESSION      Complex query with logical operations\n";
    std::cout << "  --group FIELD_NAMES...          Group by query\n";
    std::cout << "  --data-dir DIRECTORY            Directory containing compressed data\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --field user\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --dict user\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --point user postgres\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --range pid 7880 7890\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --aggregate COUNT\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --aggregate SUM pid\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --complex \"user:postgres AND dbname:example\"\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --complex \"user:postgres AND pid > 7992\"\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --complex \"pid >= 8000 AND pid <= 9000\"\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --group user dbname\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --point message \"error occurred\"\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --point name \"John Doe\" --range city \"New York\" \"San Francisco\"\n";
    std::cout << "\nSupported operators in complex queries:\n";
    std::cout << "  =, !=, >, >=, <, <=, AND, OR, NOT\n";
    std::cout << "\nNote: Use quotes around values that contain spaces.\n";
    std::cout << "\nMultiple queries can be executed in sequence:\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --field user --point user postgres --aggregate COUNT\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string data_directory = "compressed_type_aware_data"; // Default directory
    ParameterizedQueryTest tester;
    
    // Parse command line arguments
    std::vector<std::pair<std::string, std::vector<std::string>>> queries;
    std::string current_query_type;
    std::vector<std::string> current_query_params;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--data-dir") {
            if (i + 1 < argc) {
                data_directory = argv[++i];
            } else {
                std::cerr << "Error: --data-dir requires a directory argument" << std::endl;
                return 1;
            }
        } else if (arg == "--field" || arg == "--dict" || arg == "--point" || 
                   arg == "--range" || arg == "--aggregate" || arg == "--complex" || 
                   arg == "--group") {
            // Save previous query if exists
            if (!current_query_type.empty()) {
                queries.push_back({current_query_type, current_query_params});
                current_query_type.clear();
                current_query_params.clear();
            }
            
            // Set new query type
            if (arg == "--field") current_query_type = "field";
            else if (arg == "--dict") current_query_type = "dict";
            else if (arg == "--point") current_query_type = "point";
            else if (arg == "--range") current_query_type = "range";
            else if (arg == "--aggregate") current_query_type = "aggregate";
            else if (arg == "--complex") current_query_type = "complex";
            else if (arg == "--group") current_query_type = "group";
        } else if (!current_query_type.empty()) {
            // Add parameter to current query
            // Handle quoted strings for parameters with spaces
            if (!arg.empty() && arg.front() == '"' && arg.back() != '"') {
                // Start of a quoted string, accumulate until we find the closing quote
                std::string quoted_arg = arg.substr(1); // Remove opening quote
                while (i + 1 < argc) {
                    std::string next_arg = argv[++i];
                    if (!next_arg.empty() && next_arg.back() == '"') {
                        // Found closing quote
                        quoted_arg += " " + next_arg.substr(0, next_arg.length() - 1); // Remove closing quote
                        break;
                    } else {
                        quoted_arg += " " + next_arg;
                    }
                }
                current_query_params.push_back(quoted_arg);
            } else if (!arg.empty() && arg.front() == '"' && arg.back() == '"') {
                // Fully quoted argument
                current_query_params.push_back(arg.substr(1, arg.length() - 2));
            } else {
                // Regular argument
                current_query_params.push_back(arg);
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    // Save last query if exists
    if (!current_query_type.empty()) {
        queries.push_back({current_query_type, current_query_params});
    }
    
    if (queries.empty()) {
        std::cerr << "No queries specified. Use --help for usage information." << std::endl;
        return 1;
    }
    
    std::cout << "Parameterized Query Test" << std::endl;
    std::cout << "Data directory: " << data_directory << std::endl;
    
    if (!std::filesystem::exists(data_directory)) {
        std::cerr << "Error: Data directory does not exist: " << data_directory << std::endl;
        return 1;
    }
    
    // Set the data directory for the query engine
    tester.setDataDirectory(data_directory);
    
    // Load compressed data
    if (!tester.loadCompressedData(data_directory)) {
        std::cerr << "Failed to load compressed data" << std::endl;
        return 1;
    }
    
    // Execute all queries
    for (const auto& query : queries) {
        tester.executeQuery(query.first, query.second);
    }
    
    std::cout << "\nQuery testing completed!" << std::endl;
    return 0;
}