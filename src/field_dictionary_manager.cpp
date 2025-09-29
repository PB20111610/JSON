#include "../include/field_dictionary_manager.h"
#include <stdexcept>
#include <set>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <iostream> // Added for debug output
#include <cmath>
#include <unordered_set>
#include <functional>
#include <simdjson.h>
#include "../include/field_parser.h" 

namespace json2 {

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
            auto encoded = logtype_dict_.encodeLog(key, value, {});
            return encoded.template_id;
        }
        case FieldType::Null:
            return variable_dict_.addFieldValue(key, key.type, nullptr);
        default:
            return variable_dict_.addFieldValue(key, key.type, value);
    }
}

uint32_t FieldDictionaryManager::addFieldValue(const FieldKey& key, FieldType type, int64_t value) {
    // Update counters
    field_type_total_count_[key]++;
    if (!field_type_seen_[key]) {
        all_fields_and_types_.push_back(key);
        field_type_seen_[key] = true;
    }
    field_type_unique_values_[key].insert(std::to_string(value));
    
    // Delegate to variable dictionary
    return variable_dict_.addFieldValue(key, type, value);
}

uint32_t FieldDictionaryManager::addFieldValue(const FieldKey& key, FieldType type, double value) {
    // Update counters
    field_type_total_count_[key]++;
    if (!field_type_seen_[key]) {
        all_fields_and_types_.push_back(key);
        field_type_seen_[key] = true;
    }
    field_type_unique_values_[key].insert(std::to_string(value));
    
    // Delegate to variable dictionary
    return variable_dict_.addFieldValue(key, type, value);
}

uint32_t FieldDictionaryManager::addFieldValue(const FieldKey& key, FieldType type, bool value) {
    // Update counters
    field_type_total_count_[key]++;
    if (!field_type_seen_[key]) {
        all_fields_and_types_.push_back(key);
        field_type_seen_[key] = true;
    }
    field_type_unique_values_[key].insert(value ? "true" : "false");
    // Delegate to variable dictionary
    return variable_dict_.addFieldValue(key, type, value);
}

uint32_t FieldDictionaryManager::addFieldValue(const FieldKey& key, FieldType type, std::nullptr_t value) {
    // Update counters
    field_type_total_count_[key]++;
    if (!field_type_seen_[key]) {
        all_fields_and_types_.push_back(key);
        field_type_seen_[key] = true;
    }
    field_type_unique_values_[key].insert("null");
    
    // Delegate to variable dictionary
    return variable_dict_.addFieldValue(key, type, value);
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
                auto encoded = logtype_dict_.encodeLog(key, std::get<std::string>(value), {});
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

double FieldDictionaryManager::calculateRedundancy(const FieldKey& key, size_t total, size_t unique) const {
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