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
            // 检查是否为嵌套字段（通过递归访问产生的字段）
            std::string field_name = prefix;
            // 只有通过递归访问产生的字段才是嵌套字段，才添加 ~ 前缀
            if (!prefix.empty()) {
                field_name = "~" + prefix;  // 添加 ~ 前缀标识嵌套字段
            }
            fields.insert(field_name);
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
            manager.addFieldValue(FieldKey{field_name, type}, value_str);
            value_counts[field_name]++;
            break;
    }
}

void JsonParser::analyzeAndSortFields(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<FieldKey>& ordered_fields) {
    std::set<FieldKey> all_fields;
    std::unordered_map<FieldKey, size_t> value_counts;
    simdjson::dom::parser parser;

    // std::cout << "[DEBUG] analyzeAndSortFields: records.size()=" << records.size() << std::endl;
    size_t rec_idx = 0;
    for (const auto& rec_str : records) {
        try {
            // if (rec_idx % 50 == 0) std::cout << "[DEBUG] 解析第 " << rec_idx << " 条记录" << std::endl;
            // std::cout << "[DEBUG] simdjson parse 前, rec_idx=" << rec_idx << std::endl;
            simdjson::dom::element doc = parser.parse(rec_str).value();
            // std::cout << "[DEBUG] simdjson parse 后, rec_idx=" << rec_idx << std::endl;
            // 递归收集字段名+类型
            std::function<void(simdjson::dom::element, const std::string&, int)> collect;
            collect = [&](simdjson::dom::element node, const std::string& prefix, int depth) {
                // if (depth < 10 || depth % 10 == 0) std::cout << "[DEBUG] collect 递归, prefix='" << prefix << "', depth=" << depth << std::endl;
                switch (node.type()) {
                    case simdjson::dom::element_type::OBJECT:
                        for (simdjson::dom::key_value_pair field : node.get_object().value()) {
                            std::string full_name = prefix.empty() ? std::string(field.key) : prefix + "." + std::string(field.key);
                            collect(field.value, full_name, depth+1);
                        }
                        break;
                    case simdjson::dom::element_type::ARRAY: {
                        size_t i = 0;
                        for (simdjson::dom::element child : node.get_array().value()) {
                            std::string full_name = prefix + "[" + std::to_string(i) + "]";
                            collect(child, full_name, depth+1);
                            i++;
                        }
                        break;
                    }
                    default: {
                        FieldType type;
                        if (node.type() == simdjson::dom::element_type::STRING) {
                            std::string value_str(node.get_string().value());
                            if (manager.isTimestampField(prefix) && manager.isTimestampValue(value_str)) {
                                type = FieldType::Timestamp;
                            } else if (manager.isLogTemplate(value_str)) {
                                type = FieldType::LogType;
                            } else {
                                type = FieldType::String;
                            }
                        } else {
                            switch (node.type()) {
                                case simdjson::dom::element_type::INT64:
                                    type = FieldType::Int;
                                    break;
                                case simdjson::dom::element_type::DOUBLE:
                                    type = FieldType::Double;
                                    break;
                                case simdjson::dom::element_type::BOOL:
                                    type = FieldType::Bool;
                                    break;
                                case simdjson::dom::element_type::NULL_VALUE:
                                    type = FieldType::Null;
                                    break;
                                default:
                                    type = FieldType::String;
                                    break;
                            }
                        }
                        // 检查是否为嵌套字段（通过递归访问产生的字段）
                        std::string field_name = prefix;
                        // 只有通过递归访问产生的字段才是嵌套字段，才添加 ~ 前缀
                        // 真正的嵌套字段应该是在对象内部通过递归访问产生的
                        if (!prefix.empty() && depth > 1) {
                            field_name = "~" + prefix;  // 添加 ~ 前缀标识嵌套字段
                        }
                        FieldKey key{field_name, static_cast<FieldType>(type)};
                        all_fields.insert(key);
                        value_counts[key]++;
                        // std::cout << "[DEBUG] addFieldValue 前, prefix='" << prefix << "', type=" << static_cast<int>(type) << ", depth=" << depth << std::endl;
                        switch (node.type()) {
                            case simdjson::dom::element_type::STRING:
                                manager.addFieldValue(FieldKey{field_name, type}, type, std::string(node.get_string().value()));
                                break;
                            case simdjson::dom::element_type::INT64:
                                manager.addFieldValue(FieldKey{field_name, type}, type, node.get_int64().value());
                                break;
                            case simdjson::dom::element_type::DOUBLE:
                                manager.addFieldValue(FieldKey{field_name, type}, type, node.get_double().value());
                                break;
                            case simdjson::dom::element_type::BOOL:
                                manager.addFieldValue(FieldKey{field_name, type}, type, node.get_bool().value());
                                break;
                            case simdjson::dom::element_type::NULL_VALUE:
                                manager.addFieldValue(FieldKey{field_name, type}, type, nullptr);
                                break;
                            default:
                                manager.addFieldValue(FieldKey{field_name, type}, type, "");
                                break;
                        }
                        // std::cout << "[DEBUG] addFieldValue 后, prefix='" << prefix << "', type=" << static_cast<int>(type) << ", depth=" << depth << std::endl;
                        break;
                    }
                }
            };
            // std::cout << "[DEBUG] collect(doc, ""), rec_idx=" << rec_idx << std::endl;
            collect(doc, "", 0);
            // std::cout << "[DEBUG] collect(doc, "") 返回, rec_idx=" << rec_idx << std::endl;
        } catch (const simdjson::simdjson_error& e) {
            std::cerr << "simdjson parse error: " << e.what() << std::endl;
            std::cerr << "[DEBUG] 错误行内容: " << rec_str.substr(0, 100) << std::endl;
        }
        ++rec_idx;
    }
    // std::cout << "[DEBUG] analyzeAndSortFields: 字段收集完毕, all_fields.size()=" << all_fields.size() << std::endl;

    // 冗余度计算
    std::vector<std::pair<FieldKey, double>> redundancy;
    size_t red_idx = 0;
    for (const auto& key : all_fields) {
        // if (red_idx % 50 == 0) std::cout << "[DEBUG] 冗余度计算 key#" << red_idx << std::endl;
        size_t occ = manager.getTotalCount(key);
        size_t val_count = manager.getUniqueValueCount(key);
        double factor = (val_count > 0) ? (double)occ / val_count : 0.0;
        redundancy.emplace_back(key, factor);
        ++red_idx;
    }
    // std::cout << "[DEBUG] analyzeAndSortFields: 冗余度计算完毕, redundancy.size()=" << redundancy.size() << std::endl;
    std::sort(redundancy.begin(), redundancy.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    // std::cout << "[DEBUG] analyzeAndSortFields: 冗余度排序完毕" << std::endl;

    ordered_fields.clear();
    for (const auto& [key, _] : redundancy) {
        ordered_fields.push_back(key);
    }
    // std::cout << "[DEBUG] analyzeAndSortFields: ordered_fields.size()=" << ordered_fields.size() << std::endl;
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
