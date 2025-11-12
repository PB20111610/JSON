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
                                     FieldDictionaryManager& manager,
                                     bool is_serialized_array) {
    // 如果是序列化的数组，直接返回 Array 类型
    if (is_serialized_array) {
        return FieldType::ARRAY;
    }
    
    if (value_node.type() == simdjson::dom::element_type::STRING) {
        auto str_res = value_node.get_string();
        if (!str_res.error()) {
            std::string value_str = std::string(str_res.value());
            // 统一类型推断逻辑
            if (manager.isTimestampField(field_name) || manager.isTimestampValue(value_str)) {
                return FieldType::TIMESTAMP;
            } else if (manager.isLogTemplate(value_str)) {
                return FieldType::LOGTYPE;
            } else {
                return FieldType::STRING;
            }
        }
        return FieldType::STRING;
    } else if (value_node.type() == simdjson::dom::element_type::ARRAY) {
        // 数组类型处理 - 只返回 Array，结构化数组会被分解为具体类型字段
        return FieldType::ARRAY;
    } else {
        switch (value_node.type()) {
            case simdjson::dom::element_type::INT64:
                return FieldType::INT64;
            case simdjson::dom::element_type::DOUBLE:
                return FieldType::DOUBLE;
            case simdjson::dom::element_type::BOOL:
                return FieldType::BOOL;
            case simdjson::dom::element_type::NULL_VALUE:
                return FieldType::NULL_TYPE;
            default:
                return FieldType::STRING;
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
        case simdjson::dom::element_type::ARRAY:
            // 数组序列化为字符串
            return std::string(simdjson::to_string(value_node));
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
                // 基于 clp_s 策略的数组处理
                if (manager.getStructurizeArrays()) {
                    // 模式1：结构化数组处理（保持数组结构）
                    auto array = node.get_array().value();
                    size_t index = 0;
                    for (auto element : array) {
                        std::string element_name = prefix + "[" + std::to_string(index) + "]";
                        collect(element, element_name);
                        ++index;
                    }
                } else {
                    // 模式2：非结构化数组处理（序列化为字符串）
                    std::string array_str = simdjson::to_string(node);
                    // 创建一个临时的字符串节点来调用 inferFieldType
                    simdjson::dom::parser temp_parser;
                    auto temp_doc = temp_parser.parse(array_str);
                    if (!temp_doc.error()) {
                        FieldType type = inferFieldType(prefix, temp_doc.value(), manager, true);
                        Value value = array_str;
                        result.emplace_back(prefix, type, value);
                    } else {
                        // 如果解析失败，直接使用 ARRAY 类型
                        FieldType type = FieldType::ARRAY;
                        Value value = array_str;
                        result.emplace_back(prefix, type, value);
                    }
                }
                break;
            }
            default: {
                FieldType type = inferFieldType(prefix, node, manager, false);
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
            // 基于 clp_s 策略的数组处理
            if (manager.getStructurizeArrays()) {
                // 模式1：结构化数组处理（保持数组结构）
                parseStructuredArray(node, prefix, depth, all_fields, value_counts, manager);
            } else {
                // 模式2：非结构化数组处理（序列化为字符串）
                FieldType type = inferFieldType(prefix, node, manager);
                std::string array_str = simdjson::to_string(node);
                FieldKey key{prefix, type};
                all_fields.insert(key);
                value_counts[key]++;
                manager.addFieldValue(key, type, array_str);
            }
            break;
        }
        default: {
            FieldType type = inferFieldType(prefix, node, manager, false);
            
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
                case FieldType::STRING:
                case FieldType::TIMESTAMP:
                case FieldType::LOGTYPE:
                    if (std::holds_alternative<std::string>(value)) {
                        manager.addFieldValue(key, type, std::get<std::string>(value));
                    } else {
                        manager.addFieldValue(key, type, "");
                    }
                    break;
                case FieldType::INT64:
                    if (std::holds_alternative<int64_t>(value)) {
                        manager.addFieldValue(key, type, std::get<int64_t>(value));
                    } else {
                        manager.addFieldValue(key, type, int64_t(0));
                    }
                    break;
                case FieldType::DOUBLE:
                    if (std::holds_alternative<double>(value)) {
                        manager.addFieldValue(key, type, std::get<double>(value));
                    } else {
                        manager.addFieldValue(key, type, 0.0);
                    }
                    break;
                case FieldType::BOOL:
                    if (std::holds_alternative<bool>(value)) {
                        manager.addFieldValue(key, type, std::get<bool>(value));
                    } else {
                        manager.addFieldValue(key, type, false);
                    }
                    break;
                case FieldType::NULL_TYPE:
                    manager.addFieldValue(key, type, nullptr);
                    break;
                case FieldType::ARRAY:
                    if (std::holds_alternative<std::string>(value)) {
                        manager.addFieldValue(key, type, std::get<std::string>(value));
                    } else {
                        manager.addFieldValue(key, type, "");
                    }
                    break;
                default:
                    manager.addFieldValue(key, type, "");
                    break;
            }
            break;
        }
    }
}

