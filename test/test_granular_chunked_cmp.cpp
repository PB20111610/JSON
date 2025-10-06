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

using namespace json2;

int main() {
    const std::string input_file = "test_data.json"; // "../data/postgresql.log"; // 
    const size_t BLOCK_SIZE = 20000;
    const size_t CHUNK_SIZE = 1000;
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
    std::cout << "  Granular compression: " << (compressor.isGranularCompressionEnabled() ? "enabled" : "disabled") << std::endl;
    std::cout << "  Layer separation: " << (compressor.isLayerSeparationEnabled() ? "enabled" : "disabled") << std::endl;

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

    // 压缩并保存到目录
    std::string out_dir = "compressed_chunked_data";
    // 清理旧的压缩数据目录
    std::string cleanup_cmd = "rm -rf " + out_dir;
    int cleanup_result = system(cleanup_cmd.c_str());
    if (cleanup_result != 0) {
        std::cout << "[INFO] Cleanup command returned: " << cleanup_result << " (this is usually fine)" << std::endl;
    }    
    mkdir(out_dir.c_str(), 0777);
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
    std::set<std::string> original_unique_records(records.begin(), records.end());
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
    return 0;
}
