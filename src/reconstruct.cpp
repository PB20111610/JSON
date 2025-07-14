#include "../include/reconstruct.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>
#include <string>
#include <variant>
#include "timestamp_dictionary.h"
#include "trie.h"
#include "field_dictionary_manager.h"

namespace json2 {

// --- 以下为辅助函数 ---
static bool isInteger(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') ++i;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(s[i])) return false;
    }
    if (s.size() > 1 && s[0] == '0' && std::isdigit(s[1])) return false;
    if (s.size() > 2 && (s[0] == '-' || s[0] == '+') && s[1] == '0' && std::isdigit(s[2])) return false;
    return true;
}
static bool isDouble(const std::string& s) {
    if (s.empty()) return false;
    char* endptr = nullptr;
    double val = std::strtod(s.c_str(), &endptr);
    if (endptr == s.c_str() || *endptr != '\0') return false;
    if (s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
        size_t dot = s.find('.');
        if (dot != std::string::npos && dot > 0 && s[0] == '0' && dot > 1) return false;
        if (dot == std::string::npos && s.size() > 1 && s[0] == '0') return false;
    }
    return true;
}
static bool isBool(const std::string& s) {
    return s == "true" || s == "false";
}
static nlohmann::json parseValue(const std::string& s) {
    if (isBool(s)) return s == "true";
    if (isInteger(s)) return std::stoll(s);
    if (isDouble(s)) return std::stod(s);
    return s;
}
static void set_nested(nlohmann::json& j, const std::string& flat_key, const nlohmann::json& value) {
    size_t pos = 0, next;
    nlohmann::json* curr = &j;
    while ((next = flat_key.find('.', pos)) != std::string::npos) {
        std::string key = flat_key.substr(pos, next - pos);
        curr = &(*curr)[key];
        pos = next + 1;
    }
    (*curr)[flat_key.substr(pos)] = value;
}

static std::string epochToString(int64_t epoch, const std::string& fmt) {
    struct tm tm = *gmtime(&epoch);
    char buf[64];
    strftime(buf, sizeof(buf), fmt.c_str(), &tm);
    return std::string(buf);
}

std::string reconstructJsonFromTrie(const Trie& trie, const FieldDictionaryManager& manager) {
    using nlohmann::json;
    std::vector<json> records;
    const auto& ordered_fields = trie.getOrderedFields();
    const auto& all_fields_and_types = manager.getAllFieldsAndTypes();
    
    // 构建字段类型映射
    std::unordered_map<std::string, FieldType> field_types;
    for (const auto& [field, type] : all_fields_and_types) {
        field_types[field] = type;
    }
    
    std::function<void(const TrieNode*, std::vector<std::pair<std::string, std::string>>, size_t)> traverse =
        [&](const TrieNode* node, std::vector<std::pair<std::string, std::string>> currentRecord, size_t depth) {
        if (!node) return;
        if (depth > 0 && !node->isPlaceholder()) {
            std::string fieldName = ordered_fields[depth - 1];
            const NodeValue& node_value = node->getValue();
            
            // 获取字段类型
            FieldType fieldType = FieldType::String; // 默认类型
            auto type_it = field_types.find(fieldName);
            if (type_it != field_types.end()) {
                fieldType = type_it->second;
            }
            
            // 根据NodeValue的类型和字段类型来决定如何重建值
            std::string value;
            if (std::holds_alternative<uint32_t>(node_value)) {
                // 这是字典编码值，需要解码
                uint32_t code = std::get<uint32_t>(node_value);
                
                // 使用确切的字段类型进行解码
                const Dictionary& dict = manager.variableDict();
                auto opt_value = dict.getFieldValueByCode(fieldName, fieldType, code);
                if (opt_value) {
                    if (std::holds_alternative<std::string>(*opt_value)) {
                        value = std::get<std::string>(*opt_value);
                    } else if (std::holds_alternative<std::nullptr_t>(*opt_value)) {
                        value = "null";
                    } else {
                        // 其他类型转换为字符串
                        value = "UNKNOWN_TYPE";
                    }
                } else {
                    // 解码失败
                    value = "DECODE_ERROR_" + std::to_string(code);
                }
                
            } else if (std::holds_alternative<int64_t>(node_value)) {
                // 直接返回整数值（不需要解码）
                value = std::to_string(std::get<int64_t>(node_value));
            } else if (std::holds_alternative<double>(node_value)) {
                // 直接返回浮点值（不需要解码）
                value = std::to_string(std::get<double>(node_value));
            } else if (std::holds_alternative<bool>(node_value)) {
                // 直接返回布尔值（不需要解码）
                value = std::get<bool>(node_value) ? "true" : "false";
            }
            
            currentRecord.push_back({fieldName, value});
        }
        if (node->getChildren().empty() && !currentRecord.empty()) {
            json j;
            for (const auto& [k, v] : currentRecord) {
                set_nested(j, k, parseValue(v));
            }
            records.push_back(j);
        }
        for (const auto& [val, child] : node->getChildren()) {
            traverse(child.get(), currentRecord, depth + 1);
        }
    };
    traverse(trie.getRoot(), {}, 0);
    std::stringstream ss;
    for (size_t i = 0; i < records.size(); ++i) {
        if (i > 0) ss << "\n";
        ss << records[i].dump();
    }
    return ss.str();
}

} // namespace json2 