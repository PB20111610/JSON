#include "../include/parser.h"
#include "../include/field_dictionary_manager.h"
#include "../include/trie.h"
#include "../include/reconstruct.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <variant>
#include <nlohmann/json.hpp> // 只用于重建验证
#include <simdjson.h>

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

// 打印字典内容
void printDictionary(const FieldDictionaryManager& manager, const std::vector<std::string>& ordered_fields) {
    std::cout << "\n=== Dictionary Contents ===\n";
    for (const auto& field : ordered_fields) {
        std::cout << "\nField: " << field << "\n";
        // 打印字符串类型的字典内容
        const Dictionary& dict = manager.variableDict();
        size_t count = dict.getFieldValueCount(field, FieldType::String);
        for (uint32_t code = 1; code <= count; ++code) {
            auto opt_value = dict.getFieldValueByCode(field, FieldType::String, code);
            if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                std::cout << "  Code " << code << ": " << std::get<std::string>(*opt_value) << "\n";
            }
        }
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
    const size_t CHUNK_SIZE = 10000; // Process 10,000 records per chunk

    try {
        FieldDictionaryManager manager;
        std::vector<std::string> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;

        std::ifstream in("test_data.json");
        if (!in.is_open()) {
            throw std::runtime_error("Cannot open test_data.json for reading");
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
                // printDictionary(manager, fieldOrder);
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

        // Verification and reconstruction steps remain the same
        std::cout << "\n=== Trie Tree Structure ===\n";
        printTrieNode(trie->getRoot(), 0, fieldOrder, manager.variableDict());
        
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