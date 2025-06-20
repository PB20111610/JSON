#include "../include/parser.h"
#include <nlohmann/json.hpp>
#include <fstream>
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

void JsonParser::parseAndCollect(const std::string& filename, Dictionary& dict, std::vector<std::string>& ordered_fields) {
    std::ifstream in(filename);
    if (!in.is_open()) throw std::runtime_error("Cannot open file: " + filename);
    std::string line;
    std::set<std::string> all_fields;
    std::unordered_map<std::string, size_t> value_counts;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json j;
        try {
            j = json::parse(line);
        } catch (const std::exception& e) {
            std::cerr << "JSON parse error: " << e.what() << "\n";
            continue;
        }
        collectFields(j, "", all_fields, value_counts, dict);
    }
    in.close();
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

// 你可以自定义返回类型，这里只返回原始json对象
std::vector<std::shared_ptr<void>> JsonParser::parseLogFile(const std::string& filename) {
    std::vector<std::shared_ptr<void>> records;
    std::ifstream in(filename);
    if (!in.is_open()) throw std::runtime_error("Cannot open file: " + filename);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = std::make_shared<json>(json::parse(line));
            records.push_back(j);
        } catch (...) {}
    }
    in.close();
    return records;
}

}
