#include "../include/parser.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <regex>
#include <iostream>

namespace json2 {

// 检查是否是时间戳格式
bool isTimestamp(const std::string& str) {
    // 匹配两种时间戳格式：
    // 1. "2023-03-27 00:26:37.665 EDT" (带毫秒)
    // 2. "2023-03-27 00:26:37 EDT" (不带毫秒)
    static const std::regex timestamp_pattern(
        R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(\.\d+)? [A-Z]{3,4}$)"
    );
    return std::regex_match(str, timestamp_pattern);
}

// 检查是否是日志类型（长句子）
bool isLogType(const std::string& str) {
    // 如果字符串包含多个单词（包含空格），且不是时间戳格式，则认为是日志类型
    return str.find(' ') != std::string::npos && !isTimestamp(str);
}

// 辅助：判断字段值类型
DictType getDictType(const std::string& value) {
    static const std::regex timestamp_pattern(
        R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(\.\d+)? [A-Z]{3,4}$)"
    );
    
    // 检查是否是时间戳
    if (std::regex_match(value, timestamp_pattern)) {
        return DictType::TIMESTAMP_DICT;
    }
    
    // 检查是否是布尔值
    if (value == "true" || value == "false") {
        return DictType::RAW_BOOLEAN;
    }
    
    // 检查是否是数值（必须是纯数字格式，包括小数点和负号）
    static const std::regex number_pattern(R"(^-?\d+(\.\d+)?$)");
    if (std::regex_match(value, number_pattern)) {
        return DictType::RAW_NUMBER;
    }
    
    // 检查是否是日志类型（长句子）
    if (value.find(' ') != std::string::npos) {
        return DictType::LOG_DICT;
    }
    
    // 默认为变量类型（包括INFO, WARN, ERROR, DEBUG等）
    return DictType::VARIABLE_DICT;
}

// 简单属性-值对解析（适合结构化日志，无嵌套/转义）
std::unordered_map<std::string, std::string> parseFlatJson(const std::string& jsonStr) {
    std::unordered_map<std::string, std::string> kvs;
    size_t pos = 0, len = jsonStr.length();
    
    // 跳过开头的 '{' 和结尾的 '}'
    while (pos < len && (isspace(jsonStr[pos]) || jsonStr[pos] == '{')) ++pos;
    if (pos >= len) return kvs;
    
    // 解析顶层对象
    while (pos < len) {
        // 跳过空白和逗号
        while (pos < len && (isspace(jsonStr[pos]) || jsonStr[pos] == ',')) ++pos;
        if (pos >= len || jsonStr[pos] == '}') break;
        
        // 解析key
        if (jsonStr[pos] != '"') throw std::runtime_error("Invalid JSON: expected '\"' at key");
        size_t key_start = ++pos;
        while (pos < len && jsonStr[pos] != '"') ++pos;
        if (pos >= len) throw std::runtime_error("Invalid JSON: unterminated key");
        std::string key = jsonStr.substr(key_start, pos - key_start);
        ++pos;
        
        // 跳过冒号
        while (pos < len && (isspace(jsonStr[pos]) || jsonStr[pos] == ':')) ++pos;
        
        // 解析value
        if (jsonStr[pos] == '{') {
            // 处理嵌套对象
            size_t nested_start = pos;
            int brace_count = 1;
            ++pos;
            while (pos < len && brace_count > 0) {
                if (jsonStr[pos] == '{') ++brace_count;
                else if (jsonStr[pos] == '}') --brace_count;
                ++pos;
            }
            if (brace_count > 0) throw std::runtime_error("Invalid JSON: unterminated nested object");
            
            // 提取嵌套对象的字符串（包含花括号）
            std::string nested_str = jsonStr.substr(nested_start, pos - nested_start);
            
            // 递归解析嵌套对象
            auto nested_kvs = parseFlatJson(nested_str);
            
            // 将嵌套字段添加到结果中，使用点号连接
            for (const auto& nested_kv : nested_kvs) {
                std::string full_key = key + "." + nested_kv.first;
                kvs[full_key] = nested_kv.second;
            }
        } else if (jsonStr[pos] == '[') {
            // 处理数组
            size_t array_start = pos;
            int bracket_count = 1;
            ++pos;
            while (pos < len && bracket_count > 0) {
                if (jsonStr[pos] == '[') ++bracket_count;
                else if (jsonStr[pos] == ']') --bracket_count;
                ++pos;
            }
            if (bracket_count > 0) throw std::runtime_error("Invalid JSON: unterminated array");
            
            // 将整个数组作为一个值存储
            std::string array_str = jsonStr.substr(array_start, pos - array_start);
            kvs[key] = array_str;
        } else if (jsonStr[pos] == '"') {
            // 处理字符串
            size_t val_start = ++pos;
            while (pos < len && jsonStr[pos] != '"') ++pos;
            if (pos >= len) throw std::runtime_error("Invalid JSON: unterminated value");
            kvs[key] = jsonStr.substr(val_start, pos - val_start);
            ++pos;
        } else {
            // 处理其他值（数字、布尔值、null）
            size_t val_start = pos;
            while (pos < len && jsonStr[pos] != ',' && jsonStr[pos] != '}') ++pos;
            std::string value = jsonStr.substr(val_start, pos - val_start);
            // 去除首尾空白
            value.erase(value.find_last_not_of(" \t\r\n") + 1);
            value.erase(0, value.find_first_not_of(" \t\r\n"));
            kvs[key] = value;
        }
    }
    return kvs;
}

