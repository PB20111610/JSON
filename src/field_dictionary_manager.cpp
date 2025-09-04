#include "../include/field_dictionary_manager.h"
#include <stdexcept>
#include <set>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <iostream> // Added for debug output
#include <cmath> // 添加cmath头文件支持log2函数

namespace json2 {

// 简化的类型检测，仿照 clp_s 的方式
class SimpleTypeDetector {
private:
    std::vector<std::string> timestamp_fields_;
    std::vector<std::regex> timestamp_patterns_; // 预编译的正则表达式
    
public:
    SimpleTypeDetector() {
        // 预编译时间戳正则表达式
        std::vector<std::string> patterns = {
            R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \w+)",  // 2023-03-27 00:26:35.719 EDT
            R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \w+)",        // 2023-03-27 00:26:35 EDT
            R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})",            // 2023-03-27 00:26:35
            R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)",    // 2023-03-28T04:00:00.040Z (新增：支持毫秒)
            R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)",           // 2023-03-27T00:26:35Z
            R"(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2})",            // 2023/03/27 00:26:35
            R"(\\d{4}\\.\\d{2}\\.\\d{2})", // 2005.06.03
            R"(\\d{4}-\\d{2}-\\d{2}-\\d{2}\\.\\d{2}\\.\\d{2}\\.\\d{6})", // 2005-06-03-15.42.51.428563
            R"(\\d{4}-\\d{2}-\\d{2}-\\d{2}\\.\\d{2}\\.\\d{2}\\.\\d{3,6})", // 兼容3~6位毫秒/微秒
        };
        for (const auto& pattern : patterns) {
            timestamp_patterns_.emplace_back(pattern);
        }
    }
    
    void setTimestampFields(const std::vector<std::string>& fields) {
        timestamp_fields_ = fields;
    }
    
    // 检查字段是否为时间戳字段
    bool isTimestampField(const std::string& field) const {
        return std::find(timestamp_fields_.begin(), timestamp_fields_.end(), field) != timestamp_fields_.end();
    }
    
    // 检查字符串是否为时间戳格式（使用预编译的正则表达式）
    bool isTimestampValue(const std::string& value) const {
        if (value.empty()) return false;
        
        for (const auto& pattern : timestamp_patterns_) {
            if (std::regex_match(value, pattern)) {
                return true;
            }
        }
        return false;
    }
    
    // 检查字符串是否为日志模板（包含空格，但不是时间戳）
    bool isLogTemplate(const std::string& value) const {
        // 如果包含空格但不是时间戳，则认为是日志模板
        return value.find(' ') != std::string::npos && !isTimestampValue(value);
    }
};

static SimpleTypeDetector type_detector;

uint32_t FieldDictionaryManager::addFieldValue(const FieldKey& key, const std::string& value) {
    // 严格按照传入的FieldKey类型进行字典分配，不重新判断类型
    field_type_total_count_[key]++;
    if (!field_type_seen_[key]) {
        all_fields_and_types_.push_back(key);
        field_type_seen_[key] = true;
    }
    field_type_unique_values_[key].insert(value);
    switch (key.type) {
        case FieldType::Int: {
            int64_t v = std::stoll(value);
            auto ret = variable_dict_.addFieldValue(key, key.type, v);
            return ret;
        }
        case FieldType::Double: {
            double v = std::stod(value);
            auto ret = variable_dict_.addFieldValue(key, key.type, v);
            return ret;
        }
        case FieldType::Bool: {
            bool v = (value == "true");
            auto ret = variable_dict_.addFieldValue(key, key.type, v);
            return ret;
        }
        case FieldType::String: {
            auto ret = variable_dict_.addFieldValue(key, key.type, value);
            return ret;
        }
        case FieldType::Timestamp: {
            // 直接使用TimestampDictionary编码，与LogType保持一致
            auto encoded = timestamp_dict_.encodeTemplate(key, value);
            return encoded.template_id;
        }
        case FieldType::LogType: {
            // 直接调用logtype_dict_
            // std::cout << "[DEBUG] FieldDictionaryManager::addFieldValue - LogType field: " << key.name << " = '" << value << "'" << std::endl;
            auto encoded = logtype_dict_.encodeLog(key, value, {});
            // std::cout << "[DEBUG] FieldDictionaryManager::addFieldValue - LogType encoded template_id: " << encoded.template_id << std::endl;
            return encoded.template_id;
        }
        case FieldType::Null:
            return variable_dict_.addFieldValue(key, key.type, nullptr);
        default:
            return variable_dict_.addFieldValue(key, key.type, value);
    }
}

