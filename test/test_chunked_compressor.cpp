#include "../include/chunked_compressor.h"
#include "../include/reconstruct.h"
#include <simdjson.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <cassert>
#include <sys/stat.h>

using namespace json2;

int main() {
    const std::string input_file = "test_data.json";
    const size_t BLOCK_SIZE = 20000;
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
    ChunkedTrieCompressor compressor(config);

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
            std::cout << "[DEBUG] Block " << i << ": fields=" << block.field_order.size();
            if (block.dict) std::cout << ", dict valid";
            else std::cout << ", dict nullptr!";
            if (block.trie) std::cout << ", trie valid";
            else std::cout << ", trie nullptr!";
            std::cout << std::endl;
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

        // 校验一致性（行数）
        if (total_lines == records.size()) {
            std::cout << "✅ All records recovered!" << std::endl;
        } else {
            std::cout << "❌ Record count mismatch: original=" << records.size() << ", reconstructed=" << total_lines << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception during deserialization: " << e.what() << std::endl;
        return 2;
    }
    return 0;
}
