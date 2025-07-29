#include "../include/variable_dictionary.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>
#include <variant>
#include <iostream>
#include <algorithm>

namespace json2 {

// variant哈希与相等比较
namespace {
struct ValueHash {
    std::size_t operator()(const Value& v) const {
        return std::visit([](auto&& arg) { return std::hash<std::decay_t<decltype(arg)>>{}(arg); }, v);
    }
};
struct ValueEqual {
    bool operator()(const Value& a, const Value& b) const {
        return a == b;
    }
};
}

// -------------- 底层类型分发实现 --------------
uint32_t Dictionary::addFieldValue(const FieldKey& key, FieldType type, const std::string& value) {
    auto& dict = global_variable_dict; // 全局字符串字典，所有String类型数据共享
    auto it = dict.value_to_code.find(value);
    if (it != dict.value_to_code.end()) {
        // std::cout << "[DEBUG][Dictionary::addFieldValue] String already exists: field=" << key.name << ", code=" << it->second << ", value='" << value << "'" << std::endl;
        return it->second;
    }
    uint32_t code = dict.next_code++;
    dict.value_to_code[value] = code;
    if (dict.code_to_value.size() < code) {
        dict.code_to_value.resize(code);
    }
    dict.code_to_value[code - 1] = value;
    // std::cout << "[DEBUG][Dictionary::addFieldValue] String new: field=" << key.name << ", code=" << code << ", value='" << value << "'" << std::endl;
    return code;
}

uint32_t Dictionary::addFieldValue(const FieldKey& key, FieldType type, int64_t value) {
    auto& dict = field_dicts[key].integer_dict;
    auto it = dict.value_to_code.find(value);
    if (it != dict.value_to_code.end()) {
        return it->second;
    }
    uint32_t code = dict.next_code++;
    dict.value_to_code[value] = code;
    if (dict.code_to_value.size() < code) {
        dict.code_to_value.resize(code);
    }
    dict.code_to_value[code - 1] = value;
    return code;
}

uint32_t Dictionary::addFieldValue(const FieldKey& key, FieldType type, double value) {
    auto& dict = field_dicts[key].float_dict;
    auto it = dict.value_to_code.find(value);
    if (it != dict.value_to_code.end()) {
        return it->second;
    }
    uint32_t code = dict.next_code++;
    dict.value_to_code[value] = code;
    if (dict.code_to_value.size() < code) {
        dict.code_to_value.resize(code);
    }
    dict.code_to_value[code - 1] = value;
    return code;
}

uint32_t Dictionary::addFieldValue(const FieldKey& key, FieldType type, bool value) {
    auto& dict = field_dicts[key].boolean_dict;
    auto it = dict.value_to_code.find(value);
    if (it != dict.value_to_code.end()) {
        return it->second;
    }
    uint32_t code = dict.next_code++;
    dict.value_to_code[value] = code;
    if (dict.code_to_value.size() < code) {
        dict.code_to_value.resize(code);
    }
    dict.code_to_value[code - 1] = value;
    return code;
}

uint32_t Dictionary::addFieldValue(const FieldKey& key, FieldType type, std::nullptr_t) {
    auto& dict = global_variable_dict; // 全局字符串字典
    std::string null_str = "<null>";
    auto it = dict.value_to_code.find(null_str);
    if (it != dict.value_to_code.end()) {
        return it->second;
    }
    uint32_t code = dict.next_code++;
    dict.value_to_code[null_str] = code;
    if (dict.code_to_value.size() < code) {
        dict.code_to_value.resize(code);
    }
    dict.code_to_value[code - 1] = null_str;
    return code;
}

// 只保留 getFieldValueCode(const FieldKey&, const Value&) const 和 getFieldValueByCode(const FieldKey&, uint32_t) const 的实现，移除其它重载和老接口实现。
uint32_t Dictionary::getFieldValueCode(const FieldKey& key, const Value& value) const {
    switch (key.type) {
        case FieldType::String:
            return global_variable_dict.value_to_code.at(std::get<std::string>(value));
        case FieldType::Null:
            return global_variable_dict.value_to_code.at("<null>");
        case FieldType::Int:
            return field_dicts.at(key).integer_dict.value_to_code.at(std::get<int64_t>(value));
        case FieldType::Double:
            return field_dicts.at(key).float_dict.value_to_code.at(std::get<double>(value));
        case FieldType::Bool:
            return field_dicts.at(key).boolean_dict.value_to_code.at(std::get<bool>(value));
        default:
            throw std::invalid_argument("Unsupported type in getFieldValueCode");
    }
}

std::optional<Value> Dictionary::getFieldValueByCode(const FieldKey& key, uint32_t code) const {
    switch (key.type) {
        case FieldType::String:
        case FieldType::Null: {
            const auto& dict = global_variable_dict;
            if (code == 0 || code > dict.code_to_value.size()) return std::nullopt;
            return dict.code_to_value[code - 1];
        }
        case FieldType::Int: {
            const auto& dict = field_dicts.at(key).integer_dict;
            if (code == 0 || code > dict.code_to_value.size()) return std::nullopt;
            return dict.code_to_value[code - 1];
        }
        case FieldType::Double: {
            const auto& dict = field_dicts.at(key).float_dict;
            if (code == 0 || code > dict.code_to_value.size()) return std::nullopt;
            return dict.code_to_value[code - 1];
        }
        case FieldType::Bool: {
            const auto& dict = field_dicts.at(key).boolean_dict;
            if (code == 0 || code > dict.code_to_value.size()) return std::nullopt;
            return dict.code_to_value[code - 1];
        }
        default:
            return std::nullopt;
    }
}

uint32_t Dictionary::getOrAddFieldValue(const FieldKey& key, const Value& value) {
    switch (key.type) {
        case FieldType::String:
            return addFieldValue(key, key.type, std::get<std::string>(value));
        case FieldType::Null:
            return addFieldValue(key, key.type, nullptr);
        case FieldType::Int:
            return addFieldValue(key, key.type, std::get<int64_t>(value));
        case FieldType::Double:
            return addFieldValue(key, key.type, std::get<double>(value));
        case FieldType::Bool:
            return addFieldValue(key, key.type, std::get<bool>(value));
        case FieldType::Timestamp:
        case FieldType::LogType:
            throw std::invalid_argument("Dictionary::getOrAddFieldValue should not be used for Timestamp/LogType. Use timestampDict/logtypeDict directly.");
        default:
            throw std::invalid_argument("Unsupported type in getOrAddFieldValue");
    }
}

void Dictionary::clear() {
    field_dicts.clear();
    global_variable_dict = VariableDict(); // 清空全局字符串字典
}

size_t Dictionary::getFieldValueCount(const FieldKey& key) const {
    switch (key.type) {
        case FieldType::String:
        case FieldType::Null:
            return global_variable_dict.code_to_value.size();
        case FieldType::Int: {
            auto it = field_dicts.find(key);
            if (it == field_dicts.end()) return 0;
            return it->second.integer_dict.code_to_value.size();
        }
        case FieldType::Double: {
            auto it = field_dicts.find(key);
            if (it == field_dicts.end()) return 0;
            return it->second.float_dict.code_to_value.size();
        }
        case FieldType::Bool: {
            auto it = field_dicts.find(key);
            if (it == field_dicts.end()) return 0;
            return it->second.boolean_dict.code_to_value.size();
        }
        default:
            return 0;
    }
}

void Dictionary::loadFromCodeToValue(const std::vector<FieldKey>& ordered_fields, const std::unordered_map<FieldKey, std::vector<std::string>>& field_code_to_value) {
    // 兼容旧实现，但String类型统一使用全局字典
    for (const auto& key : ordered_fields) {
        auto it = field_code_to_value.find(key);
        if (it != field_code_to_value.end()) {
            if (key.type == FieldType::String || key.type == FieldType::Null) {
                // 合并所有字段的字符串到全局字典（不再按FieldKey分类）
                for (const auto& val : it->second) {
                    addFieldValue(key, key.type, val);
                }
            } else if (key.type == FieldType::Int) {
                auto& dict = field_dicts[key].integer_dict;
                dict.code_to_value.clear();
                dict.code_to_value.reserve(it->second.size());
                for (const auto& s : it->second) {
                    dict.code_to_value.push_back(std::stoll(s));
                }
                dict.next_code = static_cast<uint32_t>(it->second.size() + 1);
                for (size_t i = 0; i < it->second.size(); ++i) {
                    dict.value_to_code[std::stoll(it->second[i])] = static_cast<uint32_t>(i + 1);
                }
            } else if (key.type == FieldType::Double) {
                auto& dict = field_dicts[key].float_dict;
                dict.code_to_value.clear();
                dict.code_to_value.reserve(it->second.size());
                for (const auto& s : it->second) {
                    dict.code_to_value.push_back(std::stod(s));
                }
                dict.next_code = static_cast<uint32_t>(it->second.size() + 1);
                for (size_t i = 0; i < it->second.size(); ++i) {
                    dict.value_to_code[std::stod(it->second[i])] = static_cast<uint32_t>(i + 1);
                }
            } else if (key.type == FieldType::Bool) {
                auto& dict = field_dicts[key].boolean_dict;
                dict.code_to_value.clear();
                dict.next_code = 1;
                for (size_t i = 0; i < it->second.size(); ++i) {
                    bool b = (it->second[i] == "true" || it->second[i] == "1");
                    dict.value_to_code[b] = static_cast<uint32_t>(i + 1);
                    dict.code_to_value.push_back(b);
                    dict.next_code++;
                }
            }
        }
    }
}

std::vector<std::string> Dictionary::getAllStringValues() const {
    return global_variable_dict.code_to_value;
}

} // namespace json2