#include "../include/parser.h"
#include "../include/dictionary.h"
#include "../include/trie.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <nlohmann/json.hpp>

using namespace json2;
using nlohmann::json;

// 打印字段冗余度排序
void printFieldOrder(const std::vector<std::string>& ordered_fields) {
    std::cout << "\nField Order (by redundancy factor):\n";
    std::cout << "================================\n";
    for (size_t i = 0; i < ordered_fields.size(); ++i) {
        std::cout << (i + 1) << ". " << ordered_fields[i] << "\n";
    }
}

// 打印字典内容
void printDictionary(const Dictionary& dict, const std::vector<std::string>& ordered_fields) {
    std::cout << "\n=== Dictionary Contents ===\n";
    for (const auto& field : ordered_fields) {
        std::cout << "\nField: " << field << "\n";
        for (uint32_t code = 1;; ++code) {
            std::string value = dict.getFieldValueByCode(field, code);
            if (value.empty()) break;
            std::cout << "  Code " << code << ": " << value << "\n";
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
            std::cout << "Code " << node->getCode() << " -> ";
            std::cout << dict.getFieldValueByCode(fields[depth], node->getCode()) << "\n";
        } else {
            std::cout << "Code " << node->getCode() << " -> Unknown\n";
        }
    }
    for (const auto& [code, child] : node->getChildren()) {
        printTrieNode(child.get(), depth + 1, fields, dict);
    }
}

int main() {
    const size_t CHUNK_SIZE = 5000; // Process 10,000 records per chunk

    try {
        Dictionary dict;
        std::vector<std::string> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;

        std::ifstream in("test_data.json");
        if (!in.is_open()) {
            throw std::runtime_error("Cannot open test_data.json for reading");
        }

        bool isFirstChunk = true;
        while (in) {
            std::vector<json> records;
            records.reserve(CHUNK_SIZE);
            std::string line;
            for (size_t i = 0; i < CHUNK_SIZE && std::getline(in, line); ++i) {
                if (line.empty()) continue;
                try {
                    records.push_back(json::parse(line));
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
                JsonParser::analyzeAndSortFields(records, dict, fieldOrder);
                printFieldOrder(fieldOrder);
                // printDictionary(dict, fieldOrder);
                trie = std::make_unique<Trie>(fieldOrder);
                isFirstChunk = false;
            } else {
                std::cout << "--- Processing Subsequent Chunk (" << records.size() << " records) ---\n";
            }

            // Insert records from the current chunk into the Trie
            for (const auto& record : records) {
                trie->insert(record, dict);
            }
        }
        in.close();

        if (!trie) {
            std::cout << "No records processed, exiting." << std::endl;
            return 0;
        }

        // Verification and reconstruction steps remain the same
        // std::cout << "\n=== Trie Tree Structure ===\n";
        // printTrieNode(trie->getRoot(), 0, fieldOrder, dict);
        
        std::string reconstructed_json = trie->toJson(dict);
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