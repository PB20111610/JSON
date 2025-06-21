#include "../include/reconstruct.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>
#include <string>

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

std::string reconstructJsonFromTrie(const Trie& trie, const Dictionary& dict) {
    using nlohmann::json;
    std::vector<json> records;
    const auto& ordered_fields = trie.getOrderedFields();
    std::function<void(const TrieNode*, std::vector<std::pair<std::string, std::string>>, size_t)> traverse =
        [&](const TrieNode* node, std::vector<std::pair<std::string, std::string>> currentRecord, size_t depth) {
        if (!node) return;
        if (depth > 0 && !node->isPlaceholder()) {
            std::string fieldName = ordered_fields[depth - 1];
            std::string value = dict.getFieldValueByCode(fieldName, node->getCode());
            currentRecord.push_back({fieldName, value});
        }
        if (node->getChildren().empty() && !currentRecord.empty()) {
            json j;
            for (const auto& [k, v] : currentRecord) {
                set_nested(j, k, parseValue(v));
            }
            records.push_back(j);
        }
        for (const auto& [code, child] : node->getChildren()) {
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