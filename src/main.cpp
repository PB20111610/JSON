#include "../include/parser.h"
#include "../include/dictionary.h"
#include "../include/trie.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

using namespace json2;

void printDictionaryStats(const Dictionary& dict) {
    std::cout << "\nDictionary Statistics:\n";
    std::cout << "=====================\n";
    
    // 打印每种类型的字典大小
    std::cout << "Timestamp Dictionary Size: " << dict.size(DictType::TIMESTAMP_DICT) << "\n";
    std::cout << "Log Dictionary Size: " << dict.size(DictType::LOG_DICT) << "\n";
    std::cout << "Variable Dictionary Size: " << dict.size(DictType::VARIABLE_DICT) << "\n\n";
    
    // 计算总记录数（使用最大出现次数作为估计）
    size_t total_records = 0;
    for (const auto& stats : dict.getFieldStats()) {
        total_records = std::max(total_records, stats.occurrence_count);
    }
    
    // 打印字段值域统计和冗余度因子
    std::cout << "Field Statistics and Redundancy Factors:\n";
    std::cout << "=====================================\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::setw(15) << "Field" 
              << std::setw(15) << "Type" 
              << std::setw(15) << "Value Count" 
              << std::setw(15) << "Occurrences" 
              << std::setw(15) << "Redundancy" 
              << "\n";
    std::cout << std::string(75, '-') << "\n";
    
    // 创建字段统计的副本用于排序
    std::vector<FieldStats> sorted_stats = dict.getFieldStats();
    
    // 按冗余度因子排序
    std::sort(sorted_stats.begin(), sorted_stats.end(),
        [total_records](const FieldStats& a, const FieldStats& b) {
            double redundancy_a = static_cast<double>(a.occurrence_count) / 
                                 ((a.value_count + 1e-6) * total_records);
            double redundancy_b = static_cast<double>(b.occurrence_count) / 
                                 ((b.value_count + 1e-6) * total_records);
            return redundancy_a > redundancy_b;
        });
    
    // 打印排序后的字段统计
    for (const auto& stats : sorted_stats) {
        double redundancy_factor = static_cast<double>(stats.occurrence_count) / 
                                  ((stats.value_count + 1e-6) * total_records);
        
        std::cout << std::setw(15) << stats.name 
                  << std::setw(15) 
                  << (stats.type == DictType::TIMESTAMP_DICT ? "TIMESTAMP" :
                      stats.type == DictType::LOG_DICT ? "LOG" : "VARIABLE")
                  << std::setw(15) << stats.value_count
                  << std::setw(15) << stats.occurrence_count
                  << std::setw(15) << redundancy_factor
                  << "\n";
    }
    
    // 打印字段顺序（按冗余度因子）
    std::cout << "\nField Order (by redundancy factor):\n";
    std::cout << "================================\n";
    auto ordered_fields = dict.getOrderedFields();
    for (size_t i = 0; i < ordered_fields.size(); ++i) {
        std::cout << (i + 1) << ". " << ordered_fields[i] << "\n";
    }
}

// 打印字典内容
void printDictionary(const Dictionary& dict) {
    std::cout << "\n=== Dictionary Contents ===\n";
    
    // 打印时间戳字典
    std::cout << "\nTimestamp Dictionary:\n";
    const auto& timestamp_codes = dict.getCodes(DictType::TIMESTAMP_DICT);
    for (size_t i = 0; i < timestamp_codes.size(); ++i) {
        std::cout << "Code " << (i + 1) << ": " << timestamp_codes[i] << "\n";
    }
    
    // 打印日志字典
    std::cout << "\nLog Dictionary:\n";
    const auto& log_codes = dict.getCodes(DictType::LOG_DICT);
    for (size_t i = 0; i < log_codes.size(); ++i) {
        std::cout << "Code " << (i + 1) << ": " << log_codes[i] << "\n";
    }
    
    // 打印变量字典
    std::cout << "\nVariable Dictionary:\n";
    const auto& variable_codes = dict.getCodes(DictType::VARIABLE_DICT);
    for (size_t i = 0; i < variable_codes.size(); ++i) {
        std::cout << "Code " << (i + 1) << ": " << variable_codes[i] << "\n";
    }
}

// 递归打印Trie树结构
void printTrieNode(const TrieNode* node, int depth, const std::vector<ParsedField>& fields, const Dictionary& dict) {
    if (!node) return;
    
    // 打印当前节点
    std::cout << std::string(depth * 2, ' ') << "Depth " << depth << ": ";
    if (node->isPlaceholder()) {
        std::cout << "[PLACEHOLDER]\n";
    } else {
        std::cout << "Code " << node->getCode() << " -> ";
        if (depth < fields.size()) {
            const auto& field = fields[depth];
            std::cout << dict.getString(node->getCode(), field.dictType) << "\n";
        } else {
            std::cout << "Unknown\n";
        }
    }
    
    // 递归打印子节点
    for (const auto& [code, child] : node->getChildren()) {
        printTrieNode(child.get(), depth + 1, fields, dict);
    }
}

// 打印序列化数据
void printSerializedData(const std::vector<uint8_t>& data) {
    std::cout << "\n=== Serialized Data ===\n";
    std::cout << "Size: " << data.size() << " bytes\n";
    std::cout << "Hex dump:\n";
    
    for (size_t i = 0; i < data.size(); ++i) {
        if (i % 16 == 0) {
            if (i > 0) std::cout << "\n";
            std::cout << std::hex << std::setw(4) << std::setfill('0') << i << ": ";
        }
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
    }
    std::cout << std::dec << "\n";
}

int main() {
    try {
        // 1. 创建字典和字段列表
        Dictionary dict;
        std::vector<ParsedField> fieldOrder;
        
        // 2. 解析并收集字段信息
        JsonParser::parseAndCollect("test_data.json", dict, fieldOrder);
        
        // 3. 打印字段顺序
        std::cout << "=== Field Order ===\n";
        for (const auto& field : fieldOrder) {
            std::cout << field.name << " (Type: ";
            switch (field.dictType) {
                case DictType::TIMESTAMP_DICT: std::cout << "TIMESTAMP"; break;
                case DictType::LOG_DICT: std::cout << "LOG"; break;
                case DictType::VARIABLE_DICT: std::cout << "VARIABLE"; break;
            }
            std::cout << ")\n";
        }
        
        // 4. 打印字典内容
        printDictionary(dict);
        
        // 5. 创建Trie树
        Trie trie(fieldOrder);
        
        // 6. 解析并插入记录
        auto records = JsonParser::parseLogFile("test_data.json");
        for (const auto& record : records) {
            trie.insert(record, dict);
        }
        
        // 7. 打印Trie树结构
        std::cout << "\n=== Trie Tree Structure ===\n";
        printTrieNode(trie.getRoot(), 0, fieldOrder, dict);
        
        // 8. 序列化Trie树
        auto serialized = trie.serialize();
        printSerializedData(serialized);
        
        // 9. 反序列化Trie树
        Trie newTrie(fieldOrder);
        newTrie.deserialize(serialized);
        
        // 10. 验证反序列化后的树结构
        std::cout << "\n=== Deserialized Trie Tree Structure ===\n";
        printTrieNode(newTrie.getRoot(), 0, fieldOrder, dict);
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}