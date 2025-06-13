#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace json2 {

enum class DictType {
    TIMESTAMP_DICT,
    LOG_DICT,
    VARIABLE_DICT
};

// 字段值域统计
struct FieldStats {
    std::string name;
    DictType type;
    size_t value_count;
    std::unordered_map<std::string, uint32_t> value_codes;
};

class Dictionary {
private:
    // 按类型存储的映射
    std::unordered_map<std::string, uint32_t> timestamp_dict;
    std::unordered_map<std::string, uint32_t> log_dict;
    std::unordered_map<std::string, uint32_t> variable_dict;
    
    // 按类型存储的编码到字符串的映射
    std::vector<std::string> timestamp_codes;
    std::vector<std::string> log_codes;
    std::vector<std::string> variable_codes;
    
    // 每种类型的下一个编码
    uint32_t next_timestamp_code = 1;
    uint32_t next_log_code = 1;
    uint32_t next_variable_code = 1;

    // 字段级别的值域统计
    std::vector<FieldStats> field_stats;

public:
    // 添加字符串并返回编码
    uint32_t add(const std::string& str, DictType type);
    
    // 获取字符串编码（0表示未找到）
    uint32_t getCode(const std::string& str, DictType type) const;
    
    // 根据编码获取原始字符串（空字符串表示未找到）
    const std::string& getString(uint32_t code, DictType type) const;
    
    // 获取指定类型字典的大小
    size_t size(DictType type) const;
    
    // 清空所有字典
    void clear();
    
    // 获取指定类型的所有编码
    const std::vector<std::string>& getCodes(DictType type) const;

    // 添加字段值域统计
    void addFieldValue(const std::string& field_name, const std::string& value, DictType type);
    
    // 获取字段值域统计
    const std::vector<FieldStats>& getFieldStats() const { return field_stats; }
    
    // 根据值域大小排序字段（值域小的在上层）
    std::vector<std::string> getOrderedFields() const;
};

} // namespace json2