// JsonObject实现
void JsonObject::addField(const std::string& key, std::shared_ptr<JsonValue> value) {
    fields_[key] = value;
}

std::shared_ptr<JsonValue> JsonObject::getField(const std::string& key) const {
    auto it = fields_.find(key);
    if (it != fields_.end()) {
        return it->second;
    }
    return nullptr;
}

std::string JsonObject::toString() const {
    std::stringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& field : fields_) {
        if (!first) {
            ss << ",";
        }
        ss << "\"" << field.first << "\":" << field.second->toString();
        first = false;
    }
    ss << "}";
    return ss.str();
}

// JsonArray实现
std::string JsonArray::toString() const {
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < values_.size(); ++i) {
        if (i > 0) {
            ss << ",";
        }
        ss << values_[i]->toString();
    }
    ss << "]";
    return ss.str();
}

// JsonParser实现
std::shared_ptr<JsonObject> JsonParser::parse(const std::string& jsonStr) {
    size_t pos = 0;
    skipWhitespace(jsonStr, pos);
    
    if (pos >= jsonStr.length() || jsonStr[pos] != '{') {
        throw std::runtime_error("Invalid JSON: expected '{' at start of object");
    }
    
    auto obj = std::make_shared<JsonObject>();
    pos++; // 跳过 '{'
    
    skipWhitespace(jsonStr, pos);
    if (jsonStr[pos] == '}') {
        return obj; // 空对象
    }
    
    while (pos < jsonStr.length()) {
        // 解析键
        if (jsonStr[pos] != '"') {
            throw std::runtime_error("Invalid JSON: expected '\"' at start of key");
        }
        std::string key = parseString(jsonStr, pos);
        
        // 解析冒号
        skipWhitespace(jsonStr, pos);
        if (pos >= jsonStr.length() || jsonStr[pos] != ':') {
            throw std::runtime_error("Invalid JSON: expected ':' after key");
        }
        pos++;
        
        // 解析值
        skipWhitespace(jsonStr, pos);
        auto value = parseValue(jsonStr, pos);
        obj->addField(key, value);
        
        // 检查是否继续
        skipWhitespace(jsonStr, pos);
        if (pos >= jsonStr.length()) {
            throw std::runtime_error("Invalid JSON: unexpected end of input");
        }
        
        if (jsonStr[pos] == '}') {
            pos++;
            break;
        } else if (jsonStr[pos] == ',') {
            pos++;
            skipWhitespace(jsonStr, pos);
        } else {
            throw std::runtime_error("Invalid JSON: expected ',' or '}'");
        }
    }
    
    return obj;
}

std::vector<std::shared_ptr<JsonObject>> JsonParser::parseLogFile(const std::string& filePath) {
    std::vector<std::shared_ptr<JsonObject>> objects;
    std::ifstream file(filePath);
    
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filePath);
    }
    
    std::string line;
    while (std::getline(file, line)) {
        try {
            // 使用parseFlatJson来解析嵌套结构
            std::unordered_map<std::string, std::string> kvs = parseFlatJson(line);
            
            // 将扁平化的键值对转换为JsonObject
            auto obj = std::make_shared<JsonObject>();
            for (const auto& kv : kvs) {
                const std::string& key = kv.first;
                const std::string& value = kv.second;
                
                // 根据值的类型创建相应的JsonValue
                DictType type = getDictType(value);
                std::shared_ptr<JsonValue> jsonValue;
                
                switch (type) {
                    case DictType::RAW_NUMBER:
                        try {
                            double num = std::stod(value);
                            jsonValue = std::make_shared<JsonNumber>(num);
                        } catch (const std::exception&) {
                            jsonValue = std::make_shared<JsonString>(value);
                        }
                        break;
                    case DictType::RAW_BOOLEAN:
                        jsonValue = std::make_shared<JsonBoolean>(value == "true");
                        break;
                    case DictType::TIMESTAMP_DICT:
                        jsonValue = std::make_shared<JsonTimestamp>(value);
                        break;
                    case DictType::LOG_DICT:
                        jsonValue = std::make_shared<JsonLogType>(value);
                        break;
                    default:
                        jsonValue = std::make_shared<JsonString>(value);
                        break;
                }
                
                obj->addField(key, jsonValue);
            }
            
            objects.push_back(obj);
        } catch (const std::exception& e) {
            // 跳过无效的JSON行
            continue;
        }
    }
    
    return objects;
}

