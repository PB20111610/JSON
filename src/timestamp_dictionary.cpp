#include "../include/timestamp_dictionary.h"
#include "../include/variable_dictionary.h"
#include <regex>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <variant>
#include <iostream>
#include <map>

namespace json2 {

// 支持的时间戳格式（pattern, regex, 是否有毫秒, 是否有Z时区, 是否有三字母时区）
struct PatternInfo {
    std::string regex_str;
    std::string fmt;
    bool has_millis;
    bool has_z;
    bool has_tz;
};

static const std::vector<PatternInfo> kPatterns = {
    // 2024-03-20 10:30:45.123 EDT
    {R"((\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\.(\d{3}) (\w+))", "%Y-%m-%d %H:%M:%S.%f %Z", true, false, true},
    // 2024-03-20 10:30:45 EDT
    {R"((\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) (\w+))", "%Y-%m-%d %H:%M:%S %Z", false, false, true},
    // 2024-03-20 10:30:45.123
    {R"((\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\.(\d{3}))", "%Y-%m-%d %H:%M:%S.%f", true, false, false},
    // 2024-03-20 10:30:45
    {R"((\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}))", "%Y-%m-%d %H:%M:%S", false, false, false},
    // 2024-03-20T10:30:45.123Z
    {R"((\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})\.(\d{3})Z)", "%Y-%m-%dT%H:%M:%S.%fZ", true, true, false},
    // 2024-03-20T10:30:45Z
    {R"((\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})Z)", "%Y-%m-%dT%H:%M:%SZ", false, true, false},
    // 2024/03/20 10:30:45
    {R"((\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}))", "%Y/%m/%d %H:%M:%S", false, false, false},
    // 可扩展更多
};

// 解析时间戳字符串，返回(pattern, epoch毫秒, tz字符串)
static std::tuple<std::string, int64_t, std::string> parseTimestamp(const std::string& value) {
    for (const auto& pat : kPatterns) {
        std::smatch match;
        if (std::regex_match(value, match, std::regex(pat.regex_str))) {
            std::tm tm = {};
            int millis = 0;
            std::string time_part = match[1];
            std::string tz_str;
            if (pat.has_millis) {
                millis = std::stoi(match[2]);
            }
            if (pat.has_tz) {
                tz_str = match[pat.has_millis ? 3 : 2];
            } else if (pat.has_z) {
                tz_str = "Z";
                }
            // 解析时间
            std::istringstream ss(time_part);
            std::string fmt = pat.fmt;
            // 去掉.%f和%Z/Z，strptime/strftime不支持
            size_t pos;
            if ((pos = fmt.find(".%f")) != std::string::npos) fmt.erase(pos, 3);
            if ((pos = fmt.find(" %Z")) != std::string::npos) fmt.erase(pos, 4);
            if ((pos = fmt.find("%Z")) != std::string::npos) fmt.erase(pos, 2);
            if ((pos = fmt.find("Z")) != std::string::npos) fmt.erase(pos, 1);
            ss >> std::get_time(&tm, fmt.c_str());
            if (ss.fail()) continue;
            time_t epoch = timegm(&tm); // UTC
            int64_t epoch_ms = static_cast<int64_t>(epoch) * 1000 + millis;
            return {pat.fmt, epoch_ms, tz_str};
            }
        }
    return {"", -1, ""};
}

// 反向格式化
static std::string formatTimestamp(const std::string& pattern, int64_t epoch_ms, const std::string& tz_str) {
    time_t epoch = epoch_ms / 1000;
    int millis = epoch_ms % 1000;
    std::tm* tm_ptr = gmtime(&epoch);
    char buf[64];
    std::string fmt = pattern;
    // 去掉.%f和%Z/Z，strftime不支持
    size_t pos;
    if ((pos = fmt.find(".%f")) != std::string::npos) fmt.erase(pos, 3);
    if ((pos = fmt.find(" %Z")) != std::string::npos) fmt.erase(pos, 4);
    if ((pos = fmt.find("%Z")) != std::string::npos) fmt.erase(pos, 2);
    if ((pos = fmt.find("Z")) != std::string::npos) fmt.erase(pos, 1);
    strftime(buf, sizeof(buf), fmt.c_str(), tm_ptr);
    std::string result(buf);
    // 补毫秒
    if (pattern.find(".%f") != std::string::npos) {
        char msbuf[8];
        snprintf(msbuf, sizeof(msbuf), ".%03d", millis);
        result += msbuf;
    }
    // 补空格和时区
    if (pattern.find("%Z") != std::string::npos) {
        result += " ";
        result += tz_str;
    } else if (pattern.find("Z") != std::string::npos) {
        result += "Z";
    }
    return result;
}

// 存储 pattern_id+epoch -> tz 字符串
static std::map<std::pair<uint32_t, int64_t>, std::string> pattern_epoch_to_tz;

std::pair<uint32_t, int64_t> TimestampDictionary::addTimestamp(const FieldKey& key, const std::string& value) {
    return addTimestamp(key.name, value);
}

EncodedTimestamp TimestampDictionary::encode(const FieldKey& key, const std::string& value) {
    auto [fmt, epoch, tz] = parseTimestamp(value);
    if (fmt.empty() || epoch < 0) {
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
    // 存储 pattern_id+epoch -> tz
    if (!tz.empty()) {
        pattern_epoch_to_tz[{pattern_id, epoch}] = tz;
    }
    return {pattern_id, epoch};
}

std::string TimestampDictionary::decode(const FieldKey& key, const EncodedTimestamp& encoded) const {
    if (encoded.pattern_id > 0 && encoded.pattern_id <= id_to_pattern_.size()) {
        const std::string& pattern = id_to_pattern_[encoded.pattern_id - 1];
        std::string tz;
        auto it = pattern_epoch_to_tz.find({encoded.pattern_id, encoded.epoch});
        if (it != pattern_epoch_to_tz.end()) {
            tz = it->second;
        }
        return formatTimestamp(pattern, encoded.epoch, tz);
    }
    return "";
}

std::string TimestampDictionary::getPatternById(uint32_t id) const {
    if (id > 0 && id <= id_to_pattern_.size()) return id_to_pattern_[id - 1];
    return "";
}

std::pair<int64_t, int64_t> TimestampDictionary::getRange(const FieldKey& key) const {
    return getRange(key.name);
}

uint32_t TimestampDictionary::getOrAddFieldValue(const FieldKey& key, const Value& value) {
    if (!std::holds_alternative<std::string>(value)) {
        throw std::invalid_argument("TimestampDictionary only supports string values");
    }
    const std::string& str = std::get<std::string>(value);
    auto encoded = encode(key, str);
    return encoded.pattern_id;
}

void TimestampDictionary::clear() {
    pattern_to_id_.clear();
    id_to_pattern_.clear();
    next_pattern_id_ = 1;
    field_ranges_.clear();
    pattern_epoch_to_tz.clear();
}

void TimestampDictionary::registerPattern(const std::string& pattern) {
    if (pattern_to_id_.count(pattern)) return;
    pattern_to_id_[pattern] = next_pattern_id_++;
    id_to_pattern_.push_back(pattern);
}

} // namespace json2 