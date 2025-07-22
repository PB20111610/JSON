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

// Helper function to将FieldType转为字符串
std::string fieldTypeToString(FieldType type) {
    switch (type) {
        case FieldType::Int: return "Int";
        case FieldType::Double: return "Double";
        case FieldType::Bool: return "Bool";
        case FieldType::String: return "String";
        case FieldType::Timestamp: return "Timestamp";
        case FieldType::LogType: return "LogType";
        case FieldType::Null: return "Null";
        default: return "Unknown";
    }
}

// Helper function to print field redundancy
void printFieldOrder(const std::vector<FieldKey>& ordered_fields) {
    std::cout << "\nField Order (by redundancy factor):\n";
    std::cout << "================================\n";
    for (size_t i = 0; i < ordered_fields.size(); ++i) {
        std::cout << (i + 1) << ". " << ordered_fields[i].name << " [" << fieldTypeToString(ordered_fields[i].type) << "]\n";
    }
}

// 递归打印Trie树结构（适配FieldKey）
void printTrieNode(const TrieNode* node, int depth, const std::vector<FieldKey>& fields, const FieldDictionaryManager& manager) {
    if (!node) return;
    const auto& path = node->getPath();
    std::cout << std::string(depth * 2, ' ') << "Depth " << depth << ": ";
    if (!path.empty()) {
        for (size_t i = 0; i < path.size(); ++i) {
            size_t field_idx = depth + i;
            if (field_idx >= fields.size()) continue;
            const FieldKey& fk = fields[field_idx];
            std::cout << "[Field: " << fk.name << "] ";
            const NodeValue& value = path[i];
            if (std::holds_alternative<uint32_t>(value)) {
                uint32_t code = std::get<uint32_t>(value);
                auto opt_val = manager.getFieldValueByCode(fk, code);
                if (opt_val && std::holds_alternative<std::string>(*opt_val)) {
                    std::cout << "Code " << code << " -> " << std::get<std::string>(*opt_val);
                } else {
                    std::cout << "Code " << code << " -> Unknown";
                }
            } else if (std::holds_alternative<int64_t>(value)) {
                std::cout << "Int: " << std::get<int64_t>(value);
            } else if (std::holds_alternative<double>(value)) {
                std::cout << "Double: " << std::get<double>(value);
            } else if (std::holds_alternative<bool>(value)) {
                std::cout << "Bool: " << (std::get<bool>(value) ? "true" : "false");
            } else if (std::holds_alternative<EncodedTimestamp>(value)) {
                std::cout << "Timestamp: [pattern_id=" << std::get<EncodedTimestamp>(value).pattern_id << ", epoch=" << std::get<EncodedTimestamp>(value).epoch << "]";
            } else if (std::holds_alternative<EncodedLog>(value)) {
                std::cout << "LogType: [template_id=" << std::get<EncodedLog>(value).template_id << ", var_codes=";
                for (auto v : std::get<EncodedLog>(value).var_codes) std::cout << v << ",";
                std::cout << "]";
            }
            std::cout << " | ";
        }
    }
    std::cout << "\n";
    for (const auto& child : node->getChildren()) {
        printTrieNode(child.second.get(), depth + path.size(), fields, manager);
    }
}

