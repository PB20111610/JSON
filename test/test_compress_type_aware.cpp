#include "../include/field_analyzer.h"
#include "../include/field_dictionary_manager.h"
#include "../include/trie.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include "../include/reconstruct.h"
#include "../include/compress.h"
#include "../include/compress_type_aware.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <nlohmann/json.hpp>
#include "../vendor/simdjson/simdjson.h"
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
        case FieldType::UnstructuredArray: return "UnstructuredArray";
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
            } else if (std::holds_alternative<TemplateEncodedTimestamp>(value)) {
                std::cout << "TemplateEncodedTimestamp: [template_id=" << std::get<TemplateEncodedTimestamp>(value).template_id << ", var_codes=[";
                const auto& var_codes = std::get<TemplateEncodedTimestamp>(value).var_codes;
                for (size_t i = 0; i < var_codes.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << var_codes[i];
                }
                std::cout << "]]";
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

// 测试压缩器并返回统计信息
struct CompressionStats {
    size_t original_size;
    size_t compressed_size;
    double compression_ratio;
    std::string compressor_name;
    std::string filename;
};

CompressionStats testCompressor(const std::string& compressor_name, 
                               const Trie& trie, 
                               const FieldDictionaryManager& manager,
                               const std::string& base_filename) {
    std::cout << "\n=== Testing " << compressor_name << " ===\n";
    
    CompressedData compressed;
    std::string filename = base_filename + "_" + compressor_name + ".json2";
    
    // 压缩数据
    std::cout << "[DEBUG] Before " << compressor_name << "::compress" << std::endl;
    if (compressor_name == "TypeAwareCompressor") {
        compressed = TypeAwareCompressor::compress(trie, manager);
    } else {
        compressed = Compressor::compress(trie, manager);
    }
    std::cout << "[DEBUG] After " << compressor_name << "::compress" << std::endl;
    
    std::cout << "Original size: " << compressed.original_size << " bytes\n";
    std::cout << "Compressed size: " << compressed.compressed_size << " bytes\n";
    
    double ratio;
    if (compressor_name == "TypeAwareCompressor") {
        ratio = TypeAwareCompressor::getCompressionRatio(compressed);
    } else {
        ratio = Compressor::getCompressionRatio(compressed);
    }
    std::cout << "Compression ratio: " << std::fixed << std::setprecision(2) << ratio * 100 << "%\n";
    
    // 保存压缩数据
    std::cout << "[DEBUG] Before " << compressor_name << "::saveToFile" << std::endl;
    bool save_success;
    if (compressor_name == "TypeAwareCompressor") {
        save_success = TypeAwareCompressor::saveToFile(compressed, filename);
    } else {
        save_success = Compressor::saveToFile(compressed, filename);
    }
    
    if (save_success) {
        std::cout << "✅ Compressed data saved to " << filename << "\n";
    } else {
        std::cout << "❌ Failed to save compressed data\n";
    }
    std::cout << "[DEBUG] After " << compressor_name << "::saveToFile" << std::endl;
    
    // 统计压缩后文件大小
    struct stat st;
    size_t file_size = 0;
    if (stat(filename.c_str(), &st) == 0) {
        file_size = st.st_size;
    }
    
    // 测试解压缩功能
    std::cout << "\n=== Testing " << compressor_name << " Decompression ===\n";
    
    // 从文件加载压缩数据
    std::cout << "[DEBUG] Before " << compressor_name << "::loadFromFile" << std::endl;
    CompressedData loaded_compressed;
    if (compressor_name == "TypeAwareCompressor") {
        loaded_compressed = TypeAwareCompressor::loadFromFile(filename);
    } else {
        loaded_compressed = Compressor::loadFromFile(filename);
    }
    std::cout << "[DEBUG] After " << compressor_name << "::loadFromFile" << std::endl;
    
    // 解压缩
    std::cout << "[DEBUG] Before " << compressor_name << "::decompress" << std::endl;
    try {
        std::unique_ptr<Trie> decompressed_trie;
        std::unique_ptr<FieldDictionaryManager> decompressed_manager;
        
        if (compressor_name == "TypeAwareCompressor") {
            auto result = TypeAwareCompressor::decompress(loaded_compressed);
            decompressed_trie = std::move(result.first);
            decompressed_manager = std::move(result.second);
        } else {
            auto result = Compressor::decompress(loaded_compressed);
            decompressed_trie = std::move(result.first);
            decompressed_manager = std::move(result.second);
        }
        
        std::cout << "✅ Decompression successful!\n";
        
        // 重建JSON并保存
        std::string decompressed_json = reconstructJsonFromTrie(*decompressed_trie, *decompressed_manager);
        std::string reconstructed_filename = "reconstructed_" + compressor_name + ".json";
        std::ofstream out_file(reconstructed_filename);
        if (!out_file.is_open()) {
            throw std::runtime_error("Cannot open " + reconstructed_filename + " for writing");
        }
        out_file << decompressed_json;
        out_file.close();
        std::cout << "✅ JSON reconstructed from " << compressor_name << " compressed data!\n";
        std::cout << "Reconstructed JSON: " << reconstructed_filename << "\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Decompression error: " << e.what() << std::endl;
        throw;
    }
    
    return {compressed.original_size, file_size, ratio, compressor_name, filename};
}

int main() {
    const size_t CHUNK_SIZE = 1000; // Process 1000 records for field order analysis
    const char* DATA_PATH = "test_data.json";

    try {
        FieldDictionaryManager manager;
        // 配置时间戳字段
        std::vector<std::string> timestamp_fields = {"@timestamp", "timestamp", "session_start"};
        manager.setTimestampFields(timestamp_fields);
        std::vector<FieldKey> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;

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
        FieldAnalyzer::analyzeAndSortFields(first_chunk, manager, fieldOrder);
        printFieldOrder(fieldOrder);
        trie = std::make_unique<Trie>(fieldOrder);

        // 3. 插入前 CHUNK_SIZE 行
        for (const auto& record : first_chunk) {
            trie->insert(record, manager, parser);
        }

        // 4. 继续插入剩余行
        size_t line_count = first_chunk.size();
        const size_t PRINT_INTERVAL = 10000;

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

        // 统计原始数据文件大小
        struct stat st;
        size_t original_file_size = 0;
        if (stat(DATA_PATH, &st) == 0) {
            original_file_size = st.st_size;
        }

        std::cout << "\n=== Compression Comparison Test ===\n";
        std::cout << "Original data file size: " << original_file_size << " bytes\n";
        std::cout << "Total records processed: " << line_count << "\n";

        // 测试原始压缩器
        CompressionStats original_stats = testCompressor("Compressor", *trie, manager, "compressed");
        
        // 测试类型感知压缩器
        CompressionStats type_aware_stats = testCompressor("TypeAwareCompressor", *trie, manager, "compressed");

        // 输出对比结果
        std::cout << "\n=== Compression Comparison Results ===\n";
        std::cout << "========================================\n";
        std::cout << std::left << std::setw(20) << "Compressor" 
                  << std::setw(15) << "Original Size" 
                  << std::setw(15) << "Compressed Size" 
                  << std::setw(15) << "Compression Ratio" 
                  << std::setw(15) << "File Size" << "\n";
        std::cout << std::string(80, '-') << "\n";
        
        std::cout << std::left << std::setw(20) << original_stats.compressor_name
                  << std::setw(15) << original_stats.original_size
                  << std::setw(15) << original_stats.compressed_size
                  << std::setw(15) << std::fixed << std::setprecision(2) << (original_stats.compression_ratio * 100) << "%"
                  << std::setw(15) << original_stats.compressed_size << "\n";
        
        std::cout << std::left << std::setw(20) << type_aware_stats.compressor_name
                  << std::setw(15) << type_aware_stats.original_size
                  << std::setw(15) << type_aware_stats.compressed_size
                  << std::setw(15) << std::fixed << std::setprecision(2) << (type_aware_stats.compression_ratio * 100) << "%"
                  << std::setw(15) << type_aware_stats.compressed_size << "\n";
        
        // 计算改进
        double improvement = ((original_stats.compression_ratio - type_aware_stats.compression_ratio) / original_stats.compression_ratio) * 100;
        std::cout << "\n=== Improvement Analysis ===\n";
        std::cout << "TypeAwareCompressor vs Compressor:\n";
        std::cout << "Compression ratio improvement: " << std::fixed << std::setprecision(2) << improvement << "%\n";
        
        if (improvement > 0) {
            std::cout << "✅ TypeAwareCompressor achieves better compression!\n";
        } else if (improvement < 0) {
            std::cout << "⚠️  TypeAwareCompressor has worse compression ratio.\n";
        } else {
            std::cout << "➖ Both compressors achieve similar compression.\n";
        }
        
        // 输出文件信息
        std::cout << "\n=== Generated Files ===\n";
        std::cout << "Original Compressor: " << original_stats.filename << "\n";
        std::cout << "TypeAware Compressor: " << type_aware_stats.filename << "\n";
        std::cout << "Reconstructed (Original): reconstructed_Compressor.json\n";
        std::cout << "Reconstructed (TypeAware): reconstructed_TypeAwareCompressor.json\n";
        std::cout << "\nPlease compare the reconstructed files with the original test_data.json to verify data integrity.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
