#include "../include/parser.h"
#include "../include/field_dictionary_manager.h"
// #include "../include/trie_type_aware.h"
#include "../include/reconstruct.h"
#include "../include/trie.h" // Added for Trie
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <variant>
#include <nlohmann/json.hpp> // 只用于重建验证
#include <simdjson.h>
#include <queue>
#include <type_traits>

using namespace json2;
using nlohmann::json;

// Helper function to convert FieldType to string
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

// Helper function to print field redundancy (now with FieldKey)
void printFieldOrder(const std::vector<FieldKey>& ordered_fields) {
    std::cout << "\nField Order (by redundancy factor):\n";
    std::cout << "================================\n";
    for (size_t i = 0; i < ordered_fields.size(); ++i) {
        const auto& key = ordered_fields[i];
        std::cout << (i + 1) << ". " << key.name << "[" << fieldTypeToString(key.type) << "]\n";
    }
}

// 打印字典内容（适配FieldKey）
void printDictionary(const FieldDictionaryManager& manager, const std::vector<FieldKey>& ordered_fields) {
    std::cout << "\n=== Dictionary Contents ===\n";
    const Dictionary& dict = manager.variableDict();
    for (const auto& key : ordered_fields) {
        std::cout << "\nField: " << key.name << "[" << fieldTypeToString(key.type) << "]\n";
        size_t count = dict.getFieldValueCount(key);
        for (uint32_t code = 1; code <= count; ++code) {
            auto opt_value = dict.getFieldValueByCode(key, code);
            if (opt_value) {
                std::cout << "  Code " << code << ": ";
                if (std::holds_alternative<std::string>(*opt_value)) {
                    std::cout << std::get<std::string>(*opt_value);
                } else if (std::holds_alternative<int64_t>(*opt_value)) {
                    std::cout << std::get<int64_t>(*opt_value);
                } else if (std::holds_alternative<double>(*opt_value)) {
                    std::cout << std::get<double>(*opt_value);
                } else if (std::holds_alternative<bool>(*opt_value)) {
                    std::cout << (std::get<bool>(*opt_value) ? "true" : "false");
                } else if (std::holds_alternative<std::nullptr_t>(*opt_value)) {
                    std::cout << "null";
                }
                std::cout << "\n";
            }
        }
    }
}

// 递归打印Trie树结构（适配FieldKey）
// 修改printTrieNode签名，传入FieldDictionaryManager& manager
void printTrieNode(const TrieNode* node, int depth, const std::vector<FieldKey>& fields, const FieldDictionaryManager& manager) {
    if (!node) {
        return;
    }
    const auto& path = node->getPath();
    std::cout << std::string(depth * 2, ' ') << "[Depth " << depth << "] ";
    if (node->isPlaceholder()) {
        std::cout << "[PLACEHOLDER] ";
    }
    if (!path.empty()) {
        std::cout << "Path: ";
        for (size_t i = 0; i < path.size(); ++i) {
            size_t field_idx = depth + i;
            if (field_idx >= fields.size()) {
                continue;
            }
            std::string field_desc = fields[field_idx].name + "[" + fieldTypeToString(fields[field_idx].type) + "]";
            const FieldKey* key_ptr = &fields[field_idx];
            std::cout << "{" << field_desc << ": ";
            const NodeValue& value = path[i];
            if (std::holds_alternative<uint32_t>(value) && key_ptr) {
                uint32_t code = std::get<uint32_t>(value);
                auto opt_val = manager.getFieldValueByCode(*key_ptr, code);
                if (opt_val) {
                    if (std::holds_alternative<std::string>(*opt_val)) {
                        std::cout << "Code " << code << " -> '" << std::get<std::string>(*opt_val) << "'";
                    } else if (std::holds_alternative<int64_t>(*opt_val)) {
                        std::cout << "Code " << code << " -> " << std::get<int64_t>(*opt_val);
                    } else if (std::holds_alternative<double>(*opt_val)) {
                        std::cout << "Code " << code << " -> " << std::get<double>(*opt_val);
                    } else if (std::holds_alternative<bool>(*opt_val)) {
                        std::cout << "Code " << code << " -> " << (std::get<bool>(*opt_val) ? "true" : "false");
                    } else if (std::holds_alternative<std::nullptr_t>(*opt_val)) {
                        std::cout << "Code " << code << " -> null";
                    }
                } else {
                    std::cout << "Code " << code << " -> DECODE_ERROR";
                }
            } else if (std::holds_alternative<int64_t>(value)) {
                std::cout << "Int: " << std::get<int64_t>(value);
            } else if (std::holds_alternative<double>(value)) {
                std::cout << "Double: " << std::get<double>(value);
            } else if (std::holds_alternative<bool>(value)) {
                std::cout << "Bool: " << (std::get<bool>(value) ? "true" : "false");
            }
            std::cout << "} ";
        }
    }
    std::cout << "\n";
    for (const auto& child : node->getChildren()) {
        printTrieNode(child.second.get(), depth + node->getPath().size(), fields, manager);
    }
}

