#pragma once
#include "variable_dictionary.h"
#include "timestamp_dictionary.h"
#include "logtype_dictionary.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <tuple>
#include <vector>
#include <set>

namespace json2 {

class FieldDictionaryManager {
public:
    // 插入字段值，返回编码
    uint32_t addFieldValue(const std::string& field, FieldType type, const std::string& value);

    // 获取唯一值数
    size_t getUniqueValueCount(const std::string& field, FieldType type) const;

    // 获取总出现次数
    size_t getTotalCount(const std::string& field, FieldType type) const;

    // 获取所有出现过的字段和类型
    std::vector<std::tuple<std::string, FieldType>> getAllFieldsAndTypes() const;

    // 获取所有出现过的字段
    std::set<std::string> getAllFields() const;

    // 统一输出冗余度统计（字段、类型、出现次数、唯一值数、冗余度）
    void printRedundancyStats(std::ostream& out = std::cout) const;

    // 清空所有字典和计数
    void clear();

    // 设置时间戳字段（仿照 clp_s 的 authoritative_timestamp）
    void setTimestampFields(const std::vector<std::string>& fields);

    // 类型检测辅助方法
    bool isTimestampField(const std::string& field) const;
    bool isTimestampValue(const std::string& value) const;
    bool isLogTemplate(const std::string& value) const;

    Dictionary& variableDict() { return variable_dict_; }
    TimestampDictionary& timestampDict() { return timestamp_dict_; }
    LogTypeDictionary& logtypeDict() { return logtype_dict_; }

private:
    Dictionary variable_dict_;
    TimestampDictionary timestamp_dict_;
    LogTypeDictionary logtype_dict_;
    // 统计每个字段每种类型的出现次数
    std::unordered_map<std::string, std::unordered_map<FieldType, size_t>> field_type_total_count_;
    // 记录所有出现过的字段和类型
    std::vector<std::tuple<std::string, FieldType>> all_fields_and_types_;
    // 辅助去重
    std::unordered_map<std::string, std::unordered_map<FieldType, bool>> field_type_seen_;
    // 统计每个字段每种类型的唯一值
    std::unordered_map<std::string, std::unordered_map<FieldType, std::set<std::string>>> field_type_unique_values_;
};

} // namespace json2 