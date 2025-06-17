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
        case DictType::RAW_NUMBER:
        case DictType::RAW_BOOLEAN:
            // 对于原始数值和布尔值，直接返回原始值，不进行编码
            if (type == DictType::RAW_BOOLEAN) {
                // 布尔值：true = 1, false = 0
                return (str == "true") ? 1 : 0;
            } else {
                // 数值：尝试转换为数值，保持原始精度
                try {
                    double num = std::stod(str);
                    // 对于浮点数，我们需要一个更好的编码方案
                    // 这里我们使用一个简单的方案：将浮点数乘以1000后转为整数
                    // 这样可以保持3位小数的精度
                    return static_cast<uint32_t>(num * 1000);
                } catch (const std::exception&) {
                    return 0;
                }
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
        case DictType::RAW_NUMBER:
        case DictType::RAW_BOOLEAN:
            // 对于原始数值和布尔值，直接返回原始值，不进行编码
            if (type == DictType::RAW_BOOLEAN) {
                // 布尔值：true = 1, false = 0
                return (str == "true") ? 1 : 0;
            } else {
                // 数值：尝试转换为数值，保持原始精度
                try {
                    double num = std::stod(str);
                    // 对于浮点数，我们需要一个更好的编码方案
                    // 这里我们使用一个简单的方案：将浮点数乘以1000后转为整数
                    // 这样可以保持3位小数的精度
                    return static_cast<uint32_t>(num * 1000);
                } catch (const std::exception&) {
                    return 0;
                }
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
        case DictType::RAW_NUMBER:
            // 对于数值，将编码转换回原始字符串
            if (code >= 0) {  // 修改：允许编码为0的数值
                static std::string num_str;
                // 将编码除以1000得到原始浮点数
                double num = static_cast<double>(code) / 1000.0;
                num_str = std::to_string(num);
                // 移除末尾的0（如1.000变成1）
                while (num_str.back() == '0' && num_str.find('.') != std::string::npos) {
                    num_str.pop_back();
                }
                if (num_str.back() == '.') {
                    num_str.pop_back();
                }
                return num_str;
            }
            return empty;
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
        case DictType::RAW_NUMBER:
        case DictType::RAW_BOOLEAN:
            // 对于原始数值和布尔值，返回0，因为不需要存储
            return 0;
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
        case DictType::RAW_NUMBER:
        case DictType::RAW_BOOLEAN:
            // 对于原始数值和布尔值，返回空向量，因为不需要存储
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
    
    // 对于原始数值和布尔值，仍然需要统计不同值的数量
    if (type == DictType::RAW_BOOLEAN) {
        // 布尔值只有两种可能的值
        if (it->value_codes.empty()) {
            it->value_codes[value] = 1;
            it->value_count = 1;
        } else if (it->value_codes.size() == 1 && it->value_codes.find(value) == it->value_codes.end()) {
            it->value_codes[value] = 2;
            it->value_count = 2;
        }
    } else if (type == DictType::RAW_NUMBER) {
        // 对于数值，统计不同值的数量
        if (it->value_codes.find(value) == it->value_codes.end()) {
            it->value_codes[value] = it->value_count + 1;
            it->value_count++;
        }
    } else {
        // 对于其他类型，使用字典编码
        if (it->value_codes.find(value) == it->value_codes.end()) {
            it->value_codes[value] = add(value, type);
            it->value_count++;
        }
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