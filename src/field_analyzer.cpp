#include "../include/field_analyzer.h"
#include <set>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <simdjson.h>
#include <sstream>
#include "../include/field_dictionary_manager.h"
#include "../include/field_parser.h"
#include <regex>
#include <cctype>
#include <chrono>
#include <iomanip>

namespace json2 {

// 使用统一的字段解析工具

void FieldAnalyzer::analyzeAndSortFields(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<FieldKey>& ordered_fields) {
    std::set<FieldKey> all_fields;
    std::unordered_map<FieldKey, size_t> value_counts;
    simdjson::dom::parser parser;

    size_t rec_idx = 0;
    for (const auto& rec_str : records) {
        try {
            simdjson::dom::element doc = parser.parse(rec_str).value();
            // 使用统一的字段收集工具
            FieldParser::collectAllFields(doc, "", 0, all_fields, value_counts, manager);
        } catch (const simdjson::simdjson_error& e) {
            std::cerr << "simdjson parse error: " << e.what() << std::endl;
            std::cerr << "[DEBUG] 错误行内容: " << rec_str.substr(0, 100) << std::endl;
        }
        ++rec_idx;
    }

    // 冗余度计算
    std::vector<std::pair<FieldKey, double>> redundancy;
    size_t red_idx = 0;
    for (const auto& key : all_fields) {
        size_t occ = manager.getTotalCount(key);
        size_t val_count = manager.getUniqueValueCount(key);
        // double factor = (val_count > 0) ? (double)occ / val_count : 0.0;        
        // double factor = manager.calculateRedundancyA(key, occ, val_count); // 方案A：唯一值惩罚因子 (Fifth)
        double factor = manager.calculateRedundancyB(key, occ, val_count); // 方案B：基于信息熵 (Best)
        // double factor = manager.calculateRedundancyC(key, occ, val_count); // 方案C：Trie结构影响因子 (Second)
        // double factor = manager.calculateRedundancyD(key, occ, val_count); // 方案D：自适应冗余度 (Third)
        redundancy.emplace_back(key, factor);
        ++red_idx;
    }
    std::sort(redundancy.begin(), redundancy.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    ordered_fields.clear();
    for (const auto& [key, _] : redundancy) {
        ordered_fields.push_back(key);
    }
}

// 类型敏感主解析接口（保留，便于单条记录类型推断）
std::vector<std::tuple<std::string, FieldType, Value>> FieldAnalyzer::parseFields(const std::string& record) {
    // 使用统一的字段解析工具
    FieldDictionaryManager temp_manager; // 临时manager用于类型推断
    return FieldParser::parseFields(record, temp_manager);
}

} // namespace json2
