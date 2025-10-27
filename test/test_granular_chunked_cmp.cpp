#include "../include/chunked_compressor.h"
#include "../include/compress.h"
#include "../include/reconstruct.h"
#include <simdjson.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <set>
#include <sys/stat.h>
#include <cassert>
#include <unordered_map>
#include <algorithm>

#ifdef _WIN32
#include <direct.h> // For _mkdir on Windows
#endif

using namespace json2;

// Helper function to parse command-line arguments
std::unordered_map<std::string, std::string> parseArguments(int argc, char* argv[]) {
    std::unordered_map<std::string, std::string> args;
    
    // Set default values
    args["--test-file-path"] = "test_data.json";
    args["--block_size"] = "20000";
    args["--chunk_size"] = "1000";
    args["--num_limit"] = "0"; // 0 means no limit
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.substr(0, 2) == "--" && i + 1 < argc) {
            args[arg] = argv[i + 1];
            i++; // Skip the next argument as it's the value
        }
    }
    
    return args;
}

int main(int argc, char* argv[]) {
    // Parse command-line arguments
    auto args = parseArguments(argc, argv);
    
    const std::string input_file = args["--test-file-path"];
    const size_t BLOCK_SIZE = std::stoull(args["--block_size"]);
    const size_t CHUNK_SIZE = std::stoull(args["--chunk_size"]);
    
    // 设置记录数量限制，默认为0表示无限制
    size_t NUM_LIMIT = std::stoull(args["--num_limit"]);
    
    // 分块压缩（细粒度）
    CompressorConfig config;
    config.block_size = BLOCK_SIZE;
    config.chunk_size = CHUNK_SIZE;
    config.structurize_arrays = false;
    config.enable_granular_compression = true;
    config.enable_layer_separation = true;
    ChunkedTrieCompressor compressor(config);

    // 设置时间戳字段
    std::vector<std::string> timestamp_fields = {"@timestamp","request_received","response_delivered", "timestamp", "session_start"};
    compressor.setTimestampFields(timestamp_fields);
    compressor.enableGranularCompression(config.enable_granular_compression);
    compressor.enableLayerSeparation(config.enable_layer_separation);
    compressor.setStructurizeArrays(config.structurize_arrays);

    std::cout << "[CONFIG] Compression configuration:" << std::endl;
    std::cout << "  Input file: " << input_file << std::endl;
    std::cout << "  Block size: " << BLOCK_SIZE << std::endl;
    std::cout << "  Chunk size: " << CHUNK_SIZE << std::endl;
    std::cout << "  Granular compression: " << (compressor.isGranularCompressionEnabled() ? "enabled" : "disabled") << std::endl;
    std::cout << "  Layer separation: " << (compressor.isLayerSeparationEnabled() ? "enabled" : "disabled") << std::endl;
    if (NUM_LIMIT > 0) {
        std::cout << "  Record limit: " << NUM_LIMIT << std::endl;
    } else {
        std::cout << "  Record limit: unlimited" << std::endl;
    }

    // 设置原始文件大小
    struct stat st;
    size_t original_file_size = 0;
    if (stat(input_file.c_str(), &st) == 0) {
        original_file_size = st.st_size;
    }
    // compressor.setOriginalFileSize(original_file_size); // Will be set after processing

    // Process file in blocks to avoid high memory usage
    std::ifstream fin(input_file);
    if (!fin.is_open()) {
        std::cerr << "Cannot open input file: " << input_file << std::endl;
        return 1;
    }
    
    simdjson::dom::parser parser;
    size_t record_idx = 0;
    size_t total_records = 0;
    size_t processed_data_size = 0; // Track actual processed data size
    std::string line;
    
    // Read and process records in blocks
    while (std::getline(fin, line) && (NUM_LIMIT == 0 || total_records < NUM_LIMIT)) {
        if (!line.empty()) {
            // Track the actual size of data being processed
            processed_data_size += line.length() + 1; // +1 for newline character
            compressor.addRecord(line, parser);
            record_idx++;
            total_records++;
            
            // Print progress every block
            if (record_idx % BLOCK_SIZE == 0) {
                std::cout << "[DEBUG] Processed " << record_idx << " records so far..." << std::endl;
            }
        }
    }
    
    fin.close();
    
    // Set the original file size to the actual processed data size when limit is set
    if (NUM_LIMIT > 0) {
        compressor.setOriginalFileSize(processed_data_size);
        std::cout << "Processed " << total_records << " records from " << input_file << " (" << processed_data_size << " bytes)" << std::endl;
    } else {
        compressor.setOriginalFileSize(original_file_size);
        std::cout << "Processed " << total_records << " records from " << input_file << std::endl;
    }

    std::vector<uint8_t> compressed = compressor.serialize();
    std::cout << "Compressed data size: " << compressed.size() << " bytes" << std::endl;

    // 输出详细统计信息
    CompressionStats stats = compressor.getStats();
    std::cout << "[STATS] 原始数据大小: " << stats.original_file_size << " bytes" << std::endl;
    std::cout << "[STATS] 待压缩内容大小: " << stats.total_core_data_size << " bytes" << std::endl;
    std::cout << "[STATS] 压缩后内容大小: " << stats.total_compressed_size << " bytes" << std::endl;
    if (stats.total_compressed_size > 0) {
        std::cout << "[STATS] 原始/压缩比: " << (double)stats.original_file_size / stats.total_compressed_size << std::endl;
        std::cout << "[STATS] 待压缩/压缩比: " << (double)stats.total_core_data_size / stats.total_compressed_size << std::endl;
    }

    // Save statistics to result.json
    std::ofstream result_file("result_chunked.json");
    if (result_file.is_open()) {
        result_file << "{\n";
        result_file << "  \"original_file_size\": " << stats.original_file_size << ",\n";
        result_file << "  \"core_data_size\": " << stats.total_core_data_size << ",\n";
        result_file << "  \"compressed_size\": " << stats.total_compressed_size << ",\n";
        if (stats.total_compressed_size > 0) {
            result_file << "  \"compression_ratio_original\": " << (double)stats.original_file_size / stats.total_compressed_size << ",\n";
            result_file << "  \"compression_ratio_core\": " << (double)stats.total_core_data_size / stats.total_compressed_size << "\n";
        } else {
            result_file << "  \"compression_ratio_original\": 0,\n";
            result_file << "  \"compression_ratio_core\": 0\n";
        }
        result_file << "}\n";
        result_file.close();
        std::cout << "[INFO] Compression statistics saved to result.json" << std::endl;
    } else {
        std::cerr << "[ERROR] Failed to create result.json" << std::endl;
    }

    // 压缩并保存到目录
    std::string out_dir = "compressed_chunked_data";
    // 清理旧的压缩数据目录
    #ifdef _WIN32
        std::string cleanup_cmd = "rd /s /q " + out_dir + " 2>nul";
    #else
        std::string cleanup_cmd = "rm -rf " + out_dir;
    #endif
    int cleanup_result = system(cleanup_cmd.c_str());
    // Ignore cleanup result as it's not critical
    
    #ifdef _WIN32
        _mkdir(out_dir.c_str());
    #else
        mkdir(out_dir.c_str(), 0777);
    #endif
    if (compressor.saveToDirectory(out_dir)) {
        std::cout << "[INFO] Compressed data saved to directory: " << out_dir << std::endl;
    } else {
        std::cerr << "[ERROR] Failed to save compressed data to directory!" << std::endl;
        return 2;
    }

    // 从目录加载并解压
    std::cout << "[DEBUG] Start loading and deserializing blocks from directory..." << std::endl;
    auto blocks = ChunkedTrieCompressor::loadFromDirectory(out_dir);
    std::cout << "Block count: " << blocks.size() << std::endl;

    // 复原所有JSON
    std::vector<std::string> decompressed_jsons;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        std::string json_block;
        try {
            json_block = reconstructJsonFromTrie(*block.trie, *block.dict);
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] reconstructJsonFromTrie failed for block " << i << ": " << e.what() << std::endl;
            continue;
        }
        decompressed_jsons.push_back(json_block);
    }
    // 合并所有块的JSON
    std::ofstream fout("chunked_reconstructed.json");
    size_t total_lines = 0;
    for (const auto& block_json : decompressed_jsons) {
        std::istringstream iss(block_json);
        std::string l;
        while (std::getline(iss, l)) {
            fout << l << '\n';
            total_lines++;
        }
    }
    fout.close();
    std::cout << "Reconstructed JSON written to chunked_reconstructed.json, total lines: " << total_lines << std::endl;

    // 校验一致性（去重后的记录数）
    // For verification, we need to read the original file again since we didn't store all records in memory
    std::set<std::string> original_unique_records;
    std::ifstream fin_verify(input_file);
    std::string verify_line;
    size_t verify_count = 0;
    while (std::getline(fin_verify, verify_line) && (NUM_LIMIT == 0 || verify_count < NUM_LIMIT)) {
        if (!verify_line.empty()) {
            original_unique_records.insert(verify_line);
            verify_count++;
        }
    }
    fin_verify.close();
    
    std::set<std::string> reconstructed_unique_records;
    for (const auto& block_json : decompressed_jsons) {
        std::istringstream iss(block_json);
        std::string l;
        while (std::getline(iss, l)) {
            if (!l.empty()) {
                reconstructed_unique_records.insert(l);
            }
        }
    }
    std::cout << "[STATS] 原始记录总数: " << total_records << std::endl;
    std::cout << "[STATS] 原始唯一记录数: " << original_unique_records.size() << std::endl;
    std::cout << "[STATS] 重建记录总数: " << total_lines << std::endl;
    std::cout << "[STATS] 重建唯一记录数: " << reconstructed_unique_records.size() << std::endl;
    if (original_unique_records.size() == reconstructed_unique_records.size()) {
        std::cout << "✅ All unique records recovered!" << std::endl;
    } else {
        std::cout << "❌ Unique record count mismatch: original=" << original_unique_records.size() 
                  << ", reconstructed=" << reconstructed_unique_records.size() << std::endl;
    }
    return 0;
}
