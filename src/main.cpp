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
    try {
        Dictionary dict;
        std::vector<std::string> fieldOrder;
        // 解析并收集字段信息（假设 parser 负责排序字段）
        JsonParser::parseAndCollect("test_data.json", dict, fieldOrder);
        // 打印字段顺序
        printFieldOrder(fieldOrder);
        // 打印字典内容
        printDictionary(dict, fieldOrder);
        // 创建Trie树
        Trie trie(fieldOrder);
        // 解析并插入记录
        std::vector<json> records;
        std::ifstream in("test_data.json");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                records.push_back(json::parse(line));
            } catch (...) {}
        }
        in.close();
        for (const auto& record : records) {
            trie.insert(record, dict);
        }
        // 打印Trie树结构
        std::cout << "\n=== Trie Tree Structure ===\n";
        printTrieNode(trie.getRoot(), 0, fieldOrder, dict);
        // 将Trie树转换回JSON
        std::string reconstructed_json = trie.toJson(dict);
        // 保存重建的JSON
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