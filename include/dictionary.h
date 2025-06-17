#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace json2 {

enum class DictType {
    TIMESTAMP_DICT,
    LOG_DICT,
    VARIABLE_DICT,
    INTEGER_DICT,    // 整型字典
    FLOAT_DICT,      // 浮点型字典
    RAW_BOOLEAN    // 直接存储布尔值
};

// 字段值域统计
struct FieldStats {
    std::string name;
    DictType type;  // 字段使用的字典类型
    size_t value_count;
    size_t occurrence_count;  // 字段出现的次数
    std::unordered_map<std::string, uint32_t> value_codes;  // 存储字段的每个值对应的编码
};

class Dictionary {
private:
    // 按类型存储的映射
    std::unordered_map<std::string, uint32_t> timestamp_dict;
    std::unordered_map<std::string, uint32_t> log_dict;
    std::unordered_map<std::string, uint32_t> variable_dict;
    std::unordered_map<int64_t, uint32_t> integer_dict;     // 整型字典
    std::unordered_map<double, uint32_t> float_dict;        // 浮点型字典
    
    // 按类型存储的编码到值的映射
    std::vector<std::string> timestamp_codes;
    std::vector<std::string> log_codes;
    std::vector<std::string> variable_codes;
    std::vector<int64_t> integer_codes;     // 整型编码列表
    std::vector<double> float_codes;        // 浮点型编码列表
    
    // 每种类型的下一个编码
    uint32_t next_timestamp_code = 1;
    uint32_t next_log_code = 1;
    uint32_t next_variable_code = 1;
    uint32_t next_integer_code = 1;     // 整型编码计数器
    uint32_t next_float_code = 1;       // 浮点型编码计数器

    // 字段级别的值域统计
    std::vector<FieldStats> field_stats;

public:
    // 添加字符串并返回编码
    uint32_t add(const std::string& str, DictType type);
    
    // 添加整型值并返回编码
    uint32_t addInteger(int64_t value);
    
    // 添加浮点型值并返回编码
    uint32_t addFloat(double value);
    
    // 获取字符串编码（0表示未找到）
    uint32_t getCode(const std::string& str, DictType type) const;
    
    // 获取整型值编码（0表示未找到）
    uint32_t getIntegerCode(int64_t value) const;
    
    // 获取浮点型值编码（0表示未找到）
    uint32_t getFloatCode(double value) const;
    
    // 根据编码获取原始字符串（空字符串表示未找到）
    const std::string& getString(uint32_t code, DictType type) const;
    
    // 根据编码获取整型值
    int64_t getInteger(uint32_t code) const;
    
    // 根据编码获取浮点型值
    double getFloat(uint32_t code) const;
    
    // 根据字段名和编码获取原始字符串值（用于保持数值精度）
    std::string getOriginalString(const std::string& field_name, uint32_t code) const;
    
    // 根据字段名和值获取编码
    uint32_t getFieldValueCode(const std::string& field_name, const std::string& value) const;
    
    // 获取指定类型字典的大小
    size_t size(DictType type) const;
    
    // 清空所有字典
    void clear();
    
    // 获取指定类型的所有编码
    const std::vector<std::string>& getCodes(DictType type) const;
    
    // 获取整型字典的所有值
    const std::vector<int64_t>& getIntegerCodes() const { return integer_codes; }
    
    // 获取浮点型字典的所有值
    const std::vector<double>& getFloatCodes() const { return float_codes; }

    // 添加字段值域统计
    void addFieldValue(const std::string& field_name, const std::string& value, DictType type);
    
    // 添加整型字段值域统计
    void addIntegerFieldValue(const std::string& field_name, int64_t value);
    
    // 添加浮点型字段值域统计
    void addFloatFieldValue(const std::string& field_name, double value);
    
    // 获取字段值域统计
    const std::vector<FieldStats>& getFieldStats() const { return field_stats; }
    
    // 根据值域大小排序字段（值域小的在上层）
    std::vector<std::string> getOrderedFields() const;
};

} // namespace json2