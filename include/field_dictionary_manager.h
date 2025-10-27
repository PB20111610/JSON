#pragma once
#include "field_key.h"
#include "variable_dictionary.h"
#include "timestamp_dictionary.h"
#include "logtype_dictionary.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>
#include <set>
#include <optional>
#include <functional>

namespace json2 {

class FieldDictionaryManager {
public:
    // 插入字段值，返回编码
    uint32_t addFieldValue(const FieldKey& key, const std::string& value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, const std::string& value) { return addFieldValue(key, value); }
    uint32_t addFieldValue(const FieldKey& key, FieldType type, int64_t value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, double value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, bool value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, std::nullptr_t value);
    uint32_t addFieldValue(const FieldKey& key, FieldType type, const Value& value);

    // 获取唯一值数
    size_t getUniqueValueCount(const FieldKey& key) const;
    size_t getUniqueValueCount(const FieldKey& key, FieldType type) const { return getUniqueValueCount(key); }

    // 获取总出现次数
    size_t getTotalCount(const FieldKey& key) const;
    size_t getTotalCount(const FieldKey& key, FieldType type) const { return getTotalCount(key); }

    // 获取所有出现过的字段和类型
    std::vector<FieldKey> getAllFieldsAndTypes() const;

    // 获取所有出现过的字段
    std::set<std::string> getAllFields() const;

    // 获取字段的唯一值集合
    const std::set<std::string>& getUniqueValues(const FieldKey& key) const;

    // 信息熵基础的冗余度计算方法（当前使用的方法）
    double calculateRedundancy(const FieldKey& key, size_t total, size_t unique) const; // 基于信息熵
    
    // 清空所有字典和计数
    void clear();

    // 设置时间戳字段（仿照 clp_s 的 authoritative_timestamp）
    void setTimestampFields(const std::vector<std::string>& fields);

    // 类型检测辅助方法
    bool isTimestampField(const std::string& field) const;
    bool isTimestampValue(const std::string& value) const;
    bool isLogTemplate(const std::string& value) const;

    Dictionary& variableDict() { return variable_dict_; }
    const Dictionary& variableDict() const { return variable_dict_; }
    TimestampDictionary& timestampDict() { return timestamp_dict_; }
    const TimestampDictionary& timestampDict() const { return timestamp_dict_; }
    LogTypeDictionary& logtypeDict() { return logtype_dict_; }
    const LogTypeDictionary& logtypeDict() const { return logtype_dict_; }

    std::optional<Value> getFieldValueByCode(const FieldKey& key, uint32_t code) const;

    // 新增：获取时间戳的完整编码信息
    std::optional<TemplateEncodedTimestamp> getTimestampEncoding(const FieldKey& key, uint32_t code) const;

    // 检查是否为嵌套字段（通过 ~ 前缀标识）
    bool isNestedField(const std::string& field_name) const;

    // 数组处理配置
    void setStructurizeArrays(bool structurize) { structurize_arrays_ = structurize; }
    bool getStructurizeArrays() const { return structurize_arrays_; }

private:
    bool structurize_arrays_ = false;  // 默认使用非结构化数组处理
    Dictionary variable_dict_;
    TimestampDictionary timestamp_dict_;
    LogTypeDictionary logtype_dict_;
    // 统计每个字段每种类型的出现次数
    std::unordered_map<FieldKey, size_t> field_type_total_count_;
    // 记录所有出现过的字段和类型
    std::vector<FieldKey> all_fields_and_types_;
    // 辅助去重
    std::unordered_map<FieldKey, bool> field_type_seen_;
    // 统计每个字段每种类型的唯一值
    std::unordered_map<FieldKey, std::set<std::string>> field_type_unique_values_;
      
};

} // namespace json2 