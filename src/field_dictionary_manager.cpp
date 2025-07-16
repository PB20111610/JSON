#include "../include/field_dictionary_manager.h"
#include <stdexcept>
#include <set>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <iostream> // Added for debug output

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

uint32_t FieldDictionaryManager::addFieldValue(const FieldKey& key, const std::string& value) {
    FieldType actual_type = key.type;
    if (key.type == FieldType::String) {
        if (type_detector.isTimestampField(key.name) && type_detector.isTimestampValue(value)) {
            actual_type = FieldType::Timestamp;
        } else if (type_detector.isLogTemplate(value)) {
            actual_type = FieldType::LogType;
        }
    }
    FieldKey actual_key{key.name, static_cast<FieldType>(actual_type)};
    // std::cout << "[DEBUG] field_type_total_count_++ 前, key=" << actual_key.name << ", type=" << static_cast<int>(actual_key.type) << std::endl;
    field_type_total_count_[actual_key]++;
    // std::cout << "[DEBUG] field_type_total_count_++ 后" << std::endl;
    if (!field_type_seen_[actual_key]) {
        // std::cout << "[DEBUG] all_fields_and_types_ push_back 前" << std::endl;
        all_fields_and_types_.push_back(actual_key);
        field_type_seen_[actual_key] = true;
        // std::cout << "[DEBUG] all_fields_and_types_ push_back 后" << std::endl;
    }
    // std::cout << "[DEBUG] field_type_unique_values_ insert 前" << std::endl;
    field_type_unique_values_[actual_key].insert(value);
    // std::cout << "[DEBUG] field_type_unique_values_ insert 后" << std::endl;
    switch (actual_type) {
        case FieldType::Int: {
            int64_t v = std::stoll(value);
            auto ret = variable_dict_.addFieldValue(key, actual_type, v);
            return ret;
        }
        case FieldType::Double: {
            double v = std::stod(value);
            auto ret = variable_dict_.addFieldValue(key, actual_type, v);
            return ret;
        }
        case FieldType::Bool: {
            bool v = (value == "true");
            auto ret = variable_dict_.addFieldValue(key, actual_type, v);
            return ret;
        }
        case FieldType::String: {
            auto ret = variable_dict_.addFieldValue(key, actual_type, value);
            return ret;
        }
        case FieldType::Timestamp: {
            // 直接调用timestamp_dict_
            auto encoded = timestamp_dict_.encode(key.name, value);
            return encoded.pattern_id;
        }
        case FieldType::LogType: {
            // 直接调用logtype_dict_
            auto encoded = logtype_dict_.encodeLog(key, value, {});
            return encoded.template_id;
        }
        default:
            throw std::invalid_argument("Unsupported type in addFieldValue");
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

std::optional<Value> FieldDictionaryManager::getFieldValueByCode(const FieldKey& key, uint32_t code) const {
    switch (key.type) {
        case FieldType::String:
        case FieldType::Null:
        case FieldType::Int:
        case FieldType::Double:
        case FieldType::Bool:
            return variable_dict_.getFieldValueByCode(key, code);
        case FieldType::Timestamp: {
            EncodedTimestamp encoded{code, 0};
            std::string val = timestamp_dict_.decode(key, encoded);
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

} // namespace json2 