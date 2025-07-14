#include "../include/parser.h"
#include "../include/field_dictionary_manager.h"
#include "../include/trie.h"
#include "../include/reconstruct.h"
#include "../include/compress.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <nlohmann/json.hpp> // 只用于重建验证
#include <simdjson.h>
#include <sys/stat.h>

using namespace json2;
using nlohmann::json;

// Helper function to print field redundancy
void printFieldOrder(const std::vector<std::string>& ordered_fields) {
    std::cout << "\nField Order (by redundancy factor):\n";
    std::cout << "================================\n";
    for (size_t i = 0; i < ordered_fields.size(); ++i) {
        std::cout << (i + 1) << ". " << ordered_fields[i] << "\n";
    }
}

// 递归打印Trie树结构
void printTrieNode(const TrieNode* node, int depth, const std::vector<std::string>& fields, const Dictionary& dict) {
    if (!node) return;
    std::cout << std::string(depth * 2, ' ') << "Depth " << depth << ": ";
    if (node->isPlaceholder()) {
        std::cout << "[PLACEHOLDER]\n";
    } else {
        if (depth < fields.size()) {
            std::cout << "[Field: " << fields[depth] << "] ";
            const NodeValue& value = node->getValue();
            if (std::holds_alternative<uint32_t>(value)) {
                uint32_t code = std::get<uint32_t>(value);
                std::cout << "Code " << code << " -> ";
                auto opt_val = dict.getFieldValueByCode(fields[depth], FieldType::String, code);
                if (opt_val && std::holds_alternative<std::string>(*opt_val)) {
                    std::cout << std::get<std::string>(*opt_val);
                } else {
                    std::cout << "Unknown";
                }
            } else if (std::holds_alternative<int64_t>(value)) {
                std::cout << "Int: " << std::get<int64_t>(value);
            } else if (std::holds_alternative<double>(value)) {
                std::cout << "Double: " << std::get<double>(value);
            } else if (std::holds_alternative<bool>(value)) {
                std::cout << "Bool: " << (std::get<bool>(value) ? "true" : "false");
            }
            std::cout << "\n";
        } else {
            std::cout << "Value: ";
            const NodeValue& value = node->getValue();
            if (std::holds_alternative<uint32_t>(value)) {
                std::cout << "Code " << std::get<uint32_t>(value);
            } else if (std::holds_alternative<int64_t>(value)) {
                std::cout << "Int " << std::get<int64_t>(value);
            } else if (std::holds_alternative<double>(value)) {
                std::cout << "Double " << std::get<double>(value);
            } else if (std::holds_alternative<bool>(value)) {
                std::cout << "Bool " << (std::get<bool>(value) ? "true" : "false");
            }
            std::cout << " -> Unknown\n";
        }
    }
    for (const auto& [val, child] : node->getChildren()) {
        printTrieNode(child.get(), depth + 1, fields, dict);
    }
}

