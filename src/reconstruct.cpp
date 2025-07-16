#include "../include/reconstruct.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>
#include <string>
#include <variant>
#include "timestamp_dictionary.h"
#include "trie.h"
#include "field_dictionary_manager.h"
#include <iostream> // Added for debugging output
#include <set> // Added for std::set

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
    size_t field_count = ordered_fields.size();
    
    // 递归遍历Trie，按顺序重建每条记录
    std::function<void(const TrieNode*, std::vector<std::pair<FieldKey, std::string>>&, size_t)> traverse;
    traverse = [&](const TrieNode* node, std::vector<std::pair<FieldKey, std::string>>& currentRecord, size_t depth) {
        if (!node) return;
        const auto& path = node->getPath();
        for (size_t i = 0; i < path.size(); ++i) {
            if (depth + i >= field_count) break;
            const auto& key = ordered_fields[depth + i];
            const auto& node_value = path[i];
            // 跳过 nullptr
            if (std::holds_alternative<std::nullptr_t>(node_value)) continue;
            std::string value = trie.reconstructFieldValue(key, node_value, manager);
            currentRecord.emplace_back(key, value);
        }
        if (node->getChildren().empty() && !currentRecord.empty()) {
            // 构建json对象，只包含实际存在的字段
            json j;
            std::set<std::string> output_names;
            for (const auto& [key, val] : currentRecord) {
                // 只输出第一个有值的同名字段（类型敏感）
                if (output_names.count(key.name)) continue;
                set_nested(j, key.name, parseValue(val));
                output_names.insert(key.name);
            }
            records.push_back(j);
        }
        for (const auto& child : node->getChildren()) {
            std::vector<std::pair<FieldKey, std::string>> nextRecord = currentRecord;
            traverse(child.get(), nextRecord, depth + path.size());
        }
    };
    std::vector<std::pair<FieldKey, std::string>> record;
    traverse(trie.getRoot(), record, 0);
    std::stringstream ss;
    for (size_t i = 0; i < records.size(); ++i) {
        if (i > 0) ss << "\n";
        ss << records[i].dump();
    }
    return ss.str();
}

} // namespace json2 