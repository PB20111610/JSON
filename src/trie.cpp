#include "../include/trie.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <cctype>
using nlohmann::json;

namespace json2 {

// TrieNode实现
TrieNode::TrieNode(uint32_t code, bool is_placeholder)
    : code_(code), is_placeholder_(is_placeholder) {}

TrieNode* TrieNode::getOrCreateChild(uint32_t code) {
    auto it = std::lower_bound(children_.begin(), children_.end(), code,
        [](const auto& pair, uint32_t value) {
            return pair.first < value;
        });
    if (it != children_.end() && it->first == code) {
        return it->second.get();
    }
    auto new_node = std::make_unique<TrieNode>(code, false);
    TrieNode* result = new_node.get();
    children_.insert(it, std::make_pair(code, std::move(new_node)));
    return result;
}

const std::vector<std::pair<uint32_t, std::unique_ptr<TrieNode>>>& TrieNode::getChildren() const {
    return children_;
}

uint32_t TrieNode::getCode() const {
    return code_;
}

bool TrieNode::isPlaceholder() const {
    return is_placeholder_;
}

void TrieNode::setPlaceholder(bool is_placeholder) {
    is_placeholder_ = is_placeholder;
}

Trie::Trie(const std::vector<std::string>& fields)
    : ordered_fields_(fields), root_(std::make_unique<TrieNode>(0, true)) {}

// 辅助函数：递归查找扁平字段名（支持嵌套和数组）
static const json* getJsonField(const json& j, const std::string& field) {
    size_t pos = 0, next;
    const json* current = &j;
    while (pos < field.size()) {
        next = field.find('.', pos);
        std::string key = field.substr(pos, next - pos);
        // 处理数组下标
        size_t arr_pos = key.find('[');
        if (arr_pos != std::string::npos) {
            std::string arr_key = key.substr(0, arr_pos);
            size_t arr_end = key.find(']', arr_pos);
            int idx = std::stoi(key.substr(arr_pos + 1, arr_end - arr_pos - 1));
            if (!arr_key.empty()) {
                if (!current->contains(arr_key)) return nullptr;
                current = &(*current)[arr_key];
            }
            if (!current->is_array() || idx >= current->size()) return nullptr;
            current = &(*current)[idx];
        } else {
            if (!current->contains(key)) return nullptr;
            current = &(*current)[key];
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return current;
}

void Trie::insert(const nlohmann::json& record, Dictionary& dict) {
    TrieNode* current = root_.get();
    for (const auto& field : ordered_fields_) {
        const json* value_ptr = getJsonField(record, field);
        if (!value_ptr || value_ptr->is_null()) {
            current = current->getOrCreateChild(0);
            current->setPlaceholder(true);
            continue;
        }
        std::string value = value_ptr->is_string() ? value_ptr->get<std::string>() : value_ptr->dump();
        uint32_t code = dict.getOrAddFieldValue(field, value);
        current = current->getOrCreateChild(code);
    }
}

std::vector<uint8_t> Trie::serialize() const {
    std::vector<uint8_t> result;
    serializeNode(root_.get(), result);
    return result;
}

void Trie::serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const {
    if (!node) return;
    // 写入节点编码（使用varint）
    uint32_t value = node->getCode();
    while (value >= 0x80) {
        data.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    data.push_back(static_cast<uint8_t>(value));
    // 写入占位标志（1字节）
    data.push_back(node->isPlaceholder() ? 1 : 0);
    // 收集并排序子节点
    std::vector<std::pair<uint32_t, const TrieNode*>> children;
    for (const auto& [code, child] : node->getChildren()) {
        children.emplace_back(code, child.get());
    }
    std::sort(children.begin(), children.end());
    // 写入子节点数量（使用varint）
    uint32_t child_count = children.size();
    value = child_count;
    while (value >= 0x80) {
        data.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    data.push_back(static_cast<uint8_t>(value));
    // 递归序列化子节点
    for (const auto& [code, child] : children) {
        serializeNode(child, data);
    }
}

TrieNode* Trie::deserializeNode(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos >= data.size()) return nullptr;
    // 读取节点编码
    uint32_t code = 0, shift = 0;
    while (pos < data.size()) {
        uint8_t byte = data[pos++];
        code |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    // 读取占位标志
    if (pos >= data.size()) return nullptr;
    bool is_placeholder = (data[pos++] == 1);
    auto node = std::make_unique<TrieNode>(code, is_placeholder);
    // 读取子节点数量
    uint32_t child_count = 0; shift = 0;
    while (pos < data.size()) {
        uint8_t byte = data[pos++];
        child_count |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    for (uint32_t i = 0; i < child_count; ++i) {
        TrieNode* child = deserializeNode(data, pos);
        if (child) {
            TrieNode* new_child = node->getOrCreateChild(child->getCode());
            copyChildren(child, new_child);
            delete child;
        }
    }
    return node.release();
}

void Trie::copyChildren(const TrieNode* src, TrieNode* dest) {
    for (const auto& [code, child] : src->getChildren()) {
        TrieNode* new_child = dest->getOrCreateChild(code);
        copyChildren(child.get(), new_child);
    }
}

void Trie::deserialize(const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    size_t pos = 0;
    root_.reset(deserializeNode(data, pos));
}

const std::vector<std::string>& Trie::getOrderedFields() const {
    return ordered_fields_;
}

void Trie::setOrderedFields(const std::vector<std::string>& fields) {
    ordered_fields_ = fields;
}

// 辅助函数：判断字符串是否为整数
static bool isInteger(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') ++i;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i) {
        if (!std::isdigit(s[i])) return false;
    }
    // 不允许前导零（除非就是"0"）
    if (s.size() > 1 && s[0] == '0' && std::isdigit(s[1])) return false;
    if (s.size() > 2 && (s[0] == '-' || s[0] == '+') && s[1] == '0' && std::isdigit(s[2])) return false;
    return true;
}

// 辅助函数：判断字符串是否为浮点数
static bool isDouble(const std::string& s) {
    if (s.empty()) return false;
    char* endptr = nullptr;
    double val = std::strtod(s.c_str(), &endptr);
    if (endptr == s.c_str() || *endptr != '\0') return false;
    // 排除科学计数法以外的前导零
    if (s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
        size_t dot = s.find('.');
        if (dot != std::string::npos && dot > 0 && s[0] == '0' && dot > 1) return false;
        if (dot == std::string::npos && s.size() > 1 && s[0] == '0') return false;
    }
    return true;
}

// 辅助函数：判断字符串是否为布尔值
static bool isBool(const std::string& s) {
    return s == "true" || s == "false";
}

// 辅助函数：将字符串转为合适的json类型
static nlohmann::json parseValue(const std::string& s) {
    if (isBool(s)) return s == "true";
    if (isInteger(s)) return std::stoll(s);
    if (isDouble(s)) return std::stod(s);
    return s;
}

// 辅助函数：将扁平字段名嵌套赋值到json对象
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

std::string Trie::toJson(const Dictionary& dict) const {
    std::vector<nlohmann::json> records;
    std::function<void(const TrieNode*, std::vector<std::pair<std::string, std::string>>, size_t)> traverse =
        [&](const TrieNode* node, std::vector<std::pair<std::string, std::string>> currentRecord, size_t depth) {
        if (!node) return;
        if (depth > 0 && !node->isPlaceholder()) {
            std::string fieldName = ordered_fields_[depth - 1];
            std::string value = dict.getFieldValueByCode(fieldName, node->getCode());
            currentRecord.push_back({fieldName, value});
        }
        if (node->getChildren().empty() && !currentRecord.empty()) {
            nlohmann::json j;
            for (const auto& [k, v] : currentRecord) {
                set_nested(j, k, parseValue(v));
            }
            records.push_back(j);
        }
        for (const auto& [code, child] : node->getChildren()) {
            traverse(child.get(), currentRecord, depth + 1);
        }
    };
    traverse(root_.get(), {}, 0);
    std::stringstream ss;
    for (size_t i = 0; i < records.size(); ++i) {
        if (i > 0) ss << "\n";
        ss << records[i].dump();
    }
    return ss.str();
}

} // namespace json2