uint32_t FieldDictionaryManager::addFieldValue(const FieldKey& key, FieldType type, const Value& value) {
    // 严格按照传入的FieldKey类型进行字典分配，不重新判断类型
    field_type_total_count_[key]++;
    if (!field_type_seen_[key]) {
        all_fields_and_types_.push_back(key);
        field_type_seen_[key] = true;
    }
    
    // 将Value转换为字符串用于唯一值统计
    std::string value_str;
    if (std::holds_alternative<std::string>(value)) {
        value_str = std::get<std::string>(value);
    } else if (std::holds_alternative<int64_t>(value)) {
        value_str = std::to_string(std::get<int64_t>(value));
    } else if (std::holds_alternative<double>(value)) {
        value_str = std::to_string(std::get<double>(value));
    } else if (std::holds_alternative<bool>(value)) {
        value_str = std::get<bool>(value) ? "true" : "false";
    } else if (std::holds_alternative<std::nullptr_t>(value)) {
        value_str = "null";
    }
    field_type_unique_values_[key].insert(value_str);
    
    switch (key.type) {
        case FieldType::Int:
            if (std::holds_alternative<int64_t>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::get<int64_t>(value));
            } else if (std::holds_alternative<double>(value)) {
                return variable_dict_.addFieldValue(key, key.type, static_cast<int64_t>(std::get<double>(value)));
            } else if (std::holds_alternative<bool>(value)) {
                return variable_dict_.addFieldValue(key, key.type, static_cast<int64_t>(std::get<bool>(value)));
            } else {
                return variable_dict_.addFieldValue(key, key.type, int64_t(0));
            }
        case FieldType::Double:
            if (std::holds_alternative<double>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::get<double>(value));
            } else if (std::holds_alternative<int64_t>(value)) {
                return variable_dict_.addFieldValue(key, key.type, static_cast<double>(std::get<int64_t>(value)));
            } else if (std::holds_alternative<bool>(value)) {
                return variable_dict_.addFieldValue(key, key.type, static_cast<double>(std::get<bool>(value)));
            } else {
                return variable_dict_.addFieldValue(key, key.type, 0.0);
            }
        case FieldType::Bool:
            if (std::holds_alternative<bool>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::get<bool>(value));
            } else if (std::holds_alternative<int64_t>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::get<int64_t>(value) != 0);
            } else if (std::holds_alternative<double>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::get<double>(value) != 0.0);
            } else {
                return variable_dict_.addFieldValue(key, key.type, false);
            }
        case FieldType::String:
            if (std::holds_alternative<std::string>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::get<std::string>(value));
            } else if (std::holds_alternative<int64_t>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::to_string(std::get<int64_t>(value)));
            } else if (std::holds_alternative<double>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::to_string(std::get<double>(value)));
            } else if (std::holds_alternative<bool>(value)) {
                return variable_dict_.addFieldValue(key, key.type, std::get<bool>(value) ? "true" : "false");
            } else {
                return variable_dict_.addFieldValue(key, key.type, "");
            }
        case FieldType::Timestamp:
            if (std::holds_alternative<std::string>(value)) {
                auto encoded = timestamp_dict_.encodeTemplate(key, std::get<std::string>(value));
                return encoded.template_id;
            } else {
                return variable_dict_.addFieldValue(key, key.type, "");
            }
        case FieldType::LogType:
            if (std::holds_alternative<std::string>(value)) {
                // std::cout << "[DEBUG] FieldDictionaryManager::addFieldValue(Value) - LogType field: " << key.name << " = '" << std::get<std::string>(value) << "'" << std::endl;
                auto encoded = logtype_dict_.encodeLog(key, std::get<std::string>(value), {});
                // std::cout << "[DEBUG] FieldDictionaryManager::addFieldValue(Value) - LogType encoded template_id: " << encoded.template_id << std::endl;
                return encoded.template_id;
            } else {
                return variable_dict_.addFieldValue(key, key.type, "");
            }
        case FieldType::Null:
            return variable_dict_.addFieldValue(key, key.type, nullptr);
        default:
            return variable_dict_.addFieldValue(key, key.type, "");
    }
}