int main() {
    const size_t CHUNK_SIZE = 1000; // Process 1000 records for field order analysis
    const char* DATA_PATH =   "test_data.json"; //"../data/postgresql.log";  //

    try {
        FieldDictionaryManager manager;
        // 配置时间戳字段
        std::vector<std::string> timestamp_fields = {"timestamp", "session_start"};
        manager.setTimestampFields(timestamp_fields);
        std::vector<FieldKey> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;

        std::ifstream in(DATA_PATH);
        if (!in.is_open()) {
            throw std::runtime_error(std::string("Cannot open ") + DATA_PATH + " for reading");
        }

        simdjson::dom::parser parser; // Create the parser once to be reused
        std::vector<std::string> first_chunk;
        std::string line;
        // 1. 读取前 CHUNK_SIZE 行
        for (size_t i = 0; i < CHUNK_SIZE && std::getline(in, line); ) {
            if (line.empty()) continue;
            try {
                simdjson::dom::element doc = parser.parse(line).value();
                first_chunk.push_back(line);
                ++i;
            } catch (const std::exception& e) {
                std::cerr << "JSON parse error in line, skipping: " << e.what() << "\n";
            }
        }

        if (first_chunk.empty()) {
            std::cout << "No records processed, exiting." << std::endl;
            return 0;
        }

        // 2. 分析字段顺序
        std::cout << "--- Processing First Chunk ---\n";
        JsonParser::analyzeAndSortFields(first_chunk, manager, fieldOrder);
        printFieldOrder(fieldOrder);
        trie = std::make_unique<Trie>(fieldOrder);

        // 3. 插入前 CHUNK_SIZE 行
        for (const auto& record : first_chunk) {
            trie->insert(record, manager, parser);
        }

        // 4. 继续插入剩余行
        size_t line_count = first_chunk.size();
        const size_t PRINT_INTERVAL = 10000; // 每处理1万行输出一次

        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                simdjson::dom::element doc = parser.parse(line).value();
                trie->insert(line, manager, parser);
                ++line_count;
                if (line_count % PRINT_INTERVAL == 0) {
                    std::cout << "[INFO] 已处理 " << line_count << " 行..." << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "JSON parse error in line, skipping: " << e.what() << "\n";
            }
        }
        in.close();

        // 插入后批量路径压缩
        trie->compressPaths();
        
        // 插入所有数据后，打印Trie结构
        // std::cout << "[DEBUG] 压缩前 Trie 结构：" << std::endl;
        // printTrieNode(trie->getRoot(), 0, trie->getOrderedFields(), manager);


        // 统计原始数据文件大小
        struct stat st;
        size_t original_size = 0;
        if (stat(DATA_PATH, &st) == 0) {
            original_size = st.st_size;
        }

        // 测试新的压缩功能
        std::cout << "\n=== Testing New Compression ===\n";
        
        // 压缩数据
        std::cout << "[DEBUG] Before Compressor::compress" << std::endl;
        CompressedData compressed = Compressor::compress(*trie, manager);
        std::cout << "[DEBUG] After Compressor::compress" << std::endl;
        std::cout << "Original size: " << compressed.original_size << " bytes\n";
        std::cout << "Compressed size: " << compressed.compressed_size << " bytes\n";
        std::cout << "Compression ratio: " << std::fixed << std::setprecision(2) 
                  << Compressor::getCompressionRatio(compressed) * 100 << "%\n";
        
        // 保存压缩数据
        const std::string compressed_filename = "compressed.json2";
        std::cout << "[DEBUG] Before Compressor::saveToFile" << std::endl;
        if (Compressor::saveToFile(compressed, compressed_filename)) {
            std::cout << "✅ Compressed data saved to " << compressed_filename << "\n";
        } else {
            std::cout << "❌ Failed to save compressed data\n";
        }
        std::cout << "[DEBUG] After Compressor::saveToFile" << std::endl;
        
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
        std::cout << "[DEBUG] Before Compressor::loadFromFile" << std::endl;
        CompressedData loaded_compressed = Compressor::loadFromFile(compressed_filename);
        std::cout << "[DEBUG] After Compressor::loadFromFile" << std::endl;
        
        // 解压缩
        std::cout << "[DEBUG] Before Compressor::decompress" << std::endl;
        try {
            auto [decompressed_trie, decompressed_manager] = Compressor::decompress(loaded_compressed);
            
            std::cout << "✅ Decompression successful!\n";
                       
            // 展开路径压缩
            decompressed_trie->expandPaths();
            // std::cout << "[DEBUG] expandPaths()后 Trie 结构：" << std::endl;
            // printTrieNode(decompressed_trie->getRoot(), 0, decompressed_trie->getOrderedFields(), *decompressed_manager);

            // 只重建并保存解压缩后的JSON
            std::string decompressed_json = reconstructJsonFromTrie(*decompressed_trie, *decompressed_manager);
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
            std::cerr << "Decompression error: " << e.what() << std::endl;
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}