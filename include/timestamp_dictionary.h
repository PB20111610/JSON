#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <limits>
#include <utility>
#include <ostream>
#include <iostream>
#include "field_key.h"
#include "variable_dictionary.h"
#include <variant>

namespace json2 {

struct EncodedTimestamp {
    uint32_t pattern_id;
    int64_t epoch;
    bool operator==(const EncodedTimestamp& other) const {
        return pattern_id == other.pattern_id && epoch == other.epoch;
    }
    bool operator!=(const EncodedTimestamp& other) const {
        return !(*this == other);
    }
};

// 添加operator<<定义
inline std::ostream& operator<<(std::ostream& os, const EncodedTimestamp& ts) {
    return os << "Timestamp{pattern=" << ts.pattern_id << ",epoch=" << ts.epoch << "}";
}

class TimestampDictionary {
public:
    // 添加时间戳，返回格式ID和纪元时间
    std::pair<uint32_t, int64_t> addTimestamp(const FieldKey& key, const std::string& value);
    std::pair<uint32_t, int64_t> addTimestamp(const std::string& field, const std::string& value) { return addTimestamp(FieldKey{field, FieldType::Timestamp}, value); }
    // 获取格式ID对应的格式字符串
    std::string getPatternById(uint32_t id) const;
    // 获取字段的时间范围
    std::pair<int64_t, int64_t> getRange(const FieldKey& key) const;
    std::pair<int64_t, int64_t> getRange(const std::string& field) const { return getRange(FieldKey{field, FieldType::Timestamp}); }
    // 编码/解码接口
    EncodedTimestamp encode(const FieldKey& key, const std::string& value);
    EncodedTimestamp encode(const std::string& field, const std::string& value) { return encode(FieldKey{field, FieldType::Timestamp}, value); }
    std::string decode(const FieldKey& key, const EncodedTimestamp& encoded) const;
    std::string decode(const std::string& field, const EncodedTimestamp& encoded) const { return decode(FieldKey{field, FieldType::Timestamp}, encoded); }
    void clear();
    // 兼容 Trie 类型分发接口
    uint32_t getOrAddFieldValue(const FieldKey& key, const Value& value);
    void registerPattern(const std::string& pattern);

private:
    // 格式字符串到ID
    std::unordered_map<std::string, uint32_t> pattern_to_id_;
    std::vector<std::string> id_to_pattern_;
    uint32_t next_pattern_id_ = 1;
    // 每个字段的时间范围
    struct Range { int64_t min = std::numeric_limits<int64_t>::max(); int64_t max = std::numeric_limits<int64_t>::min(); };
    std::unordered_map<std::string, Range> field_ranges_;
};

} // namespace json2 