#include "../include/field_parser.h"
#include <set>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <functional>

namespace json2 {

FieldType FieldParser::inferFieldType(const std::string& field_name, 
                                     simdjson::dom::element value_node, 
                                     FieldDictionaryManager& manager) {
    if (value_node.type() == simdjson::dom::element_type::STRING) {
        auto str_res = value_node.get_string();
        if (!str_res.error()) {
            std::string value_str = std::string(str_res.value());
            // 统一类型推断逻辑
            if (manager.isTimestampField(field_name) || manager.isTimestampValue(value_str)) {
                return FieldType::Timestamp;
            } else if (manager.isLogTemplate(value_str)) {
                return FieldType::LogType;
            } else {
                return FieldType::String;
            }
        }
        return FieldType::String;
    } else {
        switch (value_node.type()) {
            case simdjson::dom::element_type::INT64:
                return FieldType::Int;
            case simdjson::dom::element_type::DOUBLE:
                return FieldType::Double;
            case simdjson::dom::element_type::BOOL:
                return FieldType::Bool;
            case simdjson::dom::element_type::NULL_VALUE:
                return FieldType::Null;
            default:
                return FieldType::String;
        }
    }
}

Value FieldParser::extractValue(simdjson::dom::element value_node) {
    switch(value_node.type()) {
        case simdjson::dom::element_type::STRING:
            return std::string(value_node.get_string().value());
        case simdjson::dom::element_type::INT64:
            return value_node.get_int64().value();
        case simdjson::dom::element_type::UINT64:
            return static_cast<int64_t>(value_node.get_uint64().value());
        case simdjson::dom::element_type::DOUBLE:
            return value_node.get_double().value();
        case simdjson::dom::element_type::BOOL:
            return value_node.get_bool().value();
        case simdjson::dom::element_type::NULL_VALUE:
            return nullptr;
        default:
            return std::string();
    }
}

simdjson::dom::element FieldParser::getNestedField(simdjson::dom::element node, const std::string& field) {
    // 首先检查是否是真正的嵌套字段（包含数组索引）
    if (field.find('[') != std::string::npos) {
        // 这是真正的嵌套字段，使用原来的逻辑
        simdjson::dom::element current = node;
        size_t start = 0;
        size_t end = field.find('.');
        
        while (start < field.length()) {
            std::string part;
            if (end == std::string::npos) {
                part = field.substr(start);
            } else {
                part = field.substr(start, end - start);
            }
            
            size_t bracket_pos = part.find('[');
            if (bracket_pos != std::string::npos) {
                std::string key = part.substr(0, bracket_pos);
                if (!key.empty()) {
                    auto result = current[key];
                    if (result.error()) {
                        throw simdjson::simdjson_error(result.error());
                    }
                    current = result.value();
                }
                size_t end_bracket_pos = part.find(']', bracket_pos);
                int index = std::stoi(part.substr(bracket_pos + 1, end_bracket_pos - bracket_pos - 1));
                auto result = current.at(index);
                if (result.error()) {
                    throw simdjson::simdjson_error(result.error());
                }
                current = result.value();
            } else {
                auto result = current[part.c_str()];
                if (result.error()) {
                    throw simdjson::simdjson_error(result.error());
                }
                current = result.value();
            }
            
            if (end == std::string::npos) break;
            start = end + 1;
            end = field.find('.', start);
        }
        return current;
    } else {
        // 检查是否为嵌套字段（包含点号但不包含数组索引）
        if (field.find('.') != std::string::npos) {
            // 尝试直接访问，如果失败则按嵌套字段处理
            auto result = node[field.c_str()];
            if (!result.error()) {
                // 直接访问成功，说明这是扁平字段名
                return result.value();
            } else {
                // 直接访问失败，按嵌套字段处理
                simdjson::dom::element current = node;
                size_t start = 0;
                size_t end = field.find('.');
                
                while (start < field.length()) {
                    std::string part;
                    if (end == std::string::npos) {
                        part = field.substr(start);
                    } else {
                        part = field.substr(start, end - start);
                    }
                    
                    auto result = current[part.c_str()];
                    if (result.error()) {
                        throw simdjson::simdjson_error(result.error());
                    }
                    current = result.value();
                    
                    if (end == std::string::npos) break;
                    start = end + 1;
                    end = field.find('.', start);
                }
                return current;
            }
        } else {
            // 这是真正的扁平字段，直接访问
            auto result = node[field.c_str()];
            if (result.error()) {
                throw simdjson::simdjson_error(result.error());
            }
            return result.value();
        }
    }
}

std::vector<std::tuple<std::string, FieldType, Value>> FieldParser::parseFields(
    const std::string& record, FieldDictionaryManager& manager) {
    
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
            default: {
                FieldType type = inferFieldType(prefix, node, manager);
                Value value = extractValue(node);
                result.emplace_back(prefix, type, value);
                break;
            }
        }
    };
    collect(doc, "");
    return result;
}

