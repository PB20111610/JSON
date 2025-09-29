#include "../include/chunked_compressor.h"
#include "../include/compress.h"
#include "../include/reconstruct.h"
#include <simdjson.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <cassert>
#include <sys/stat.h>
#include <set>

using namespace json2;

int main() {
    const std::string input_file = "test_data.json"; // "../data/hdfs-full.json"; //
    const size_t BLOCK_SIZE = 40000; // 每个块处理 50,000 条记录
    std::ifstream fin(input_file);
    if (!fin.is_open()) {
        std::cerr << "Cannot open input file: " << input_file << std::endl;
        return 1;
    }
    std::vector<std::string> records;
    std::string line;
    while (std::getline(fin, line)) {
        if (!line.empty()) records.push_back(line);
    }
    fin.close();
    std::cout << "Loaded " << records.size() << " records from " << input_file << std::endl;

    // 分块压缩
    CompressorConfig config;
    config.block_size = BLOCK_SIZE;
    config.structurize_arrays = false;  // 设置为 false 启用非结构化数组处理
    ChunkedTrieCompressor compressor(config);

    // 设置时间戳字段
    std::vector<std::string> timestamp_fields = {"@timestamp","request_received","response_delivered", "timestamp", "session_start"};  //"@timestamp",
    compressor.setTimestampFields(timestamp_fields);
    
    // 设置数组处理模式（也可以通过setter方法设置）
    compressor.setStructurizeArrays(false);  // 设置为 false 启用非结构化数组处理
    
    // 设置原始文件大小
    struct stat st;
    size_t original_file_size = 0;
    if (stat(input_file.c_str(), &st) == 0) {
        original_file_size = st.st_size;
    }
    compressor.setOriginalFileSize(original_file_size);

    simdjson::dom::parser parser;
    size_t record_idx = 0;
    for (const auto& rec : records) {
        compressor.addRecord(rec, parser);
        if ((++record_idx) % BLOCK_SIZE == 0) {
            std::cout << "[DEBUG] Added record " << record_idx << ", current block should flush soon." << std::endl;
        }
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

    // 分块解压
    std::cout << "[DEBUG] Start deserializing blocks..." << std::endl;
    size_t offset = 0;
    try {
        auto blocks = ChunkedTrieCompressor::deserialize(compressed);
        std::cout << "Block count: " << blocks.size() << std::endl;

        // 复原所有JSON
        std::vector<std::string> decompressed_jsons;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto& block = blocks[i];
            // std::cout << "[DEBUG] Block " << i << ": fields=" << block.field_order.size();
            // if (block.dict) std::cout << ", dict valid";
            // else std::cout << ", dict nullptr!";
            // if (block.trie) std::cout << ", trie valid";
            // else std::cout << ", trie nullptr!";
            // std::cout << std::endl;
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
        std::set<std::string> original_unique_records(records.begin(), records.end());
        std::set<std::string> reconstructed_unique_records;
        
        // 从重建的JSON中提取记录
        for (const auto& block_json : decompressed_jsons) {
            std::istringstream iss(block_json);
            std::string l;
            while (std::getline(iss, l)) {
                if (!l.empty()) {
                    reconstructed_unique_records.insert(l);
                }
            }
        }
        
        std::cout << "[STATS] 原始记录总数: " << records.size() << std::endl;
        std::cout << "[STATS] 原始唯一记录数: " << original_unique_records.size() << std::endl;
        std::cout << "[STATS] 重建记录总数: " << total_lines << std::endl;
        std::cout << "[STATS] 重建唯一记录数: " << reconstructed_unique_records.size() << std::endl;
        
        if (original_unique_records.size() == reconstructed_unique_records.size()) {
            std::cout << "✅ All unique records recovered!" << std::endl;
        } else {
            std::cout << "❌ Unique record count mismatch: original=" << original_unique_records.size() 
                      << ", reconstructed=" << reconstructed_unique_records.size() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception during deserialization: " << e.what() << std::endl;
        return 2;
    }
    return 0;
}