std::shared_ptr<JsonValue> JsonParser::parseValue(const std::string& jsonStr, size_t& pos) {
    skipWhitespace(jsonStr, pos);
    
    if (pos >= jsonStr.length()) {
        throw std::runtime_error("Invalid JSON: unexpected end of input");
    }
    
    char c = jsonStr[pos];
    switch (c) {
        case '"': {
            std::string str = parseString(jsonStr, pos);
            // 检查是否是时间戳
            if (isTimestamp(str)) {
                return std::make_shared<JsonTimestamp>(str);
            }
            // 检查是否是日志类型
            if (isLogType(str)) {
                return std::make_shared<JsonLogType>(str);
            }
            // 普通字符串
            return std::make_shared<JsonString>(str);
        }
        case '{': {
            auto obj = parse(jsonStr.substr(pos));
            pos += jsonStr.length() - pos; // 更新位置
            return obj;
        }
        case '[':
            return parseArray(jsonStr, pos);
        case 't':
            if (jsonStr.substr(pos, 4) == "true") {
                pos += 4;
                return std::make_shared<JsonBoolean>(true);
            }
            break;
        case 'f':
            if (jsonStr.substr(pos, 5) == "false") {
                pos += 5;
                return std::make_shared<JsonBoolean>(false);
            }
            break;
        case 'n':
            if (jsonStr.substr(pos, 4) == "null") {
                pos += 4;
                return std::make_shared<JsonNull>();
            }
            break;
        default:
            if (c == '-' || std::isdigit(c)) {
                return std::make_shared<JsonNumber>(parseNumber(jsonStr, pos));
            }
    }
    
    throw std::runtime_error("Invalid JSON: unexpected character in value");
}

std::string JsonParser::parseString(const std::string& jsonStr, size_t& pos) {
    if (jsonStr[pos] != '"') {
        throw std::runtime_error("Invalid JSON: expected '\"' at start of string");
    }
    
    pos++; // 跳过开始的引号
    std::string result;
    
    while (pos < jsonStr.length()) {
        char c = jsonStr[pos++];
        if (c == '"') {
            return result;
        } else if (c == '\\') {
            if (pos >= jsonStr.length()) {
                throw std::runtime_error("Invalid JSON: unexpected end of string");
            }
            c = jsonStr[pos++];
            switch (c) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: throw std::runtime_error("Invalid JSON: invalid escape sequence");
            }
        } else {
            result += c;
        }
    }
    
    throw std::runtime_error("Invalid JSON: unexpected end of string");
}

double JsonParser::parseNumber(const std::string& jsonStr, size_t& pos) {
    size_t start = pos;
    bool hasDecimal = false;
    bool hasExponent = false;
    
    // 处理负号
    if (jsonStr[pos] == '-') {
        pos++;
    }
    
    // 处理整数部分
    if (pos >= jsonStr.length() || !std::isdigit(jsonStr[pos])) {
        throw std::runtime_error("Invalid JSON: expected digit in number");
    }
    
    while (pos < jsonStr.length() && std::isdigit(jsonStr[pos])) {
        pos++;
    }
    
    // 处理小数部分
    if (pos < jsonStr.length() && jsonStr[pos] == '.') {
        hasDecimal = true;
        pos++;
        if (pos >= jsonStr.length() || !std::isdigit(jsonStr[pos])) {
            throw std::runtime_error("Invalid JSON: expected digit after decimal point");
        }
        while (pos < jsonStr.length() && std::isdigit(jsonStr[pos])) {
            pos++;
        }
    }
    
    // 处理指数部分
    if (pos < jsonStr.length() && (jsonStr[pos] == 'e' || jsonStr[pos] == 'E')) {
        hasExponent = true;
        pos++;
        if (pos < jsonStr.length() && (jsonStr[pos] == '+' || jsonStr[pos] == '-')) {
            pos++;
        }
        if (pos >= jsonStr.length() || !std::isdigit(jsonStr[pos])) {
            throw std::runtime_error("Invalid JSON: expected digit in exponent");
        }
        while (pos < jsonStr.length() && std::isdigit(jsonStr[pos])) {
            pos++;
        }
    }
    
    // 提取数字字符串并转换为double
    std::string numStr = jsonStr.substr(start, pos - start);
    try {
        return std::stod(numStr);
    } catch (const std::exception& e) {
        throw std::runtime_error("Invalid JSON: invalid number format");
    }
}

