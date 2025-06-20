#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace json2 {

struct FieldDict {
    std::unordered_map<std::string, uint32_t> value_to_code;
    std::vector<std::string> code_to_value; // code-1 为下标
    uint32_t next_code = 1;
};

class Dictionary {
private:
    std::unordered_map<std::string, FieldDict> field_dicts;

public:
    // 添加字段值，返回编码
    uint32_t addFieldValue(const std::string& field_name, const std::string& value);

    // 获取编码
    uint32_t getFieldValueCode(const std::string& field_name, const std::string& value) const;

    // 通过编码获取原始值
    std::string getFieldValueByCode(const std::string& field_name, uint32_t code) const;

    // 清空所有字典
    void clear();

    size_t getFieldValueCount(const std::string& field) const;
};

} // namespace json2