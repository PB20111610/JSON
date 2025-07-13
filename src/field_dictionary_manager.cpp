#include "../include/field_dictionary_manager.h"
#include <stdexcept>
#include <set>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace json2 {

// 简化的类型检测，仿照 clp_s 的方式
class SimpleTypeDetector {
private:
    std::vector<std::string> timestamp_fields_;
    
public:
    void setTimestampFields(const std::vector<std::string>& fields) {
        timestamp_fields_ = fields;
    }
    
    // 检查字段是否为时间戳字段
    bool isTimestampField(const std::string& field) const {
        return std::find(timestamp_fields_.begin(), timestamp_fields_.end(), field) != timestamp_fields_.end();
    }
    
    // 检查字符串是否为时间戳格式
    static bool isTimestampValue(const std::string& value) {
        if (value.empty()) return false;
        
        // 扩展的时间戳格式检测，包含毫秒和时区
        std::vector<std::string> patterns = {
            R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \w+)",  // 2023-03-27 00:26:35.719 EDT
            R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \w+)",        // 2023-03-27 00:26:35 EDT
            R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})",            // 2023-03-27 00:26:35
            R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)",           // 2023-03-27T00:26:35Z
            R"(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2})",            // 2023/03/27 00:26:35
        };
        
        for (const auto& pattern : patterns) {
            if (std::regex_match(value, std::regex(pattern))) {
                return true;
            }
        }
        return false;
    }
    
    // 检查字符串是否为日志模板（包含空格，但不是时间戳）
    static bool isLogTemplate(const std::string& value) {
        // 如果包含空格但不是时间戳，则认为是日志模板
        return value.find(' ') != std::string::npos && !isTimestampValue(value);
    }
};

static SimpleTypeDetector type_detector;

uint32_t FieldDictionaryManager::addFieldValue(const std::string& field, FieldType type, const std::string& value) {
    // 对于String类型，采用 clp_s 的简单检测方式
    FieldType actual_type = type;
    if (type == FieldType::String) {
        // 1. 检查是否为时间戳字段
        if (type_detector.isTimestampField(field) && type_detector.isTimestampValue(value)) {
            actual_type = FieldType::Timestamp;
        }
        // 2. 检查是否为日志模板（包含空格）
        else if (type_detector.isLogTemplate(value)) {
            actual_type = FieldType::LogType;
        }
        // 3. 默认为普通字符串
    }
    
    field_type_total_count_[field][actual_type]++;
    if (!field_type_seen_[field][actual_type]) {
        all_fields_and_types_.emplace_back(field, actual_type);
        field_type_seen_[field][actual_type] = true;
    }
    
    // 统计唯一值
    field_type_unique_values_[field][actual_type].insert(value);
    
    switch (actual_type) {
        case FieldType::Int: {
            int64_t v = std::stoll(value);
            return variable_dict_.addFieldValue(field, actual_type, v);
        }
        case FieldType::Double: {
            double v = std::stod(value);
            return variable_dict_.addFieldValue(field, actual_type, v);
        }
        case FieldType::Bool: {
            bool v = (value == "true");
            return variable_dict_.addFieldValue(field, actual_type, v);
        }
        case FieldType::String: {
            return variable_dict_.addFieldValue(field, actual_type, value);
        }
        case FieldType::Timestamp: {
            // 使用时间戳字典处理
            auto encoded = timestamp_dict_.encode(field, value);
            // 只返回pattern_id，epoch信息存储在字典内部
            return encoded.pattern_id;
        }
        case FieldType::LogType: {
            // 使用日志类型字典处理，自动提取模板和变量
            auto encoded = logtype_dict_.encodeLog(value, {});
            return encoded.template_id;
        }
        case FieldType::Null: {
            return variable_dict_.addFieldValue(field, actual_type, nullptr);
        }
        default:
            return 0;
    }
}

size_t FieldDictionaryManager::getUniqueValueCount(const std::string& field, FieldType type) const {
    auto it = field_type_unique_values_.find(field);
    if (it == field_type_unique_values_.end()) return 0;
    auto it2 = it->second.find(type);
    if (it2 == it->second.end()) return 0;
    return it2->second.size();
}

size_t FieldDictionaryManager::getTotalCount(const std::string& field, FieldType type) const {
    auto it = field_type_total_count_.find(field);
    if (it == field_type_total_count_.end()) return 0;
    auto it2 = it->second.find(type);
    if (it2 == it->second.end()) return 0;
    return it2->second;
}

std::vector<std::tuple<std::string, FieldType>> FieldDictionaryManager::getAllFieldsAndTypes() const {
    return all_fields_and_types_;
}

std::set<std::string> FieldDictionaryManager::getAllFields() const {
    std::set<std::string> fields;
    for (const auto& [field, _] : field_type_total_count_) {
        fields.insert(field);
    }
    return fields;
}

void FieldDictionaryManager::printRedundancyStats(std::ostream& out) const {
    out << std::setw(30) << "Field" << std::setw(12) << "Type" << std::setw(12) << "Total" << std::setw(12) << "Unique" << std::setw(12) << "Redundancy" << "\n";
    for (const auto& [field, type] : all_fields_and_types_) {
        size_t total = getTotalCount(field, type);
        size_t unique = getUniqueValueCount(field, type);
        double redundancy = unique ? (double)total / unique : 0;
        std::string type_str;
        switch (type) {
            case FieldType::Int: type_str = "Int"; break;
            case FieldType::Double: type_str = "Double"; break;
            case FieldType::Bool: type_str = "Bool"; break;
            case FieldType::String: type_str = "String"; break;
            case FieldType::Timestamp: type_str = "Timestamp"; break;
            case FieldType::LogType: type_str = "LogType"; break;
            case FieldType::Null: type_str = "Null"; break;
            default: type_str = "Unknown"; break;
        }
        out << std::setw(30) << field << std::setw(12) << type_str << std::setw(12) << total << std::setw(12) << unique << std::setw(12) << redundancy << "\n";
    }
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
    return SimpleTypeDetector::isTimestampValue(value);
}

bool FieldDictionaryManager::isLogTemplate(const std::string& value) const {
    return SimpleTypeDetector::isLogTemplate(value);
}

} // namespace json2 