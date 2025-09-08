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
        
        // 新增：计算长度熵和相邻自相似度
        double length_entropy = manager.calculateLengthEntropy(key);
        double adjacent_similarity = manager.calculateAdjacentSelfSimilarity(key);
        
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
                  << ", H_len: " << std::fixed << std::setprecision(3) << length_entropy
                  << ", S_adj: " << std::fixed << std::setprecision(3) << adjacent_similarity
                  << ", Red: " << std::scientific << std::setprecision(2) << red_value << std::endl;
    }

    ordered_fields.clear();
    for (const auto& [key, _] : redundancy) {
        ordered_fields.push_back(key);
    }
}

void FieldAnalyzer::analyzeWithCustomOrder(const std::vector<std::string>& records, FieldDictionaryManager& manager, 
                                           const std::vector<FieldKey>& custom_order, std::vector<FieldKey>& ordered_fields) {
    ordered_fields = custom_order;
}

bool FieldAnalyzer::validateCustomOrder(const std::vector<std::string>& records, const std::vector<FieldKey>& custom_order, 
                                        std::string& error_message) {
    if (custom_order.empty()) {
        error_message = "Custom field order cannot be empty";
        return false;
    }
    
    if (records.empty()) {
        error_message = "Records cannot be empty for validation";
        return false;
    }

    // 收集实际数据中的所有字段
    std::set<FieldKey> all_fields;
    std::unordered_map<FieldKey, size_t> value_counts;
    FieldDictionaryManager temp_manager;
    simdjson::dom::parser parser;

    for (const auto& rec_str : records) {
        try {
            simdjson::dom::element doc = parser.parse(rec_str).value();
            FieldParser::collectAllFields(doc, "", 0, all_fields, value_counts, temp_manager);
        } catch (const simdjson::simdjson_error& e) {
            // 跳过无效的JSON记录
            continue;
        }
    }

    if (all_fields.empty()) {
        error_message = "No valid fields found in the provided records";
        return false;
    }

    // 验证自定义排序中的每个字段
    std::vector<std::string> invalid_fields;
    std::vector<std::string> type_mismatch_fields;
    
    for (const auto& custom_field : custom_order) {
        auto it = all_fields.find(custom_field);
        if (it == all_fields.end()) {
            // 检查是否存在同名但不同类型的字段
            bool found_same_name = false;
            for (const auto& actual_field : all_fields) {
                if (actual_field.name == custom_field.name && actual_field.type != custom_field.type) {
                    found_same_name = true;
                    std::string type_info = custom_field.name + "[expected:" + 
                        (custom_field.type == FieldType::String ? "String" : 
                         custom_field.type == FieldType::Int ? "Int" : 
                         custom_field.type == FieldType::Double ? "Double" : 
                         custom_field.type == FieldType::Bool ? "Bool" : 
                         custom_field.type == FieldType::Timestamp ? "Timestamp" : 
                         custom_field.type == FieldType::LogType ? "LogType" : "Other") + 
                        ", actual:" + 
                        (actual_field.type == FieldType::String ? "String" : 
                         actual_field.type == FieldType::Int ? "Int" : 
                         actual_field.type == FieldType::Double ? "Double" : 
                         actual_field.type == FieldType::Bool ? "Bool" : 
                         actual_field.type == FieldType::Timestamp ? "Timestamp" : 
                         actual_field.type == FieldType::LogType ? "LogType" : "Other") + "]";
                    type_mismatch_fields.push_back(type_info);
                    break;
                }
            }
            if (!found_same_name) {
                invalid_fields.push_back(custom_field.name + "[" + 
                    (custom_field.type == FieldType::String ? "String" : 
                     custom_field.type == FieldType::Int ? "Int" : 
                     custom_field.type == FieldType::Double ? "Double" : 
                     custom_field.type == FieldType::Bool ? "Bool" : 
                     custom_field.type == FieldType::Timestamp ? "Timestamp" : 
                     custom_field.type == FieldType::LogType ? "LogType" : "Other") + "]");
            }
        }
    }

    // 生成错误消息
    if (!invalid_fields.empty() || !type_mismatch_fields.empty()) {
        std::ostringstream oss;
        oss << "Validation failed: ";
        
        if (!invalid_fields.empty()) {
            oss << "Fields not found in data: ";
            for (size_t i = 0; i < invalid_fields.size(); ++i) {
                oss << invalid_fields[i];
                if (i < invalid_fields.size() - 1) oss << ", ";
            }
        }
        
        if (!invalid_fields.empty() && !type_mismatch_fields.empty()) {
            oss << "; ";
        }
        
        if (!type_mismatch_fields.empty()) {
            oss << "Type mismatches: ";
            for (size_t i = 0; i < type_mismatch_fields.size(); ++i) {
                oss << type_mismatch_fields[i];
                if (i < type_mismatch_fields.size() - 1) oss << ", ";
            }
        }
        
        error_message = oss.str();
        return false;
    }

    return true;
}

// 类型敏感主解析接口（保留，便于单条记录类型推断）
std::vector<std::tuple<std::string, FieldType, Value>> FieldAnalyzer::parseFields(const std::string& record) {
    // 使用统一的字段解析工具
    FieldDictionaryManager temp_manager; // 临时manager用于类型推断
    return FieldParser::parseFields(record, temp_manager);
}

} // namespace json2
