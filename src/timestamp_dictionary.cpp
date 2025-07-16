#include "../include/timestamp_dictionary.h"
#include "../include/variable_dictionary.h"
#include <regex>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <variant>
#include <iostream> // Added for [DEBUG] output

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

std::pair<uint32_t, int64_t> TimestampDictionary::addTimestamp(const FieldKey& key, const std::string& value) {
    return addTimestamp(key.name, value);
}

EncodedTimestamp TimestampDictionary::encode(const FieldKey& key, const std::string& value) {
    // 实际编码逻辑
    auto [fmt, epoch] = parseTimestamp(value);
    if (fmt.empty() || epoch < 0) {
        // 解析失败，返回无效编码
        return {0, -1};
    }
    uint32_t pattern_id;
    auto it = pattern_to_id_.find(fmt);
    if (it == pattern_to_id_.end()) {
        pattern_id = next_pattern_id_++;
        pattern_to_id_[fmt] = pattern_id;
        id_to_pattern_.push_back(fmt);
    } else {
        pattern_id = it->second;
    }
    // 更新字段时间范围
    auto& range = field_ranges_[key.name];
    if (epoch < range.min) range.min = epoch;
    if (epoch > range.max) range.max = epoch;
    // 存储反查映射
    encoded_to_value_[pattern_id][epoch] = value;
    return {pattern_id, epoch};
}

std::string TimestampDictionary::decode(const FieldKey& key, const EncodedTimestamp& encoded) const {
    // std::cout << "[DEBUG][TimestampDictionary::decode] field=" << key.name << ", pattern_id=" << encoded.pattern_id << ", epoch=" << encoded.epoch << std::endl;
    // 实际解码逻辑
    if (encoded.pattern_id > 0 && encoded.pattern_id <= id_to_pattern_.size()) {
        // 反查原始字符串
        auto it_epoch = encoded_to_value_.find(encoded.pattern_id);
        if (it_epoch != encoded_to_value_.end()) {
            auto it_val = it_epoch->second.find(encoded.epoch);
            if (it_val != it_epoch->second.end()) {
                // std::cout << "[DEBUG][TimestampDictionary::decode] found value: " << it_val->second << std::endl;
                return it_val->second;
            }
        }
        // std::cout << "[DEBUG][TimestampDictionary::decode] not found, pattern_id or epoch missing" << std::endl;
        // 若找不到原始字符串，可选：格式化epoch为字符串
        // std::time_t t = encoded.epoch;
        // std::tm* tm_ptr = std::gmtime(&t);
        // char buf[64];
        // if (tm_ptr && std::strftime(buf, sizeof(buf), id_to_pattern_[encoded.pattern_id-1].c_str(), tm_ptr)) {
        //     return std::string(buf);
        // }
    }
    // std::cout << "[DEBUG][TimestampDictionary::decode] invalid pattern_id, return empty string" << std::endl;
    return "";
}

std::string TimestampDictionary::getPatternById(uint32_t id) const {
    if (id < id_to_pattern_.size()) return id_to_pattern_[id];
    return "";
}

std::pair<int64_t, int64_t> TimestampDictionary::getRange(const FieldKey& key) const {
    return getRange(key.name);
}

uint32_t TimestampDictionary::getOrAddFieldValue(const FieldKey& key, const Value& value) {
    // 只支持 string 类型
    if (!std::holds_alternative<std::string>(value)) {
        throw std::invalid_argument("TimestampDictionary only supports string values");
    }
    const std::string& str = std::get<std::string>(value);
    auto encoded = encode(key, str);
    return encoded.pattern_id; // 这里只返回 pattern_id 作为编码，实际可扩展
}

void TimestampDictionary::clear() {
    pattern_to_id_.clear();
    id_to_pattern_.clear();
    next_pattern_id_ = 1;
    field_ranges_.clear();
    encoded_to_value_.clear();
}

} // namespace json2 