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
    auto& dict = field_dicts[key].variable_dict;
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
    auto& dict = field_dicts[key].variable_dict;
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
            return field_dicts.at(key).variable_dict.value_to_code.at(std::get<std::string>(value));
        case FieldType::Null:
            return field_dicts.at(key).variable_dict.value_to_code.at("<null>");
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
            const auto& dict = field_dicts.at(key).variable_dict;
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
}

size_t Dictionary::getFieldValueCount(const FieldKey& key) const {
    return getFieldValueCount(key.name, key.type);
}

void Dictionary::loadFromCodeToValue(const std::vector<FieldKey>& ordered_fields, const std::unordered_map<FieldKey, std::vector<std::string>>& field_code_to_value) {
    // 兼容旧实现，按字段名+类型分发
    for (const auto& key : ordered_fields) {
        auto it = field_code_to_value.find(key);
        if (it != field_code_to_value.end()) {
            auto& dict = field_dicts[key];
            dict.variable_dict.code_to_value = it->second;
            dict.variable_dict.next_code = static_cast<uint32_t>(it->second.size() + 1);
            for (size_t i = 0; i < it->second.size(); ++i) {
                dict.variable_dict.value_to_code[it->second[i]] = static_cast<uint32_t>(i + 1);
            }
        }
    }
}

} // namespace json2