size_t FieldDictionaryManager::getUniqueValueCount(const FieldKey& key) const {
    auto it = field_type_unique_values_.find(key);
    if (it == field_type_unique_values_.end()) return 0;
    return it->second.size();
}

size_t FieldDictionaryManager::getTotalCount(const FieldKey& key) const {
    auto it = field_type_total_count_.find(key);
    if (it == field_type_total_count_.end()) return 0;
    return it->second;
}

std::vector<FieldKey> FieldDictionaryManager::getAllFieldsAndTypes() const {
    return all_fields_and_types_;
}

std::set<std::string> FieldDictionaryManager::getAllFields() const {
    std::set<std::string> fields;
    for (const auto& [field, _] : field_type_total_count_) {
        fields.insert(field.name);
    }
    return fields;
}

void FieldDictionaryManager::printRedundancyStats(std::ostream& out) const {
    out << std::setw(30) << "Field" << std::setw(12) << "Type" << std::setw(12) << "Total" << std::setw(12) << "Unique" << std::setw(12) << "Redundancy" << "\n";
    for (const auto& [field, type] : all_fields_and_types_) {
        size_t total = getTotalCount(FieldKey{field, type});
        size_t unique = getUniqueValueCount(FieldKey{field, type});
        
        // 使用与排序相同的冗余度计算方法（calculateRedundancyB）
        double redundancy = calculateRedundancyB(FieldKey{field, type}, total, unique);
        // double factor = (val_count > 0) ? (double)occ / val_count : 0.0;        
        // double factor = manager.calculateRedundancyA(key, occ, val_count); // 方案A：唯一值惩罚因子 (Fifth)
        // double factor = manager.calculateRedundancyB(key, occ, val_count); // 方案B：基于信息熵 (Best)
        // double factor = manager.calculateRedundancyC(key, occ, val_count); // 方案C：Trie结构影响因子 (Second)
        // double factor = manager.calculateRedundancyD(key, occ, val_count); // 方案D：自适应冗余度 (Third)
        
        std::string type_str;
        switch (type) {
            case FieldType::Int: type_str = "Int"; break;
            case FieldType::Double: type_str = "Double"; break;
            case FieldType::Bool: type_str = "Bool"; break;
            case FieldType::String: type_str = "String"; break;
            case FieldType::Timestamp: type_str = "Timestamp"; break;
            case FieldType::LogType: type_str = "LogType"; break;
            case FieldType::Null: type_str = "Null"; break;
            case FieldType::UnstructuredArray: type_str = "UnstructuredArray"; break;
            default: type_str = "Unknown"; break;
        }
        out << std::setw(30) << field << std::setw(12) << type_str << std::setw(12) << total << std::setw(12) << unique << std::setw(12) << redundancy << "\n";
    }
}

// 四种不同的冗余度计算方法实现
double FieldDictionaryManager::calculateRedundancyA(const FieldKey& key, size_t total, size_t unique) const {
    double base_redundancy = (unique > 0) ? (double)total / unique : 0.0;
    
    // 唯一值惩罚因子：唯一值越多，惩罚越重
    double uniqueness_penalty = 1.0 / (1.0 + unique * 0.1); // 可调参数
    
    return base_redundancy * uniqueness_penalty;
}

