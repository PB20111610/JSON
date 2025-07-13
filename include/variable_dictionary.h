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

namespace json2 {
// 类型枚举
enum class FieldType {
    Int,
    Double,
    Bool,
    String,
    Timestamp,
    LogType,
    Null
};
// 类型敏感的值
using Value = std::variant<int64_t, double, bool, std::string, std::nullptr_t>;
// 哈希支持
struct ValueHash {
    std::size_t operator()(const Value& v) const;
};

class Dictionary {
public:
    // 类型敏感添加字段值，返回编码
    uint32_t addFieldValue(const std::string& field, FieldType type, const Value& value);
    // 类型敏感获取或添加字段值，返回编码
    uint32_t getOrAddFieldValue(const std::string& field, FieldType type, const Value& value);
    // 类型敏感通过原始值获取编码
    uint32_t getFieldValueCode(const std::string& field, FieldType type, const Value& value) const;
    // 类型敏感通过编码获取原始值
    std::optional<Value> getFieldValueByCode(const std::string& field, FieldType type, uint32_t code) const;
    // 从code_to_value映射加载字典
    void loadFromCodeToValue(const std::vector<std::string>& ordered_fields, const std::unordered_map<std::string, std::vector<std::string>>& field_code_to_value);
    // 清空所有字典
    void clear();
    // 获取某类型字段值数量
    size_t getFieldValueCount(const std::string& field, FieldType type) const;

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
    std::unordered_map<std::string, FieldDicts> field_dicts;
};

} // namespace json2

#endif // JSON2_DICTIONARY_H