// 实现结构化数组处理函数（基于 clp_s 策略）
void FieldParser::parseStructuredArray(simdjson::dom::element array_node,
                                      const std::string& prefix,
                                      int depth,
                                      std::set<FieldKey>& all_fields,
                                      std::unordered_map<FieldKey, size_t>& value_counts,
                                      FieldDictionaryManager& manager) {
    auto array = array_node.get_array().value();
    size_t index = 0;
    
    for (simdjson::dom::element element : array) {
        std::string element_name = prefix + "[" + std::to_string(index) + "]";
        
        switch (element.type()) {
            case simdjson::dom::element_type::OBJECT:
                // 处理对象数组元素
                collectAllFields(element, element_name, depth + 1, all_fields, value_counts, manager);
                break;
                
            case simdjson::dom::element_type::ARRAY:
                // 处理嵌套数组
                parseStructuredArray(element, element_name, depth + 1, all_fields, value_counts, manager);
                break;
                
            case simdjson::dom::element_type::STRING:
            case simdjson::dom::element_type::INT64:
            case simdjson::dom::element_type::UINT64:
            case simdjson::dom::element_type::DOUBLE:
            case simdjson::dom::element_type::BOOL:
            case simdjson::dom::element_type::NULL_VALUE:
                // 处理简单类型数组元素 - 直接根据 simdjson 类型确定 FieldType
                FieldType type;
                switch (element.type()) {
                    case simdjson::dom::element_type::STRING:
                        type = FieldType::STRING;
                        break;
                    case simdjson::dom::element_type::INT64:
                    case simdjson::dom::element_type::UINT64:
                        type = FieldType::INT64;
                        break;
                    case simdjson::dom::element_type::DOUBLE:
                        type = FieldType::DOUBLE;
                        break;
                    case simdjson::dom::element_type::BOOL:
                        type = FieldType::BOOL;
                        break;
                    case simdjson::dom::element_type::NULL_VALUE:
                        type = FieldType::NULL_TYPE;
                        break;
                    default:
                        type = FieldType::STRING;
                        break;
                }
                Value value = extractValue(element);
                
                // 检查是否为嵌套字段（通过递归访问产生的字段）
                std::string field_name = element_name;
                // 只有通过递归访问产生的字段才是嵌套字段，才添加 ~ 前缀
                if (!element_name.empty() && depth > 1) {
                    field_name = "~" + element_name;  // 添加 ~ 前缀标识嵌套字段
                }
                
                FieldKey key{field_name, type};
                all_fields.insert(key);
                value_counts[key]++;
                manager.addFieldValue(key, type, value);
                break;
        }
        
        ++index;
    }
}

void FieldParser::extractAllFieldValues(simdjson::dom::element node,
                                      const std::string& prefix,
                                      std::unordered_map<FieldKey, std::string>& field_values) {
    if (node.type() == simdjson::dom::element_type::OBJECT) {
        for (auto [key, value] : simdjson::dom::object(node)) {
            std::string field_name = prefix.empty() ? std::string(key) : prefix + "." + std::string(key);
            
            if (value.type() == simdjson::dom::element_type::OBJECT) {
                // 递归处理嵌套对象
                extractAllFieldValues(value, field_name, field_values);
            } else if (value.type() == simdjson::dom::element_type::ARRAY) {
                // 处理数组 - 只提取第一个元素作为代表值
                auto array = simdjson::dom::array(value);
                auto it = array.begin();
                if (it != array.end()) {
                    extractAllFieldValues(*it, field_name, field_values);
                }
            } else {
                // 处理简单类型
                std::string value_str;
                switch (value.type()) {
                    case simdjson::dom::element_type::STRING: {
                        auto str_res = value.get_string();
                        if (!str_res.error()) {
                            value_str = std::string(str_res.value());
                        }
                        break;
                    }
                    case simdjson::dom::element_type::INT64: {
                        auto int_res = value.get_int64();
                        if (!int_res.error()) {
                            value_str = std::to_string(int_res.value());
                        }
                        break;
                    }
                    case simdjson::dom::element_type::UINT64: {
                        auto uint_res = value.get_uint64();
                        if (!uint_res.error()) {
                            value_str = std::to_string(uint_res.value());
                        }
                        break;
                    }
                    case simdjson::dom::element_type::DOUBLE: {
                        auto double_res = value.get_double();
                        if (!double_res.error()) {
                            value_str = std::to_string(double_res.value());
                        }
                        break;
                    }
                    case simdjson::dom::element_type::BOOL: {
                        auto bool_res = value.get_bool();
                        if (!bool_res.error()) {
                            value_str = bool_res.value() ? "true" : "false";
                        }
                        break;
                    }
                    case simdjson::dom::element_type::NULL_VALUE:
                        value_str = "null";
                        break;
                    default:
                        value_str = "";
                        break;
                }
                
                // 推断字段类型
                FieldType type = FieldType::STRING; // 默认类型
                if (value.type() == simdjson::dom::element_type::INT64 || 
                    value.type() == simdjson::dom::element_type::UINT64) {
                    type = FieldType::INT64;
                } else if (value.type() == simdjson::dom::element_type::DOUBLE) {
                    type = FieldType::DOUBLE;
                } else if (value.type() == simdjson::dom::element_type::BOOL) {
                    type = FieldType::BOOL;
                } else if (value.type() == simdjson::dom::element_type::NULL_VALUE) {
                    type = FieldType::NULL_TYPE;
                } else if (value.type() == simdjson::dom::element_type::STRING) {
                    // 对于字符串，需要进一步判断是否为时间戳或日志模板
                    // 这里简化处理，直接设为String类型
                    type = FieldType::STRING;
                }
                
                FieldKey key{field_name, type};
                field_values[key] = value_str;
            }
        }
    }
}

} // namespace json2
