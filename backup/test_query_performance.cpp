#include "../include/query/query_engine.h"
#include "../include/chunked_type_aware_compress.h"
#include "../include/compress.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include "../include/reconstruct.h"
#include "../include/field_dictionary_manager.h"
#include "../include/timestamp_dictionary.h"
#include "../include/logtype_dictionary.h"
#include "../test/test_config_utils.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <thread>

using namespace json2;
using namespace json2::query;

class QueryTest {
private:
    QueryEngine engine_;
    std::vector<ChunkedTypeAwareBlock> blocks_;
    std::vector<GranularCompressedData> granular_data_;
    
public:
    QueryTest() : engine_(QueryConfig{
        true,   // enable_caching
        1000,   // max_cache_entries
        false   // enable_parallel_processing
    }) {
    }
    
    // Method to set the data directory for the query engine
    void setDataDirectory(const std::string& data_dir) {
        engine_.setDataDirectory(data_dir);
    }
    
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
            config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_VARINT;
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
            
            // Reconstruct GranularCompressedData from compressed data
            granular_data_.clear();
            for (size_t i = 0; i < blocks_.size(); ++i) {
                GranularCompressedData gdata = loadGranularDataFromDirectory(data_dir, i);
                if (!gdata.metadata.empty()) {
                    granular_data_.push_back(std::move(gdata));
                }
            }
            
            std::cout << "Successfully loaded " << blocks_.size() << " compressed blocks" << std::endl;
            std::cout << "Successfully reconstructed " << granular_data_.size() << " granular data" << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to load compressed data: " << e.what() << std::endl;
            return false;
        }
    }
    
private:
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
            
            // Granular compressed metadata file
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
                // Read by layer separately
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
    
public:
    void testFieldExistenceQueries() {
        std::cout << "\n=== Field Existence Query Test ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "No compressed block data available" << std::endl;
            return;
        }
        
        std::vector<std::string> test_fields = {
            "timestamp", "user", "dbname", "pid", "session_id", 
            "error_severity", "message", "application_name"
        };
        
        // Process all chunks
        for (size_t chunk_idx = 0; chunk_idx < granular_data_.size(); ++chunk_idx) {
            std::cout << "\n--- Processing chunk " << chunk_idx << " ---" << std::endl;
            const auto& granular_data = granular_data_[chunk_idx];
            
            for (const auto& field_name : test_fields) {
                auto start = std::chrono::high_resolution_clock::now();
                
                // Call checkFieldExistenceAndType method with correct parameters
                auto result = engine_.checkFieldExistenceAndType(
                    field_name, FieldType::STRING, granular_data, nullptr);
                
                auto end = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration<double, std::milli>(end - start);
                
                std::cout << "Field: " << field_name << std::endl;
                std::cout << "  Exists: " << (result.exists ? "Yes" : "No") << std::endl;
                if (result.exists) {
                    std::cout << "  Type: " << static_cast<int>(result.actual_type) << std::endl;
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
    }
    
    void testExactMatchQueries() {
        std::cout << "\n=== Exact Match Query Test ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "No compressed block data available" << std::endl;
            return;
        }
        
        // Test exact match query - Find user = "postgres" (using all blocks)
        std::cout << "\nExact match query (user = \"postgres\"):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeExactMatchQueryMultiBlock(
            "user", "postgres", FieldType::STRING, blocks_);
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
        
        // Test exact match query - Find dbname = "example" (using all blocks)
        std::cout << "\nExact match query (dbname = \"example\"):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeExactMatchQueryMultiBlock(
            "dbname", "example", FieldType::STRING, blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
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
        
        // Test exact match query - Find application_name = "pgbench" (using all blocks)
        std::cout << "\nExact match query (application_name = \"pgbench\"):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeExactMatchQueryMultiBlock(
            "application_name", "pgbench", FieldType::STRING, blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
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
        
        // Test single block exact match query
        if (!granular_data_.empty()) {
            std::cout << "\nSingle block exact match query (user = \"postgres\"):" << std::endl;
            const auto& granular_data = granular_data_[0];
            start = std::chrono::high_resolution_clock::now();
            auto single_result = engine_.executeExactMatchQuery(
                "user", "postgres", FieldType::STRING, granular_data);
            end = std::chrono::high_resolution_clock::now();
            duration = std::chrono::duration<double, std::milli>(end - start);
            
            std::cout << "  Records found: " << single_result.count << std::endl;
            std::cout << "  Query time: " << std::fixed << std::setprecision(2) 
                      << duration.count() << " ms" << std::endl;
            if (!single_result.error_message.empty()) {
                std::cout << "  Error: " << single_result.error_message << std::endl;
            } else {
                // Show first few matching records
                std::cout << "  Matching record examples:" << std::endl;
                for (size_t i = 0; i < std::min(size_t(3), single_result.records.size()); ++i) {
                    std::cout << "    " << single_result.records[i] << std::endl;
                }
            }
        }
    }
    
    void testRangeQueries() {
        std::cout << "\n=== Range Query Test ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "No compressed block data available" << std::endl;
            return;
        }
        
        // Process all chunks for initial checks
        for (size_t chunk_idx = 0; chunk_idx < granular_data_.size(); ++chunk_idx) {
            std::cout << "\n--- Processing chunk " << chunk_idx << " ---" << std::endl;
            const auto& granular_data = granular_data_[chunk_idx];
            
            // First check if field exists
            std::cout << "Checking field existence:" << std::endl;
            auto pid_existence = engine_.checkFieldExistenceAndType("pid", FieldType::INT64, granular_data);
            std::cout << "  pid field exists: " << (pid_existence.exists ? "Yes" : "No") << std::endl;
            if (pid_existence.exists) {
                std::cout << "  pid field type: " << static_cast<int>(pid_existence.actual_type) << std::endl;
            }
        }
        
        // Test range query - Find records with pid in range [7880, 7890] (using all blocks)
        std::cout << "\nRange query (pid in range [7880, 7890]):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeRangeQueryMultiBlock(
            "pid", "7880", "7890", FieldType::INT64, blocks_);
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
        
        // Test range query - Find records with timestamp in specific range (using all blocks)
        std::cout << "\nRange query (timestamp in range [\"2023-03-27 00:32:15.929 EDT\", \"2023-03-27 00:32:15.936 EDT\"]):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeRangeQueryMultiBlock(
            "timestamp", "2023-03-27 00:32:15.929 EDT", "2023-03-27 00:32:15.936 EDT", FieldType::TIMESTAMP, blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
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
};

int main() {
    const std::string compressed_data_dir = "compressed_type_aware_data";
    
    std::cout << "Query Performance Test" << std::endl;
    std::cout << "Compressed data directory: " << compressed_data_dir << std::endl;
    
    if (!std::filesystem::exists(compressed_data_dir)) {
        std::cerr << "Error: Compressed data directory does not exist: " << compressed_data_dir << std::endl;
        return 1;
    }
    
    QueryTest tester;
    
    // Set the data directory for the query engine
    // This is needed for granular data extraction from chunk directories
    tester.setDataDirectory(compressed_data_dir);
    
    // Load compressed data
    if (!tester.loadCompressedData(compressed_data_dir)) {
        std::cerr << "Failed to load compressed data" << std::endl;
        return 1;
    }
    
    // Run tests
    // tester.testFieldExistenceQueries();
    // tester.testExactMatchQueries();
    tester.testRangeQueries();

    std::cout << "\nTest completed!" << std::endl;
    return 0;
}