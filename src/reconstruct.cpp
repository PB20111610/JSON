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
static nlohmann::json parseValue(const std::string& s, FieldType type = FieldType::STRING) {
    if (isBool(s)) return s == "true";
    if (isInteger(s)) return std::stoll(s);
    if (isDouble(s)) return std::stod(s);
    
    // 检查是否为非结构化数组字符串
    if (type == FieldType::ARRAY || 
        (s.length() >= 2 && s[0] == '[' && s[s.length()-1] == ']')) {
        try {
            return nlohmann::json::parse(s);
        } catch (const std::exception& e) {
            // 如果解析失败，作为普通字符串处理
            return s;
        }
    }
    
    return s;
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
    
    // 调试输出：重建时的字段序列
    // std::cerr << "[DEBUG] 重建时字段序列:" << std::endl;
    // for (size_t i = 0; i < ordered_fields.size(); ++i) {
    //     std::cerr << "  [" << i << "] " << ordered_fields[i].name << " (type=" << static_cast<int>(ordered_fields[i].type) << ")" << std::endl;
    // }
    
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
            // std::cerr << "[DEBUG] 重构字段: " << key.name << " = " << value << std::endl;
            currentRecord.emplace_back(key, value);
        }
        if (node->getChildren().empty() && !currentRecord.empty()) {
            // 构建json对象，只包含实际存在的字段
            json j;
            std::set<std::string> output_names;
            for (const auto& [key, val] : currentRecord) {
                            // 跳过空值
            if (val.empty()) {
                // std::cerr << "[DEBUG] 跳过空值字段: " << key.name << std::endl;
                continue;
            }
                
                // 检查是否为嵌套字段（以 ~ 开头）或结构化数组字段（包含 [）
                if (manager.isNestedField(key.name)) {
                    // 嵌套字段：去掉 ~ 前缀后按点号分割并创建嵌套结构
                    std::string key_without_prefix = key.name.substr(1); // 去掉 ~ 前缀
                    size_t pos = 0, next;
                    nlohmann::json* curr = &j;
                    while ((next = key_without_prefix.find('.', pos)) != std::string::npos) {
                        std::string field_key = key_without_prefix.substr(pos, next - pos);
                        curr = &(*curr)[field_key];
                        pos = next + 1;
                    }
                    (*curr)[key_without_prefix.substr(pos)] = parseValue(val);
                } else if (key.name.find('[') != std::string::npos) {
                    // 结构化数组字段：解析数组索引并构建数组
                    size_t bracket_pos = key.name.find('[');
                    std::string array_name = key.name.substr(0, bracket_pos);
                    size_t end_bracket_pos = key.name.find(']', bracket_pos);
                    int index = std::stoi(key.name.substr(bracket_pos + 1, end_bracket_pos - bracket_pos - 1));
                    // 确保数组存在
                    if (!j.contains(array_name)) {
                        j[array_name] = nlohmann::json::array();
                    }
                    // 扩展数组到所需大小
                    while (j[array_name].size() <= index) {
                        j[array_name].push_back(nlohmann::json::value_t::null);
                    }
                    // 设置数组元素
                    j[array_name][index] = parseValue(val, key.type);
                } else {
                    // 扁平字段：只输出第一个有值的同名字段（类型敏感）
                    if (output_names.count(key.name)) continue;
                    j[key.name] = parseValue(val, key.type);
                    output_names.insert(key.name);
                }
            }
            records.push_back(j);
        }
        for (const auto& child_pair : node->getChildren()) {
            std::vector<std::pair<FieldKey, std::string>> nextRecord = currentRecord;
            traverse(child_pair.second.get(), nextRecord, depth + path.size());
        }
    };
    std::vector<std::pair<FieldKey, std::string>> record;
    traverse(trie.getRoot(), record, 0);
    std::stringstream ss;
    for (size_t i = 0; i < records.size(); ++i) {
        if (i > 0) ss << "\n";
        ss << records[i].dump(-1);
    }
    return ss.str();
}

} // namespace json2 