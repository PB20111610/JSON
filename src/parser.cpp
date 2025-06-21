#include "../include/parser.h"
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <iostream>

using nlohmann::json;

namespace json2 {

// 递归收集所有字段名（扁平化），并统计每个字段的所有原始字符串值
void collectFields(const json& j, const std::string& prefix, std::set<std::string>& fields, std::unordered_map<std::string, size_t>& value_counts, Dictionary& dict) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string full_name = prefix.empty() ? it.key() : prefix + "." + it.key();
            collectFields(it.value(), full_name, fields, value_counts, dict);
        }
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size(); ++i) {
            std::string full_name = prefix + "[" + std::to_string(i) + "]";
            collectFields(j[i], full_name, fields, value_counts, dict);
        }
    } else {
        // 基本类型
        fields.insert(prefix);
        std::string value = j.is_string() ? j.get<std::string>() : j.dump();
        dict.addFieldValue(prefix, value);
        value_counts[prefix]++;
    }
}

void JsonParser::analyzeAndSortFields(const std::vector<nlohmann::json>& records, Dictionary& dict, std::vector<std::string>& ordered_fields) {
    std::set<std::string> all_fields;
    std::unordered_map<std::string, size_t> value_counts;

    for (const auto& j : records) {
        collectFields(j, "", all_fields, value_counts, dict);
    }

    // 字段冗余度排序（出现次数/不同值数量，降序）
    std::vector<std::pair<std::string, double>> redundancy;
    for (const auto& field : all_fields) {
        size_t occ = value_counts[field];
        size_t val_count = dict.getFieldValueCount(field);
        double factor = (val_count > 0) ? (double)occ / val_count : 0.0;
        redundancy.emplace_back(field, factor);
    }
    std::sort(redundancy.begin(), redundancy.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    ordered_fields.clear();
    for (const auto& [field, _] : redundancy) {
        ordered_fields.push_back(field);
    }
}

}
