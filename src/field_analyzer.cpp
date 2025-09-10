#include "../include/field_analyzer.h"
#include <set>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <simdjson.h>
#include <sstream>
#include "../include/field_dictionary_manager.h"
#include "../include/field_parser.h"
#include <regex>
#include <cctype>
#include <chrono>
#include <cmath> 

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
        double factor = manager.calculateRedundancy(key, occ, val_count); // 方案B：基于信息熵 (Best)
        redundancy.emplace_back(key, factor);
        ++red_idx;
    }
    std::sort(redundancy.begin(), redundancy.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // Debug output with enhanced statistics
    std::cout << "\n[SORTING DEBUG] Final sorted order (by redundancy):" << std::endl;
    std::cout << "====================================================" << std::endl;
    for (size_t i = 0; i < redundancy.size(); ++i) {
        const auto& key = redundancy[i].first;
        double red_value = redundancy[i].second;
        size_t occ = manager.getTotalCount(key);
        size_t val_count = manager.getUniqueValueCount(key);
        
        // Calculate additional statistics
        double coverage = (double)occ / records.size(); // Field coverage percentage
        double compression_ratio = val_count > 0 ? (double)occ / val_count : 0.0;
        double entropy = 0.0;
        if (val_count > 1) {
            // Simplified entropy calculation (assuming uniform distribution)
            entropy = log2(val_count);
        }
        double normalized_entropy = val_count > 1 ? entropy / log2(val_count) : 0.0;
        
        std::cout << std::setw(4) << (i + 1) << ". " 
                  << key.name << "[" << (key.type == FieldType::String ? "String" : 
                       key.type == FieldType::Int ? "Int" : 
                       key.type == FieldType::Double ? "Double" : 
                       key.type == FieldType::Bool ? "Bool" : 
                       key.type == FieldType::Timestamp ? "Timestamp" : 
                       key.type == FieldType::LogType ? "LogType" : 
                       key.type == FieldType::Null ? "Null" : "Other") << "]" 
                  << " - Occ: " << occ 
                  << ", Uniq: " << val_count 
                  << ", Cov: " << std::fixed << std::setprecision(2) << (coverage * 100) << "%"
                  << ", CompRatio: " << std::fixed << std::setprecision(1) << compression_ratio
                  << ", Entropy: " << std::fixed << std::setprecision(2) << entropy
                  << ", Red: " << std::scientific << std::setprecision(2) << red_value << std::endl;
    }

    ordered_fields.clear();
    for (const auto& [key, _] : redundancy) {
        ordered_fields.push_back(key);
    }
}

void FieldAnalyzer::analyzeWithCustomOrder(const std::vector<FieldKey>& custom_order, std::vector<FieldKey>& ordered_fields) {
    ordered_fields = custom_order;
}

// 类型敏感主解析接口（保留，便于单条记录类型推断）
std::vector<std::tuple<std::string, FieldType, Value>> FieldAnalyzer::parseFields(const std::string& record) {
    // 使用统一的字段解析工具
    FieldDictionaryManager temp_manager; // 临时manager用于类型推断
    return FieldParser::parseFields(record, temp_manager);
}

} // namespace json2
