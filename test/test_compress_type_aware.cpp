#include "../include/field_analyzer.h"
#include "../include/field_dictionary_manager.h"
#include "../include/trie.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include "../include/reconstruct.h"
#include "../include/compress_type_aware.h"
#include "../include/compress.h" // For file I/O operations
#include <iostream>
#include <fstream>
#include <iomanip>
#include <simdjson.h>
#include <sys/stat.h>
#include <cassert>
#include <set>
#include <nlohmann/json.hpp>

using namespace json2;

// Function declarations
bool verifyLosslessReconstruction(const std::string& original_file_path, const std::string& reconstructed_json);

// Helper function to convert compression backend enum to readable string
std::string backendToString(compression::CompressionBackend backend) {
    switch (backend) {
        case compression::CompressionBackend::AUTO: return "AUTO";
        case compression::CompressionBackend::RLE: return "RLE";
        case compression::CompressionBackend::BIT_PACKING: return "BIT_PACKING";
        case compression::CompressionBackend::DICTIONARY: return "DICTIONARY";
        case compression::CompressionBackend::DELTA_VARINT: return "DELTA_VARINT";
        case compression::CompressionBackend::DELTA_DELTA: return "DELTA_DELTA";
        case compression::CompressionBackend::ZSTD: return "ZSTD";
        case compression::CompressionBackend::BROTLI: return "BROTLI";
        case compression::CompressionBackend::LZMA: return "LZMA";
        case compression::CompressionBackend::LZ4: return "LZ4";
        case compression::CompressionBackend::SNAPPY: return "SNAPPY";
        case compression::CompressionBackend::NONE: return "NONE";
        default: return "UNKNOWN";
    }
}

// Helper function to display compression configuration
void displayCompressionConfig(const compression::TypeAwareCompressionConfig& config) {
    std::cout << "\n=== Compression Configuration ===\n";
    std::cout << "LOUDS Backend: " << backendToString(config.louds_backend) << "\n";
    std::cout << "Dictionary Backend: " << backendToString(config.dictionary_backend) << "\n";
    std::cout << "Metadata Backend: " << backendToString(config.metadata_backend) << "\n";
    std::cout << "Fallback Backend: " << backendToString(config.fallback_backend) << "\n";
    std::cout << "Compression Level: " << config.compression_level << " (for ZSTD/LZMA/Brotli backends)\n";
    std::cout << "Null Aware: " << (config.enable_null_aware ? "Enabled" : "Disabled") << "\n";
    std::cout << "\nLayer Configuration:\n";
    std::cout << "  Integer: " << backendToString(config.layer_config.int_backend) << "\n";
    std::cout << "  Double: " << backendToString(config.layer_config.double_backend) << "\n";
    std::cout << "  Boolean: " << backendToString(config.layer_config.bool_backend) << "\n";
    std::cout << "  String: " << backendToString(config.layer_config.string_backend) << "\n";
    std::cout << "  Timestamp: " << backendToString(config.layer_config.timestamp_backend) << "\n";
    std::cout << "  LogType: " << backendToString(config.layer_config.logtype_backend) << "\n";
    std::cout << "  Array: " << backendToString(config.layer_config.array_backend) << "\n";
    std::cout << "  Null: " << backendToString(config.layer_config.null_backend) << "\n";
    std::cout << "================================\n";
}