// === printNodeValue 声明和实现 ===
void printNodeValue(const NodeValue& v) {
    std::visit([](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>) {
            std::cout << val;
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
            std::cout << "null";
        } else if constexpr (std::is_same_v<T, TemplateEncodedTimestamp>) {
            std::cout << "TemplateEncodedTimestamp{template_id=" << val.template_id << ", var_codes=[";
            for (size_t i = 0; i < val.var_codes.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << val.var_codes[i];
            }
            std::cout << "]}";
        } else if constexpr (std::is_same_v<T, EncodedLog>) {
            std::cout << "EncodedLog{template_id=" << val.template_id << ", var_codes=[";
            for (size_t i = 0; i < val.var_codes.size(); ++i) {
                std::cout << val.var_codes[i];
                if (i + 1 < val.var_codes.size()) std::cout << ", ";
            }
            std::cout << "]}";
        } else {
            std::cout << "[UnknownType]";
        }
    }, v);
}

int main() {
    const size_t CHUNK_SIZE = 10000; // Process 10,000 records per chunk

    try {
        FieldDictionaryManager manager;
        // 配置时间戳字段
        // std::vector<std::string> timestamp_fields = {"@timestamp", "timestamp", "session_start"};
        // manager.setTimestampFields(timestamp_fields);
        
        std::vector<FieldKey> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;

        std::ifstream in("test_data.json");
        if (!in.is_open()) {
            throw std::runtime_error("Cannot open test_data.json for reading");
        }

        simdjson::dom::parser parser; // Create the parser once to be reused
        bool isFirstChunk = true;
        size_t chunk_idx = 0;
        while (in) {
            std::vector<std::string> records;
            records.reserve(CHUNK_SIZE);
            std::string line;
            size_t line_count = 0;
            for (size_t i = 0; i < CHUNK_SIZE && std::getline(in, line); ++i) {
                if (line.empty()) continue;
                try {
                    simdjson::dom::element doc = parser.parse(line).value();
                    records.push_back(line);
                    ++line_count;
                } catch (const std::exception& e) {
                    std::cerr << "[DEBUG] JSON parse error in line, skipping: " << e.what() << "\n";
                }
            }

            if (records.empty()) {
                break;
            }

            if (isFirstChunk) {
                std::cout << "--- Processing First Chunk ---\n";
                JsonParser::analyzeAndSortFields(records, manager, fieldOrder);
                printFieldOrder(fieldOrder);
                trie = std::make_unique<Trie>(fieldOrder);
                isFirstChunk = false;
            } else {
                // std::cout << "--- Processing Subsequent Chunk (" << records.size() << " records) ---\n";
            }

            // 开始插入Trie...
            // Insert records from the current chunk into the Trie
            size_t insert_count = 0;
            for (const auto& record : records) {
                trie->insert(record, manager, parser);
                ++insert_count;
                // if (insert_count % 1000 == 0) std::cout << "[DEBUG] 已插入 " << insert_count << " 条" << std::endl;
            }
            // std::cout << "[DEBUG] 本chunk插入Trie完成, 共 " << insert_count << " 条" << std::endl;
            ++chunk_idx;
        }
        in.close();

        if (!trie) {
            std::cout << "No records processed, exiting." << std::endl;
            return 0;
        }

        // 插入后批量路径压缩
        trie->compressPaths();

        // 展开Trie树，保证重建时字段不缺失
        trie->expandPaths();
        std::cout << "\n=== Trie Tree Structure (Before Compression) ===\n";
        printTrieNode(trie->getRoot(), 0, fieldOrder, manager);

        // === 调试：打印LogTypeDictionary内容 ===
        const auto& log_dict = manager.logtypeDict();
        std::cout << "\n=== LogTypeDictionary Templates ===\n";
        for (uint32_t i = 1; i <= log_dict.getLogTypeCount(); ++i) {
            std::cout << "Template " << i << ": " << log_dict.getLogTypeById(i) << std::endl;
        }
        // 打印变量字典
        std::cout << "\n=== LogTypeDictionary Variables ===\n";
        for (uint32_t i = 1; i < 1000; ++i) { // 假定变量数不会超过1000
            std::string var = log_dict.decodeVariable(i);
            if (!var.empty()) {
                std::cout << "VarCode " << i << ": " << var << std::endl;
            }
        }

        // === 调试：打印TimestampDictionary内容 ===
        const auto& ts_dict = manager.timestampDict();
        
        // 打印模板化编码信息
        std::cout << "\n=== TimestampDictionary Templates ===\n";
        for (uint32_t i = 1; i <= ts_dict.getTemplateCount(); ++i) {
            std::string template_str = ts_dict.getTemplateById(i);
            if (!template_str.empty()) {
                std::cout << "Template " << i << ": " << template_str << std::endl;
        }
        }
        
        // 打印变量信息
        std::cout << "\n=== TimestampDictionary Variables ===\n";
        for (uint32_t i = 1; i <= ts_dict.getVariableCount(); ++i) {
            std::string var = ts_dict.getVariableByCode(i);
            if (!var.empty()) {
                std::cout << "VarCode " << i << ": " << var << std::endl;
            }
        }

        // Verification and reconstruction steps remain the same
        std::string reconstructed_json = reconstructJsonFromTrie(*trie, manager);
        std::ofstream out_file("reconstructed.json");
        if (!out_file.is_open()) {
            throw std::runtime_error("Cannot open reconstructed.json for writing");
        }
        out_file << reconstructed_json;
        out_file.close();

        std::cout << "\n✅ JSON reconstruction completed!\n";
        std::cout << "Original JSON: test_data.json\n";
        std::cout << "Reconstructed JSON: reconstructed.json\n";
        std::cout << "Please compare these files manually to verify the reconstruction.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}