#include "../include/timestamp_dictionary.h"
#include <regex>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace json2 {

// 支持的时间戳格式（可扩展）
static const std::vector<std::pair<std::string, std::string>> kPatterns = {
    // 格式字符串, strptime格式
    {R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \w+)", "%Y-%m-%d %H:%M:%S"},  // 2023-03-27 00:26:35.719 EDT
    {R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \w+)", "%Y-%m-%d %H:%M:%S"},        // 2023-03-27 00:26:35 EDT
    {R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})", "%Y-%m-%d %H:%M:%S"},            // 2023-03-27 00:26:35
    {R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)", "%Y-%m-%dT%H:%M:%SZ"},           // 2023-03-27T00:26:35Z
    {R"(\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2})", "%Y/%m/%d %H:%M:%S"},            // 2023/03/27 00:26:35
    // 可扩展更多
};

// 尝试匹配并解析时间戳字符串，返回(格式字符串, epoch秒)
static std::pair<std::string, int64_t> parseTimestamp(const std::string& value) {
    for (const auto& [regex_str, fmt] : kPatterns) {
        if (std::regex_match(value, std::regex(regex_str))) {
            std::tm tm = {};
            std::string time_part = value;
            
            // 如果包含毫秒和时区，提取基本时间部分
            if (value.find('.') != std::string::npos) {
                // 包含毫秒，提取到秒的部分
                size_t dot_pos = value.find('.');
                time_part = value.substr(0, dot_pos);
            } else if (value.find(' ') != std::string::npos && value.find(' ') < value.length() - 4) {
                // 可能包含时区，提取到秒的部分
                size_t last_space = value.rfind(' ');
                if (last_space > 15) { // 确保有足够的时间部分
                    time_part = value.substr(0, last_space);
                }
            }
            
            std::istringstream ss(time_part);
            ss >> std::get_time(&tm, fmt.c_str());
            if (!ss.fail()) {
                time_t epoch = timegm(&tm); // 使用UTC
                return {fmt, static_cast<int64_t>(epoch)};
            }
        }
    }
    // 解析失败，返回空格式和-1
    return {"", -1};
}

std::pair<uint32_t, int64_t> TimestampDictionary::addTimestamp(const std::string& field, const std::string& value) {
    auto [fmt, epoch] = parseTimestamp(value);
    if (fmt.empty() || epoch < 0) {
        // 解析失败，全部归为格式0，值-1
        return {0, -1};
    }
    // 分配格式ID
    uint32_t pattern_id;
    auto it = pattern_to_id_.find(fmt);
    if (it == pattern_to_id_.end()) {
        pattern_id = next_pattern_id_++;
        pattern_to_id_[fmt] = pattern_id;
        id_to_pattern_.resize(pattern_id + 1);
        id_to_pattern_[pattern_id] = fmt;
    } else {
        pattern_id = it->second;
    }
    // 更新范围
    auto& range = field_ranges_[field];
    if (epoch < range.min) range.min = epoch;
    if (epoch > range.max) range.max = epoch;
    
    // 填充反查映射，确保解码功能正常工作
    encoded_to_value_[pattern_id][epoch] = value;
    
    return {pattern_id, epoch};
}

EncodedTimestamp TimestampDictionary::encode(const std::string& field, const std::string& value) {
    auto [pattern_id, epoch] = addTimestamp(field, value);
    if (pattern_id != 0 && epoch != -1) {
        encoded_to_value_[pattern_id][epoch] = value;
    }
    return {pattern_id, epoch};
}

std::string TimestampDictionary::decode(const EncodedTimestamp& encoded) const {
    auto it1 = encoded_to_value_.find(encoded.pattern_id);
    if (it1 != encoded_to_value_.end()) {
        auto it2 = it1->second.find(encoded.epoch);
        if (it2 != it1->second.end()) {
            return it2->second;
        }
    }
    return "";
}

std::string TimestampDictionary::getPatternById(uint32_t id) const {
    if (id < id_to_pattern_.size()) return id_to_pattern_[id];
    return "";
}

std::pair<int64_t, int64_t> TimestampDictionary::getRange(const std::string& field) const {
    auto it = field_ranges_.find(field);
    if (it == field_ranges_.end()) return {0, 0};
    return {it->second.min, it->second.max};
}



void TimestampDictionary::clear() {
    pattern_to_id_.clear();
    id_to_pattern_.clear();
    next_pattern_id_ = 1;
    field_ranges_.clear();
    encoded_to_value_.clear();
}

} // namespace json2 