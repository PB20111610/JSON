#include "../include/dictionary.h"
#include <algorithm>
#include <unordered_map>

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
    }
    return 0;
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
    }
    return 0;
}

void Dictionary::clear() {
    timestamp_dict.clear();
    log_dict.clear();
    variable_dict.clear();
    timestamp_codes.clear();
    log_codes.clear();
    variable_codes.clear();
    next_timestamp_code = 1;
    next_log_code = 1;
    next_variable_code = 1;
}

const std::vector<std::string>& Dictionary::getCodes(DictType type) const {
    switch (type) {
        case DictType::TIMESTAMP_DICT:
            return timestamp_codes;
        case DictType::LOG_DICT:
            return log_codes;
        case DictType::VARIABLE_DICT:
            return variable_codes;
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
    
    // 如果值不在字段的值域中，添加它
    if (it->value_codes.find(value) == it->value_codes.end()) {
        it->value_codes[value] = add(value, type);
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

} // namespace json2