double FieldDictionaryManager::calculateRedundancyB(const FieldKey& key, size_t total, size_t unique) const {
    if (unique == 0) return 0.0;
    
    // 计算信息熵
    double entropy = 0.0;
    // 假设每个唯一值出现次数相等（简化计算）
    double p = 1.0 / unique;
    entropy = -unique * p * log2(p);
    
    // 最大熵（完全随机）
    double max_entropy = log2(unique);
    
    // 冗余度 = 1 - 归一化熵
    double normalized_entropy = entropy / max_entropy;
    return (1.0 - normalized_entropy) * total;
}

double FieldDictionaryManager::calculateRedundancyC(const FieldKey& key, size_t total, size_t unique) const {
    double base_redundancy = (unique > 0) ? (double)total / unique : 0.0;
    
    // Trie分支惩罚：唯一值越多，Trie分支越多
    double trie_branch_penalty = 1.0 / (1.0 + log2(unique + 1));
    
    // 字段重要性权重（可配置）
    double field_weight = 1.0;
    if (key.type == FieldType::Timestamp) {
        field_weight = 0.1; // 时间戳字段降权
    }
    
    return base_redundancy * trie_branch_penalty * field_weight;
}

double FieldDictionaryManager::calculateRedundancyD(const FieldKey& key, size_t total, size_t unique) const {
    if (unique == 0) return 0.0;
    
    double base_redundancy = (double)total / unique;
    
    // 唯一值比例
    double uniqueness_ratio = (double)unique / total;
    
    // 自适应惩罚：唯一值比例越高，惩罚越重
    double penalty = 1.0 / (1.0 + uniqueness_ratio * 10.0);
    
    // 字段类型特殊处理
    if (key.type == FieldType::Timestamp) {
        penalty *= 0.5; // 时间戳额外降权
    }
    
    return base_redundancy * penalty;
}

void FieldDictionaryManager::clear() {
    variable_dict_.clear();
    timestamp_dict_.clear();
    logtype_dict_.clear();
    field_type_total_count_.clear();
    all_fields_and_types_.clear();
    field_type_seen_.clear();
    field_type_unique_values_.clear();
}

void FieldDictionaryManager::setTimestampFields(const std::vector<std::string>& fields) {
    type_detector.setTimestampFields(fields);
}

bool FieldDictionaryManager::isTimestampField(const std::string& field) const {
    return type_detector.isTimestampField(field);
}

bool FieldDictionaryManager::isTimestampValue(const std::string& value) const {
    return type_detector.isTimestampValue(value);
}

bool FieldDictionaryManager::isLogTemplate(const std::string& value) const {
    return type_detector.isLogTemplate(value);
}

std::optional<Value> FieldDictionaryManager::getFieldValueByCode(const FieldKey& key, uint32_t code) const {
    switch (key.type) {
        case FieldType::String:
        case FieldType::Null:
        case FieldType::Int:
        case FieldType::Double:
        case FieldType::Bool:
        case FieldType::UnstructuredArray:
            return variable_dict_.getFieldValueByCode(key, code);
        case FieldType::Timestamp: {
            // 直接使用TimestampDictionary解码，与LogType保持一致
            std::string val = timestamp_dict_.getTemplateById(code);
            return val.empty() ? std::nullopt : std::optional<Value>(val);
        }
        case FieldType::LogType: {
            std::string val = logtype_dict_.getLogTypeById(code);
            return val.empty() ? std::nullopt : std::optional<Value>(val);
        }
        default:
            return std::nullopt;
    }
}

bool FieldDictionaryManager::isNestedField(const std::string& field_name) const {
    return !field_name.empty() && field_name[0] == '~';
}

} // namespace json2 