void FieldParser::collectAllFields(simdjson::dom::element node, 
                                  const std::string& prefix, 
                                  int depth,
                                  std::set<FieldKey>& all_fields,
                                  std::unordered_map<FieldKey, size_t>& value_counts,
                                  FieldDictionaryManager& manager) {
    switch (node.type()) {
        case simdjson::dom::element_type::OBJECT:
            for (simdjson::dom::key_value_pair field : node.get_object().value()) {
                std::string full_name = prefix.empty() ? std::string(field.key) : prefix + "." + std::string(field.key);
                collectAllFields(field.value, full_name, depth+1, all_fields, value_counts, manager);
            }
            break;
        case simdjson::dom::element_type::ARRAY: {
            size_t i = 0;
            for (simdjson::dom::element child : node.get_array().value()) {
                std::string full_name = prefix + "[" + std::to_string(i) + "]";
                collectAllFields(child, full_name, depth+1, all_fields, value_counts, manager);
                i++;
            }
            break;
        }
        default: {
            FieldType type = inferFieldType(prefix, node, manager);
            
            // 检查是否为嵌套字段（通过递归访问产生的字段）
            std::string field_name = prefix;
            // 只有通过递归访问产生的字段才是嵌套字段，才添加 ~ 前缀
            if (!prefix.empty() && depth > 1) {
                field_name = "~" + prefix;  // 添加 ~ 前缀标识嵌套字段
            }
            
            FieldKey key{field_name, type};
            all_fields.insert(key);
            value_counts[key]++;
            
            // 添加到manager
            Value value = extractValue(node);
            switch (type) {
                case FieldType::String:
                case FieldType::Timestamp:
                case FieldType::LogType:
                    if (std::holds_alternative<std::string>(value)) {
                        manager.addFieldValue(key, type, std::get<std::string>(value));
                    } else {
                        manager.addFieldValue(key, type, "");
                    }
                    break;
                case FieldType::Int:
                    if (std::holds_alternative<int64_t>(value)) {
                        manager.addFieldValue(key, type, std::get<int64_t>(value));
                    } else {
                        manager.addFieldValue(key, type, int64_t(0));
                    }
                    break;
                case FieldType::Double:
                    if (std::holds_alternative<double>(value)) {
                        manager.addFieldValue(key, type, std::get<double>(value));
                    } else {
                        manager.addFieldValue(key, type, 0.0);
                    }
                    break;
                case FieldType::Bool:
                    if (std::holds_alternative<bool>(value)) {
                        manager.addFieldValue(key, type, std::get<bool>(value));
                    } else {
                        manager.addFieldValue(key, type, false);
                    }
                    break;
                case FieldType::Null:
                    manager.addFieldValue(key, type, nullptr);
                    break;
                default:
                    manager.addFieldValue(key, type, "");
                    break;
            }
            break;
        }
    }
}

} // namespace json2