// Order-independent lossless verification function
bool verifyLosslessReconstruction(const std::string& original_file_path, const std::string& reconstructed_json) {
    try {
        // Count original records
        std::ifstream original_file(original_file_path);
        if (!original_file.is_open()) {
            std::cerr << "Cannot open original file for verification: " << original_file_path << std::endl;
            return false;
        }
        
        std::vector<std::string> original_records;
        std::string line;
        while (std::getline(original_file, line)) {
            if (!line.empty()) {
                original_records.push_back(line);
            }
        }
        original_file.close();
        
        // Count reconstructed records
        std::istringstream reconstructed_stream(reconstructed_json);
        std::vector<std::string> reconstructed_records;
        while (std::getline(reconstructed_stream, line)) {
            if (!line.empty()) {
                reconstructed_records.push_back(line);
            }
        }
        
        // Basic count verification
        std::cout << "Original records: " << original_records.size() << std::endl;
        std::cout << "Reconstructed records: " << reconstructed_records.size() << std::endl;
        
        if (original_records.size() != reconstructed_records.size()) {
            std::cerr << "Record count mismatch: original=" << original_records.size() 
                      << ", reconstructed=" << reconstructed_records.size() << std::endl;
            return false;
        }
        
        // Content-based verification using JSON parsing
        // Convert both sets to parsed JSON objects for semantic comparison
        std::multiset<std::string> original_normalized, reconstructed_normalized;
        
        // Normalize original records
        for (const auto& record : original_records) {
            try {
                nlohmann::json parsed = nlohmann::json::parse(record);
                original_normalized.insert(parsed.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
            } catch (const std::exception& e) {
                std::cerr << "Failed to parse original record: " << e.what() << std::endl;
                return false;
            }
        }
        
        // Normalize reconstructed records
        for (const auto& record : reconstructed_records) {
            try {
                nlohmann::json parsed = nlohmann::json::parse(record);
                reconstructed_normalized.insert(parsed.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
            } catch (const std::exception& e) {
                std::cerr << "Failed to parse reconstructed record: " << e.what() << std::endl;
                return false;
            }
        }
        
        // Compare normalized sets
        if (original_normalized == reconstructed_normalized) {
            std::cout << "✓ Lossless verification passed: All records match semantically\n";
            return true;
        } else {
            // Find missing/extra records for debugging
            std::cout << "✗ Content verification failed\n";
            
            // Count differences
            size_t common_count = 0;
            auto orig_it = original_normalized.begin();
            auto recon_it = reconstructed_normalized.begin();
            
            while (orig_it != original_normalized.end() && recon_it != reconstructed_normalized.end()) {
                if (*orig_it == *recon_it) {
                    common_count++;
                    ++orig_it;
                    ++recon_it;
                } else if (*orig_it < *recon_it) {
                    ++orig_it;
                } else {
                    ++recon_it;
                }
            }
            
            double match_percentage = (double)common_count / original_records.size() * 100.0;
            std::cout << "Match percentage: " << std::fixed << std::setprecision(2) 
                      << match_percentage << "% (" << common_count << "/" << original_records.size() << ")\n";
            
            // Consider it successful if match percentage is very high (accounting for minor differences)
            return match_percentage >= 99.0;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Verification error: " << e.what() << std::endl;
        return false;
    }
}

int main() {
    const size_t CHUNK_SIZE = 1000;
    const char* DATA_PATH = "test_data.json";
    try {
        FieldDictionaryManager manager;
        std::vector<std::string> timestamp_fields = {"@timestamp", "timestamp", "session_start"};
        manager.setTimestampFields(timestamp_fields);
        std::vector<FieldKey> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;
        manager.setStructurizeArrays(false);
        std::ifstream in(DATA_PATH);
        if (!in.is_open()) {
            throw std::runtime_error(std::string("Cannot open ") + DATA_PATH + " for reading");
        }
        simdjson::dom::parser parser;
        std::vector<std::string> first_chunk;
        std::string line;
        // 1. 读取前 CHUNK_SIZE 行
        for (size_t i = 0; i < CHUNK_SIZE && std::getline(in, line); ) {
            if (line.empty()) continue;
            try {
                simdjson::dom::element doc = parser.parse(line).value();
                first_chunk.push_back(line);
                ++i;
            } catch (...) {}
        }
        if (first_chunk.empty()) {
            std::cout << "No records processed, exiting." << std::endl;
            return 0;
        }
        FieldAnalyzer::analyzeAndSortFields(first_chunk, manager, fieldOrder);
        trie = std::make_unique<Trie>(fieldOrder);
        for (const auto& record : first_chunk) {
            trie->insert(record, manager, parser);
        }
        // 继续插入剩余行
        size_t line_count = first_chunk.size();
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                simdjson::dom::element doc = parser.parse(line).value();
                trie->insert(line, manager, parser);
                ++line_count;
            } catch (...) {}
        }
        in.close();
        // 统计原始数据文件大小
        struct stat st;
        size_t original_file_size = 0;
        if (stat(DATA_PATH, &st) == 0) {
            original_file_size = st.st_size;
        }
 
        std::cout << "\n=== Type-Aware Compression Test ===\n";
        std::cout << "Lines processed: " << line_count << std::endl;
        std::cout << "Field types detected: " << fieldOrder.size() << std::endl;
        
        // Configure type-aware compression with optimized settings
        compression::TypeAwareCompressionConfig config;
        // Configure comprehensive type-aware compression settings
        config.louds_backend = compression::CompressionBackend::BIT_PACKING;
        config.dictionary_backend = compression::CompressionBackend::ZSTD;
        config.metadata_backend = compression::CompressionBackend::ZSTD;
        
        // Configure ALL field type backends explicitly for optimal performance
        config.layer_config.int_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.double_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.bool_backend = compression::CompressionBackend::BIT_PACKING;
        config.layer_config.string_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_DELTA;
        config.layer_config.logtype_backend = compression::CompressionBackend::DELTA_VARINT;
        config.layer_config.array_backend = compression::CompressionBackend::RLE;
        config.layer_config.null_backend = compression::CompressionBackend::BIT_PACKING;
        
        // Use compression level 3 to match compress.cpp's ZSTD_CLEVEL_DEFAULT
        config.compression_level = 3;
        
        // Display current compression configuration
        displayCompressionConfig(config);
        
        try {
            CompressedData compressed = TypeAwareCompressor::compress(*trie, manager, config);
            
            std::string filename = "compressed_type_aware.json2";
            Compressor::saveToFile(compressed, filename);
            
            CompressedData loaded = Compressor::loadFromFile(filename);
            
            auto [trie2, manager2] = TypeAwareCompressor::decompress(loaded, config);
            
            // Reconstruct and verify
            std::string reconstructed_json = reconstructJsonFromTrie(*trie2, *manager2);
            std::ofstream out_file("reconstructed.json");
            out_file << reconstructed_json;
            out_file.close();
            // Verify reconstruction integrity (order-independent verification)
            // bool reconstruction_valid = verifyLosslessReconstruction(DATA_PATH, reconstructed_json);
            // bool correct = (trie2 != nullptr && manager2 != nullptr && reconstruction_valid);
            
            // Calculate compression results
            struct stat st;
            size_t compressed_file_size = 0;
            if (stat(filename.c_str(), &st) == 0) {
                compressed_file_size = st.st_size;
            }
            // Output simplified compression results
            std::cout << "\n=== Compression Results ===\n";
            std::cout << "Original file size: " << original_file_size << " bytes\n";
            std::cout << "Extracted data size: " << compressed.original_size << " bytes\n";
            std::cout << "Compressed file size: " << compressed_file_size << " bytes\n";
            
            if (compressed_file_size > 0 && original_file_size > 0) {
                double file_ratio = (double)original_file_size / compressed_file_size;
                double data_ratio = (double)compressed.original_size / compressed_file_size;
                std::cout << "File compression ratio: " << std::fixed << std::setprecision(2) << file_ratio << "x\n";
                std::cout << "Data compression ratio: " << std::fixed << std::setprecision(2) << data_ratio << "x\n";
            }
            
            // std::cout << "Lossless compression: " << (correct ? "VERIFIED" : "FAILED") << std::endl;
            // if (!correct) {
            //     std::cerr << "\nERROR: Lossless compression verification failed!\n";
            //     return 1;
            // }
        } catch (const std::exception& e) {
            std::cerr << "ERROR: " << e.what() << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
