#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "dictionary.h"

namespace json2 {

// JSON值的类型枚举
enum class JsonType {
    STRING,
    NUMBER,
    BOOLEAN,
    NULL_TYPE,
    OBJECT,
    ARRAY,
    TIMESTAMP,
    LOG_TYPE
};

// JSON值的基类
class JsonValue {
public:
    virtual ~JsonValue() = default;
    virtual JsonType getType() const = 0;
    virtual std::string toString() const = 0;
};

// 字符串类型的JSON值
class JsonString : public JsonValue {
public:
    explicit JsonString(const std::string& value) : value_(value) {}
    JsonType getType() const override { return JsonType::STRING; }
    std::string toString() const override { return value_; }
    const std::string& getValue() const { return value_; }

private:
    std::string value_;
};

// 数字类型的JSON值
class JsonNumber : public JsonValue {
public:
    explicit JsonNumber(double value) : value_(value), original_str_(std::to_string(value)) {}
    JsonNumber(double value, const std::string& original_str) : value_(value), original_str_(original_str) {}
    JsonType getType() const override { return JsonType::NUMBER; }
    std::string toString() const override { return original_str_; }
    double getValue() const { return value_; }
    const std::string& getOriginalString() const { return original_str_; }

private:
    double value_;
    std::string original_str_;  // 存储原始的字符串表示
};

// 布尔类型的JSON值
class JsonBoolean : public JsonValue {
public:
    explicit JsonBoolean(bool value) : value_(value) {}
    JsonType getType() const override { return JsonType::BOOLEAN; }
    std::string toString() const override { return value_ ? "true" : "false"; }
    bool getValue() const { return value_; }

private:
    bool value_;
};

// 空类型的JSON值
class JsonNull : public JsonValue {
public:
    JsonType getType() const override { return JsonType::NULL_TYPE; }
    std::string toString() const override { return "null"; }
};

// 时间戳类型的JSON值
class JsonTimestamp : public JsonValue {
public:
    explicit JsonTimestamp(const std::string& value) : value_(value) {}
    JsonType getType() const override { return JsonType::TIMESTAMP; }
    std::string toString() const override { return value_; }
    const std::string& getValue() const { return value_; }

private:
    std::string value_;
};

// 日志类型（长句子）的JSON值
class JsonLogType : public JsonValue {
public:
    explicit JsonLogType(const std::string& value) : value_(value) {}
    JsonType getType() const override { return JsonType::LOG_TYPE; }
    std::string toString() const override { return value_; }
    const std::string& getValue() const { return value_; }

private:
    std::string value_;
};

// 数组类型的JSON值
class JsonArray : public JsonValue {
public:
    JsonType getType() const override { return JsonType::ARRAY; }
    std::string toString() const override;
    
    void addValue(std::shared_ptr<JsonValue> value) { values_.push_back(value); }
    const std::vector<std::shared_ptr<JsonValue>>& getValues() const { return values_; }
    size_t size() const { return values_.size(); }
    std::shared_ptr<JsonValue> getValue(size_t index) const { return values_[index]; }

private:
    std::vector<std::shared_ptr<JsonValue>> values_;
};

// JSON对象类
class JsonObject : public JsonValue {
public:
    JsonType getType() const override { return JsonType::OBJECT; }
    void addField(const std::string& key, std::shared_ptr<JsonValue> value);
    std::shared_ptr<JsonValue> getField(const std::string& key) const;
    const std::unordered_map<std::string, std::shared_ptr<JsonValue>>& getFields() const { return fields_; }
    std::string toString() const override;

private:
    std::unordered_map<std::string, std::shared_ptr<JsonValue>> fields_;
};

// 字段及其字典类型
struct ParsedField {
    std::string name;           // 完整字段名（如 "metadata.browser"）
    DictType dictType;

    ParsedField(const std::string& full_name, DictType type) 
        : name(full_name), dictType(type) {}
};

// JSON解析器类
class JsonParser {
public:
    static std::shared_ptr<JsonObject> parse(const std::string& jsonStr);
    static std::vector<std::shared_ptr<JsonObject>> parseLogFile(const std::string& filePath);

    // 扫描文件，确定字段顺序、类型和值域，并填充字典
    static void parseAndCollect(
        const std::string& filePath,
        Dictionary& dict,
        std::vector<ParsedField>& fieldOrder // 记录字段名和类型
    );

private:
    static std::shared_ptr<JsonValue> parseValue(const std::string& jsonStr, size_t& pos);
    static std::string parseString(const std::string& jsonStr, size_t& pos);
    static std::string parseNumber(const std::string& jsonStr, size_t& pos);
    static std::shared_ptr<JsonArray> parseArray(const std::string& jsonStr, size_t& pos);
    static void skipWhitespace(const std::string& jsonStr, size_t& pos);
};

} // namespace json2