std::shared_ptr<JsonArray> JsonParser::parseArray(const std::string& jsonStr, size_t& pos) {
    if (jsonStr[pos] != '[') {
        throw std::runtime_error("Invalid JSON: expected '[' at start of array");
    }
    
    auto array = std::make_shared<JsonArray>();
    pos++; // 跳过 '['
    
    skipWhitespace(jsonStr, pos);
    if (jsonStr[pos] == ']') {
        pos++;
        return array; // 空数组
    }
    
    while (pos < jsonStr.length()) {
        auto value = parseValue(jsonStr, pos);
        array->addValue(value);
        
        skipWhitespace(jsonStr, pos);
        if (pos >= jsonStr.length()) {
            throw std::runtime_error("Invalid JSON: unexpected end of input");
        }
        
        if (jsonStr[pos] == ']') {
            pos++;
            break;
        } else if (jsonStr[pos] == ',') {
            pos++;
            skipWhitespace(jsonStr, pos);
        } else {
            throw std::runtime_error("Invalid JSON: expected ',' or ']'");
        }
    }
    
    return array;
}

void JsonParser::skipWhitespace(const std::string& jsonStr, size_t& pos) {
    while (pos < jsonStr.length() && std::isspace(jsonStr[pos])) {
        pos++;
    }
}

void JsonParser::parseAndCollect(
    const std::string& filePath,
    Dictionary& dict,
    std::vector<ParsedField>& fieldOrder
) {
    std::ifstream file(filePath);
    if (!file.is_open()) throw std::runtime_error("Cannot open file: " + filePath);

    std::string line;
    std::unordered_map<std::string, DictType> field_types;  // 用于收集所有字段及其类型
    size_t total_records = 0;  // 总记录数

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        total_records++;  // 增加记录计数
        
        std::unordered_map<std::string, std::string> kvs = parseFlatJson(line);

        // 处理每个字段
        for (const auto& kv : kvs) {
            const std::string& field_name = kv.first;
            const std::string& value = kv.second;

            // 如果是新字段，添加到字段列表中
            if (field_types.find(field_name) == field_types.end()) {
                DictType type = getDictType(value);
                field_types[field_name] = type;
                fieldOrder.emplace_back(field_name, type);
            } else {
                // 如果字段已存在，检查类型是否一致
                DictType current_type = getDictType(value);
                if (field_types[field_name] != current_type) {
                    // 类型不一致，使用更通用的类型
                    if (field_types[field_name] == DictType::RAW_NUMBER && current_type == DictType::VARIABLE_DICT) {
                        // 保持RAW_NUMBER类型
                    } else if (field_types[field_name] == DictType::VARIABLE_DICT && current_type == DictType::RAW_NUMBER) {
                        // 保持VARIABLE_DICT类型
                    } else if (field_types[field_name] == DictType::RAW_BOOLEAN && current_type == DictType::VARIABLE_DICT) {
                        // 保持RAW_BOOLEAN类型
                    } else if (field_types[field_name] == DictType::VARIABLE_DICT && current_type == DictType::RAW_BOOLEAN) {
                        // 保持VARIABLE_DICT类型
                    } else {
                        // 其他情况，使用VARIABLE_DICT作为通用类型
                        field_types[field_name] = DictType::VARIABLE_DICT;
                    }
                }
            }

            // 统计字典和字段值域
            dict.addFieldValue(field_name, value, field_types[field_name]);
        }
    }

    // 根据冗余度因子重新排序字段
    std::vector<std::string> ordered_fields = dict.getOrderedFields();
    std::vector<ParsedField> new_field_order;
    new_field_order.reserve(ordered_fields.size());
    
    for (const auto& field_name : ordered_fields) {
        auto it = std::find_if(fieldOrder.begin(), fieldOrder.end(),
            [&field_name](const ParsedField& field) { return field.name == field_name; });
        if (it != fieldOrder.end()) {
            new_field_order.push_back(*it);
        }
    }
    
    fieldOrder = std::move(new_field_order);
}

} // namespace json2
