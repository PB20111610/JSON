#ifndef JSON2_DICTIONARY_H
#define JSON2_DICTIONARY_H

#include <string>
#include <unordered_map>
#include <vector>
#include <variant>
#include <cstdint>
#include <optional>
#include <ostream>
#include <iostream>
#include "field_key.h"

namespace json2 {
// 类型敏感的值
using Value = std::variant<int64_t, double, bool, std::string, std::nullptr_t>;
// 哈希支持
struct ValueHash {
    std::size_t operator()(const Value& v) const;
};

class Dictionary {
public:
    // 类型敏感添加字段值，返回编码
    uint32_t addFieldValue(const FieldKey& key, const Value& value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, const std::string& value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, int64_t value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, double value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, bool value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, std::nullptr_t value);
    // 类型敏感获取或添加字段值，返回编码
    uint32_t getOrAddFieldValue(const FieldKey& key, const Value& value);
    // 类型敏感通过原始值获取编码
    uint32_t getFieldValueCode(const FieldKey& key, const Value& value) const;
    // 类型敏感通过编码获取原始值
    std::optional<Value> getFieldValueByCode(const FieldKey& key, uint32_t code) const;
    // 从code_to_value映射加载字典
    void loadFromCodeToValue(const std::vector<FieldKey>& ordered_fields, const std::unordered_map<FieldKey, std::vector<std::string>>& field_code_to_value);
    // 清空所有字典
    void clear();
    // 获取某类型字段值数量
    size_t getFieldValueCount(const FieldKey& key) const;
    size_t getFieldValueCount(const std::string& field, FieldType type) const { return getFieldValueCount(FieldKey{field, static_cast<FieldType>(type)}); }

private:
    // 各类型分离的字典
    struct VariableDict {
        std::unordered_map<std::string, uint32_t> value_to_code;
        std::vector<std::string> code_to_value;
        uint32_t next_code = 1;
    };
    struct IntegerDict {
        std::unordered_map<int64_t, uint32_t> value_to_code;
        std::vector<int64_t> code_to_value;
        uint32_t next_code = 1;
    };
    struct FloatDict {
        std::unordered_map<double, uint32_t> value_to_code;
        std::vector<double> code_to_value;
        uint32_t next_code = 1;
    };
    struct BooleanDict {
        std::unordered_map<bool, uint32_t> value_to_code;
        std::vector<bool> code_to_value;
        uint32_t next_code = 1;
    };
    struct FieldDicts {
        VariableDict variable_dict;
        IntegerDict integer_dict;
        FloatDict float_dict;
        BooleanDict boolean_dict;
    };
    std::unordered_map<FieldKey, FieldDicts> field_dicts;
};

} // namespace json2

#endif // JSON2_DICTIONARY_H