int main() {
    const size_t CHUNK_SIZE = 10000; // Process 100,000 records per chunk
    const char* DATA_PATH = "test_data.json"; 

    try {
        FieldDictionaryManager manager;
        std::vector<std::string> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;

        std::ifstream in(DATA_PATH);
        if (!in.is_open()) {
            throw std::runtime_error(std::string("Cannot open ") + DATA_PATH + " for reading");
        }

        simdjson::dom::parser parser; // Create the parser once to be reused
        bool isFirstChunk = true;
        while (in) {
            std::vector<std::string> records;
            records.reserve(CHUNK_SIZE);
            std::string line;
            for (size_t i = 0; i < CHUNK_SIZE && std::getline(in, line); ++i) {
                if (line.empty()) continue;
                try {
                    // Use simdjson to parse, then convert to nlohmann::json
                    simdjson::dom::element doc = parser.parse(line).value();
                    records.push_back(line);
                } catch (const std::exception& e) {
                    std::cerr << "JSON parse error in line, skipping: " << e.what() << "\n";
                }
            }

            if (records.empty()) {
                break;
            }

            if (isFirstChunk) {
                // First chunk: Analyze, determine field order, and create the Trie
                std::cout << "--- Processing First Chunk ---\n";
                JsonParser::analyzeAndSortFields(records, manager, fieldOrder);
                printFieldOrder(fieldOrder);
                trie = std::make_unique<Trie>(fieldOrder);
                isFirstChunk = false;
            } else {
                std::cout << "--- Processing Subsequent Chunk (" << records.size() << " records) ---\n";
            }

            // Insert records from the current chunk into the Trie
            for (const auto& record : records) {
                trie->insert(record, manager.variableDict(), parser);
            }
        }
        in.close();

        if (!trie) {
            std::cout << "No records processed, exiting." << std::endl;
            return 0;
        }

        // 统计原始数据文件大小
        struct stat st;
        size_t original_size = 0;
        if (stat(DATA_PATH, &st) == 0) {
            original_size = st.st_size;
        }

        // 测试新的压缩功能
        std::cout << "\n=== Testing New Compression ===\n";
        
        // 压缩数据
        CompressedData compressed = Compressor::compress(*trie, manager);
        std::cout << "Original size: " << compressed.original_size << " bytes\n";
        std::cout << "Compressed size: " << compressed.compressed_size << " bytes\n";
        std::cout << "Compression ratio: " << std::fixed << std::setprecision(2) 
                  << Compressor::getCompressionRatio(compressed) * 100 << "%\n";
        
        // 保存压缩数据
        const std::string compressed_filename = "compressed.json2";
        if (Compressor::saveToFile(compressed, compressed_filename)) {
            std::cout << "✅ Compressed data saved to " << compressed_filename << "\n";
        } else {
            std::cout << "❌ Failed to save compressed data\n";
        }
        
        // 统计压缩后文件大小
        size_t compressed_size = 0;
        if (stat(compressed_filename.c_str(), &st) == 0) {
            compressed_size = st.st_size;
        }

        // 输出统计信息
        std::cout << "\n=== Compression Statistics ===\n";
        std::cout << "Original data size: " << original_size << " bytes\n";
        std::cout << "Core data (to be compressed) size: " << compressed.original_size << " bytes\n";
        std::cout << "Compressed file size: " << compressed_size << " bytes\n";
        if (compressed_size > 0) {
            std::cout << "Original/Compressed Ratio: " << std::fixed << std::setprecision(2) << (double)original_size / compressed_size << "x\n";
            std::cout << "Core/Compressed Ratio: " << std::fixed << std::setprecision(2) << (double)compressed.original_size / compressed_size << "x\n";
        }

        // 测试解压缩功能
        std::cout << "\n=== Testing Decompression ===\n";
        
        // 从文件加载压缩数据
        CompressedData loaded_compressed = Compressor::loadFromFile(compressed_filename);
        
        // 解压缩
        auto [decompressed_trie, decompressed_manager] = Compressor::decompress(loaded_compressed);
        
        std::cout << "✅ Decompression successful!\n";
        
        // 验证解压缩结果
        std::cout << "\n=== Verifying Decompression ===\n";
        
        // 重建JSON并比较
        std::string original_json = reconstructJsonFromTrie(*trie, manager);
        std::string decompressed_json = reconstructJsonFromTrie(*decompressed_trie, *decompressed_manager);
        
        if (original_json == decompressed_json) {
            std::cout << "✅ Decompression verification successful! Original and decompressed JSON are identical.\n";
        } else {
            std::cout << "❌ Decompression verification failed! Original and decompressed JSON differ.\n";
            
            // 保存两个文件进行比较
            std::ofstream original_file("original_reconstructed.json");
            original_file << original_json;
            original_file.close();
            
            std::ofstream decompressed_file("decompressed_reconstructed.json");
            decompressed_file << decompressed_json;
            decompressed_file.close();
            
            std::cout << "Original JSON saved to: original_reconstructed.json\n";
            std::cout << "Decompressed JSON saved to: decompressed_reconstructed.json\n";
        }

        // 保存解压缩后的JSON
        const std::string reconstructed_filename = "reconstructed_from_compressed.json";
        std::ofstream out_file_compressed(reconstructed_filename);
        if (!out_file_compressed.is_open()) {
            throw std::runtime_error("Cannot open " + reconstructed_filename + " for writing");
        }
        out_file_compressed << decompressed_json;
        out_file_compressed.close();

        std::cout << "✅ JSON reconstructed from compressed data!\n";
        std::cout << "Reconstructed JSON: " << reconstructed_filename << "\n";
        std::cout << "Please compare with the original to verify.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}