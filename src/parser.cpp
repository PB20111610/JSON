#include "../include/parser.h"
#include <set>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <simdjson.h>
#include <sstream>
#include "../include/field_dictionary_manager.h"
#include <regex>
#include <cctype>
#include <chrono>
#include <iomanip>

namespace json2 {

// 递归收集字段并插入 manager
void collectFieldsSimd(simdjson::dom::element node, const std::string& prefix, std::set<std::string>& fields, std::unordered_map<std::string, size_t>& value_counts, FieldDictionaryManager& manager) {
    switch (node.type()) {
        case simdjson::dom::element_type::OBJECT:
            for (simdjson::dom::key_value_pair field : node.get_object().value()) {
                std::string full_name = prefix.empty() ? std::string(field.key) : prefix + "." + std::string(field.key);
                collectFieldsSimd(field.value, full_name, fields, value_counts, manager);
            }
            break;
        case simdjson::dom::element_type::ARRAY:
            {
                size_t i = 0;
                for (simdjson::dom::element child : node.get_array().value()) {
                    std::string full_name = prefix + "[" + std::to_string(i) + "]";
                    collectFieldsSimd(child, full_name, fields, value_counts, manager);
                    i++;
                }
            }
            break;
        default:
            // It's a scalar type
            fields.insert(prefix);
            // 类型敏感处理
            std::string value_str;
            FieldType type;
            switch (node.type()) {
                case simdjson::dom::element_type::STRING:
                    value_str = node.get_string().value();
                    // manager 内部自动判断 timestamp/logtype/string
                    type = FieldType::String;
                    break;
                case simdjson::dom::element_type::INT64:
                    value_str = std::to_string(node.get_int64().value());
                    type = FieldType::Int;
                    break;
                case simdjson::dom::element_type::DOUBLE:
                    value_str = std::to_string(node.get_double().value());
                    type = FieldType::Double;
                    break;
                case simdjson::dom::element_type::BOOL:
                    value_str = node.get_bool().value() ? "true" : "false";
                    type = FieldType::Bool;
                    break;
                case simdjson::dom::element_type::NULL_VALUE:
                    value_str = "";
                    type = FieldType::Null;
                    break;
                default:
                    value_str = "";
                    type = FieldType::String;
                    break;
            }
            manager.addFieldValue(prefix, type, value_str);
            value_counts[prefix]++;
            break;
    }
}

void JsonParser::analyzeAndSortFields(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<std::string>& ordered_fields) {
    std::set<std::string> all_fields;
    std::unordered_map<std::string, size_t> value_counts;
    simdjson::dom::parser parser;

    for (const auto& rec_str : records) {
        try {
            simdjson::dom::element doc = parser.parse(rec_str).value();
            collectFieldsSimd(doc, "", all_fields, value_counts, manager);
        } catch (const simdjson::simdjson_error& e) {
            std::cerr << "simdjson parse error: " << e.what() << std::endl;
        }
    }

    // 类型敏感冗余度计算
    std::vector<std::pair<std::string, double>> redundancy;
    for (const auto& field : all_fields) {
        double max_factor = 0.0;
        for (int t = (int)FieldType::Int; t <= (int)FieldType::Null; ++t) {
            FieldType type = static_cast<FieldType>(t);
            size_t occ = manager.getTotalCount(field, type);
            size_t val_count = manager.getUniqueValueCount(field, type);
            double factor = (val_count > 0) ? (double)occ / val_count : 0.0;
            if (factor > max_factor) max_factor = factor;
        }
        redundancy.emplace_back(field, max_factor);
    }
    std::sort(redundancy.begin(), redundancy.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    ordered_fields.clear();
    for (const auto& [field, _] : redundancy) {
        ordered_fields.push_back(field);
    }
}

// 类型敏感主解析接口（保留，便于单条记录类型推断）
std::vector<std::tuple<std::string, FieldType, Value>> JsonParser::parseFields(const std::string& record) {
    std::vector<std::tuple<std::string, FieldType, Value>> result;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(record).get(doc)) return result;

    std::function<void(simdjson::dom::element, const std::string&)> collect;
    collect = [&](simdjson::dom::element node, const std::string& prefix) {
        switch (node.type()) {
            case simdjson::dom::element_type::OBJECT:
                for (auto field : node.get_object().value()) {
                    std::string full_name = prefix.empty() ? std::string(field.key) : prefix + "." + std::string(field.key);
                    collect(field.value, full_name);
                }
                break;
            case simdjson::dom::element_type::ARRAY: {
                size_t i = 0;
                for (auto child : node.get_array().value()) {
                    std::string full_name = prefix + "[" + std::to_string(i) + "]";
                    collect(child, full_name);
                    ++i;
                }
                break;
            }
            case simdjson::dom::element_type::STRING: {
                std::string value(node.get_string().value());
                // 这里不再做 timestamp/logtype 判别，交由 manager 统一处理
                result.emplace_back(prefix, FieldType::String, value);
                break;
            }
            case simdjson::dom::element_type::INT64:
                result.emplace_back(prefix, FieldType::Int, node.get_int64().value());
                break;
            case simdjson::dom::element_type::DOUBLE:
                result.emplace_back(prefix, FieldType::Double, node.get_double().value());
                break;
            case simdjson::dom::element_type::BOOL:
                result.emplace_back(prefix, FieldType::Bool, node.get_bool().value());
                break;
            case simdjson::dom::element_type::NULL_VALUE:
                result.emplace_back(prefix, FieldType::Null, nullptr);
                break;
            default:
                break;
        }
    };
    collect(doc, "");
    return result;
}

} // namespace json2
