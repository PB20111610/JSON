#include "../include/dictionary.h"
#include <algorithm>
#include <unordered_map>
#include <iostream>

namespace json2 {

uint32_t Dictionary::add(const std::string& str, DictType type) {
    switch (type) {
        case DictType::TIMESTAMP_DICT: {
            auto it = timestamp_dict.find(str);
            if (it != timestamp_dict.end()) {
                return it->second;
            }
            timestamp_dict[str] = next_timestamp_code;
            timestamp_codes.push_back(str);
            return next_timestamp_code++;
        }
        case DictType::LOG_DICT: {
            auto it = log_dict.find(str);
            if (it != log_dict.end()) {
                return it->second;
            }
            log_dict[str] = next_log_code;
            log_codes.push_back(str);
            return next_log_code++;
        }
        case DictType::VARIABLE_DICT: {
            auto it = variable_dict.find(str);
            if (it != variable_dict.end()) {
                return it->second;
            }
            variable_dict[str] = next_variable_code;
            variable_codes.push_back(str);
            return next_variable_code++;
        }
        case DictType::RAW_BOOLEAN:
            // 布尔值：true = 1, false = 0
            return (str == "true") ? 1 : 0;
    }
    return 0;
}

uint32_t Dictionary::addInteger(int64_t value) {
    auto it = integer_dict.find(value);
    if (it != integer_dict.end()) {
        return it->second;
    }
    integer_dict[value] = next_integer_code;
    integer_codes.push_back(value);
    return next_integer_code++;
}

uint32_t Dictionary::addFloat(double value) {
    auto it = float_dict.find(value);
    if (it != float_dict.end()) {
        return it->second;
    }
    float_dict[value] = next_float_code;
    float_codes.push_back(value);
    return next_float_code++;
}

uint32_t Dictionary::getCode(const std::string& str, DictType type) const {
    switch (type) {
        case DictType::TIMESTAMP_DICT: {
            auto it = timestamp_dict.find(str);
            return (it != timestamp_dict.end()) ? it->second : 0;
        }
        case DictType::LOG_DICT: {
            auto it = log_dict.find(str);
            return (it != log_dict.end()) ? it->second : 0;
        }
        case DictType::VARIABLE_DICT: {
            auto it = variable_dict.find(str);
            return (it != variable_dict.end()) ? it->second : 0;
        }
        case DictType::RAW_BOOLEAN:
            // 布尔值：true = 1, false = 0
            return (str == "true") ? 1 : 0;
    }
    return 0;
}

const std::string& Dictionary::getString(uint32_t code, DictType type) const {
    static const std::string empty;
    switch (type) {
        case DictType::TIMESTAMP_DICT:
            return (code > 0 && code <= timestamp_codes.size()) ? 
                timestamp_codes[code-1] : empty;
        case DictType::LOG_DICT:
            return (code > 0 && code <= log_codes.size()) ? 
                log_codes[code-1] : empty;
        case DictType::VARIABLE_DICT:
            return (code > 0 && code <= variable_codes.size()) ? 
                variable_codes[code-1] : empty;
        case DictType::RAW_BOOLEAN:
            // 对于布尔值，将编码转换回字符串
            if (code == 1) {
                static const std::string true_str = "true";
                return true_str;
            } else if (code == 0) {
                static const std::string false_str = "false";
                return false_str;
            }
            return empty;
    }
    return empty;
}

size_t Dictionary::size(DictType type) const {
    switch (type) {
        case DictType::TIMESTAMP_DICT:
            return timestamp_codes.size();
        case DictType::LOG_DICT:
            return log_codes.size();
        case DictType::VARIABLE_DICT:
            return variable_codes.size();
        case DictType::INTEGER_DICT:
            return integer_codes.size();
        case DictType::FLOAT_DICT:
            return float_codes.size();
        case DictType::RAW_BOOLEAN:
            // 布尔值只有两种可能的值
            return 2;
    }
    return 0;
}

void Dictionary::clear() {
    timestamp_dict.clear();
    log_dict.clear();
    variable_dict.clear();
    integer_dict.clear();
    float_dict.clear();
    timestamp_codes.clear();
    log_codes.clear();
    variable_codes.clear();
    integer_codes.clear();
    float_codes.clear();
    next_timestamp_code = 1;
    next_log_code = 1;
    next_variable_code = 1;
    next_integer_code = 1;
    next_float_code = 1;
}

const std::vector<std::string>& Dictionary::getCodes(DictType type) const {
    switch (type) {
        case DictType::TIMESTAMP_DICT:
            return timestamp_codes;
        case DictType::LOG_DICT:
            return log_codes;
        case DictType::VARIABLE_DICT:
            return variable_codes;
        case DictType::RAW_BOOLEAN:
            // 布尔值返回空向量，因为不需要存储
            static const std::vector<std::string> empty;
            return empty;
    }
    static const std::vector<std::string> empty;
    return empty;
}

void Dictionary::addFieldValue(const std::string& field_name, const std::string& value, DictType type) {
    // 查找或创建字段统计
    auto it = std::find_if(field_stats.begin(), field_stats.end(),
        [&field_name](const FieldStats& stats) { return stats.name == field_name; });
    
    if (it == field_stats.end()) {
        // 新字段，创建统计信息
        FieldStats stats;
        stats.name = field_name;
        stats.type = type;  // 记录字段使用的字典类型
        stats.value_count = 0;
        stats.occurrence_count = 0;
        field_stats.push_back(stats);
        it = field_stats.end() - 1;
    } else {
        // 检查字段类型是否匹配
        if (it->type != type) {
            throw std::runtime_error("Field type mismatch for field: " + field_name);
        }
    }
    
    // 增加出现次数
    it->occurrence_count++;
    
    // 对于所有类型，都存储原始的字符串值以保持精度
    if (it->value_codes.find(value) == it->value_codes.end()) {
        if (type == DictType::RAW_BOOLEAN) {
            // 布尔值：true = 1, false = 0
            it->value_codes[value] = (value == "true") ? 1 : 0;
        } else if (type == DictType::INTEGER_DICT || type == DictType::FLOAT_DICT) {
            // 对于数值类型，生成一个编码但不进行数值转换
            uint32_t code = it->value_count + 1;
            it->value_codes[value] = code;
            std::cout << "Adding field: " << field_name << ", value: " << value << ", code: " << code << std::endl;
        } else {
            // 对于其他类型，使用字典编码
            it->value_codes[value] = add(value, type);
        }
        it->value_count++;
    }
}

std::vector<std::string> Dictionary::getOrderedFields() const {
    // 计算总记录数（使用最大出现次数作为估计）
    size_t total_records = 0;
    for (const auto& stats : field_stats) {
        total_records = std::max(total_records, stats.occurrence_count);
    }
    
    // 创建字段统计信息的索引映射
    std::unordered_map<std::string, const FieldStats*> field_index;
    for (const auto& stats : field_stats) {
        field_index[stats.name] = &stats;
    }
    
    // 创建字段名和冗余度因子的对
    std::vector<std::pair<std::string, double>> field_redundancy;
    for (const auto& stats : field_stats) {
        // 计算冗余度因子：出现次数 / (值域大小 × 总记录数)
        // 添加小量值(1e-6)防止除以0
        double redundancy_factor = static_cast<double>(stats.occurrence_count) / 
                                  ((stats.value_count + 1e-6) * total_records);
        field_redundancy.emplace_back(stats.name, redundancy_factor);
    }
    
    // 按冗余度因子从高到低排序
    std::sort(field_redundancy.begin(), field_redundancy.end(),
        [&field_index](const auto& a, const auto& b) { 
            // 首先按冗余度因子排序
            if (std::abs(a.second - b.second) > 1e-6) {
                return a.second > b.second;
            }
            // 冗余度因子相同时，按值域大小排序（值域小的优先）
            auto it_a = field_index.find(a.first);
            auto it_b = field_index.find(b.first);
            return it_a->second->value_count < it_b->second->value_count;
        });
    
    // 提取排序后的字段名
    std::vector<std::string> ordered_fields;
    ordered_fields.reserve(field_redundancy.size());
    for (const auto& pair : field_redundancy) {
        ordered_fields.push_back(pair.first);
    }
    
    return ordered_fields;
}

uint32_t Dictionary::getIntegerCode(int64_t value) const {
    auto it = integer_dict.find(value);
    return (it != integer_dict.end()) ? it->second : 0;
}

uint32_t Dictionary::getFloatCode(double value) const {
    auto it = float_dict.find(value);
    return (it != float_dict.end()) ? it->second : 0;
}

int64_t Dictionary::getInteger(uint32_t code) const {
    return (code > 0 && code <= integer_codes.size()) ? 
        integer_codes[code-1] : 0;
}

double Dictionary::getFloat(uint32_t code) const {
    return (code > 0 && code <= float_codes.size()) ? 
        float_codes[code-1] : 0.0;
}

void Dictionary::addIntegerFieldValue(const std::string& field_name, int64_t value) {
    // 查找或创建字段统计
    auto it = std::find_if(field_stats.begin(), field_stats.end(),
        [&field_name](const FieldStats& stats) { return stats.name == field_name; });
    
    if (it == field_stats.end()) {
        // 新字段，创建统计信息
        FieldStats stats;
        stats.name = field_name;
        stats.type = DictType::INTEGER_DICT;
        stats.value_count = 0;
        stats.occurrence_count = 0;
        field_stats.push_back(stats);
        it = field_stats.end() - 1;
    } else {
        // 检查字段类型是否匹配
        if (it->type != DictType::INTEGER_DICT) {
            throw std::runtime_error("Field type mismatch for field: " + field_name);
        }
    }
    
    // 增加出现次数
    it->occurrence_count++;
    
    // 统计不同值的数量并添加到字典
    std::string value_str = std::to_string(value);
    if (it->value_codes.find(value_str) == it->value_codes.end()) {
        it->value_codes[value_str] = addInteger(value);
        it->value_count++;
    }
}

void Dictionary::addFloatFieldValue(const std::string& field_name, double value) {
    // 查找或创建字段统计
    auto it = std::find_if(field_stats.begin(), field_stats.end(),
        [&field_name](const FieldStats& stats) { return stats.name == field_name; });
    
    if (it == field_stats.end()) {
        // 新字段，创建统计信息
        FieldStats stats;
        stats.name = field_name;
        stats.type = DictType::FLOAT_DICT;
        stats.value_count = 0;
        stats.occurrence_count = 0;
        field_stats.push_back(stats);
        it = field_stats.end() - 1;
    } else {
        // 检查字段类型是否匹配
        if (it->type != DictType::FLOAT_DICT) {
            throw std::runtime_error("Field type mismatch for field: " + field_name);
        }
    }
    
    // 增加出现次数
    it->occurrence_count++;
    
    // 统计不同值的数量并添加到字典
    std::string value_str = std::to_string(value);
    if (it->value_codes.find(value_str) == it->value_codes.end()) {
        it->value_codes[value_str] = addFloat(value);
        it->value_count++;
    }
}

std::string Dictionary::getOriginalString(const std::string& field_name, uint32_t code) const {
    // 查找字段统计信息
    auto it = std::find_if(field_stats.begin(), field_stats.end(),
        [&field_name](const FieldStats& stats) { return stats.name == field_name; });
    
    if (it == field_stats.end()) {
        return "";
    }
    
    // 从value_codes中查找对应的原始字符串值
    for (const auto& [value_str, value_code] : it->value_codes) {
        if (value_code == code) {
            return value_str;
        }
    }
    
    return "";
}

uint32_t Dictionary::getFieldValueCode(const std::string& field_name, const std::string& value) const {
    // 查找字段统计信息
    auto it = std::find_if(field_stats.begin(), field_stats.end(),
        [&field_name](const FieldStats& stats) { return stats.name == field_name; });
    
    if (it == field_stats.end()) {
        std::cout << "Field not found: " << field_name << std::endl;
        return 0;
    }
    
    // 从value_codes中查找对应的编码
    auto code_it = it->value_codes.find(value);
    uint32_t code = (code_it != it->value_codes.end()) ? code_it->second : 0;
    std::cout << "Looking up field: " << field_name << ", value: " << value << ", found code: " << code << std::endl;
    return code;
}

} // namespace json2