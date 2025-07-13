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

uint32_t Dictionary::addFieldValue(const std::string& field, FieldType type, const Value& value) {
    auto& dicts = field_dicts[field];
    switch (type) {
        case FieldType::Int: {
            int64_t int_value = std::get<int64_t>(value);
            auto& dict = dicts.integer_dict;
            auto it = dict.value_to_code.find(int_value);
            if (it != dict.value_to_code.end()) return it->second;
            uint32_t code = dict.next_code++;
            dict.value_to_code[int_value] = code;
            dict.code_to_value.push_back(int_value);
            return code;
        }
        case FieldType::Double: {
            double double_value = std::get<double>(value);
            auto& dict = dicts.float_dict;
            auto it = dict.value_to_code.find(double_value);
            if (it != dict.value_to_code.end()) return it->second;
            uint32_t code = dict.next_code++;
            dict.value_to_code[double_value] = code;
            dict.code_to_value.push_back(double_value);
            return code;
        }
        case FieldType::Bool: {
            bool bool_value = std::get<bool>(value);
            auto& dict = dicts.boolean_dict;
            auto it = dict.value_to_code.find(bool_value);
            if (it != dict.value_to_code.end()) return it->second;
            uint32_t code = dict.next_code++;
            dict.value_to_code[bool_value] = code;
            dict.code_to_value.push_back(bool_value);
            return code;
        }
        case FieldType::String: {
            const std::string& str = std::get<std::string>(value);
            auto it = dicts.variable_dict.value_to_code.find(str);
            if (it != dicts.variable_dict.value_to_code.end()) return it->second;
            uint32_t code = dicts.variable_dict.next_code++;
            dicts.variable_dict.value_to_code[str] = code;
            dicts.variable_dict.code_to_value.push_back(str);
            return code;
        }
        case FieldType::Null: {
            // Null 只分配一个特殊编码 1
            return 1;
        }
        default:
            return 0;
    }
}

uint32_t Dictionary::getFieldValueCode(const std::string& field, FieldType type, const Value& value) const {
    auto it = field_dicts.find(field);
    if (it == field_dicts.end()) return 0;
    const auto& dicts = it->second;
    switch (type) {
        case FieldType::Int: {
            int64_t int_value = std::get<int64_t>(value);
            auto it2 = dicts.integer_dict.value_to_code.find(int_value);
            return (it2 != dicts.integer_dict.value_to_code.end()) ? it2->second : 0;
        }
        case FieldType::Double: {
            double double_value = std::get<double>(value);
            auto it2 = dicts.float_dict.value_to_code.find(double_value);
            return (it2 != dicts.float_dict.value_to_code.end()) ? it2->second : 0;
        }
        case FieldType::Bool: {
            bool bool_value = std::get<bool>(value);
            auto it2 = dicts.boolean_dict.value_to_code.find(bool_value);
            return (it2 != dicts.boolean_dict.value_to_code.end()) ? it2->second : 0;
        }
        case FieldType::String: {
            const std::string& str = std::get<std::string>(value);
            auto it2 = dicts.variable_dict.value_to_code.find(str);
            return (it2 != dicts.variable_dict.value_to_code.end()) ? it2->second : 0;
        }
        case FieldType::Null: {
            return 1;
        }
        default:
            return 0;
    }
}

std::optional<Value> Dictionary::getFieldValueByCode(const std::string& field, FieldType type, uint32_t code) const {
    auto it = field_dicts.find(field);
    if (it == field_dicts.end()) return std::nullopt;
    const auto& dicts = it->second;
    switch (type) {
        case FieldType::Int: {
            if (code == 0 || code > dicts.integer_dict.code_to_value.size()) return std::nullopt;
            return dicts.integer_dict.code_to_value[code - 1];
        }
        case FieldType::Double: {
            if (code == 0 || code > dicts.float_dict.code_to_value.size()) return std::nullopt;
            return dicts.float_dict.code_to_value[code - 1];
        }
        case FieldType::Bool: {
            if (code == 0 || code > dicts.boolean_dict.code_to_value.size()) return std::nullopt;
            return dicts.boolean_dict.code_to_value[code - 1];
        }
        case FieldType::String: {
            if (code == 0 || code > dicts.variable_dict.code_to_value.size()) return std::nullopt;
            return dicts.variable_dict.code_to_value[code - 1];
        }
        case FieldType::Null: {
            return nullptr;
        }
        default:
            return std::nullopt;
    }
}

uint32_t Dictionary::getOrAddFieldValue(const std::string& field, FieldType type, const Value& value) {
    // 先查找是否已存在
    uint32_t code = getFieldValueCode(field, type, value);
    if (code != 0) return code;
    // 不存在则插入
    return addFieldValue(field, type, value);
}

void Dictionary::clear() {
    field_dicts.clear();
}

size_t Dictionary::getFieldValueCount(const std::string& field, FieldType type) const {
    auto it = field_dicts.find(field);
    if (it == field_dicts.end()) return 0;
    const auto& dicts = it->second;
    switch (type) {
        case FieldType::Int:
            return dicts.integer_dict.code_to_value.size();
        case FieldType::Double:
            return dicts.float_dict.code_to_value.size();
        case FieldType::Bool:
            return dicts.boolean_dict.code_to_value.size();
        case FieldType::String:
            return dicts.variable_dict.code_to_value.size();
        case FieldType::Null:
            return 1;
        default:
            return 0;
    }
}

void Dictionary::loadFromCodeToValue(const std::vector<std::string>& ordered_fields, const std::unordered_map<std::string, std::vector<std::string>>& field_code_to_value) {
    clear();
    for (const auto& field : ordered_fields) {
        auto it = field_code_to_value.find(field);
        if (it != field_code_to_value.end()) {
            auto& dict = field_dicts[field];
            dict.variable_dict.code_to_value = it->second;
            dict.variable_dict.next_code = static_cast<uint32_t>(it->second.size() + 1);
            for (size_t i = 0; i < it->second.size(); ++i) {
                dict.variable_dict.value_to_code[it->second[i]] = static_cast<uint32_t>(i + 1);
            }
        }
    }
}

} // namespace json2