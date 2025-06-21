#include "../include/trie.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <functional>
#include <iostream>
#include <cctype>
#include <simdjson.h>

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

// NEW: Helper function using simdjson to find a field by flattened path.
static simdjson::dom::element getJsonFieldSimd(simdjson::dom::element node, const std::string& field) {
    simdjson::dom::element current = node;
    size_t start = 0;
    size_t end = field.find('.');
    
    while (start < field.length()) {
        std::string part = field.substr(start, end - start);
        size_t bracket_pos = part.find('[');

        if (bracket_pos != std::string::npos) {
            // Handle array access like "field[index]"
            std::string key = part.substr(0, bracket_pos);
            if (!key.empty()) {
                current = current[key];
            }
            size_t end_bracket_pos = part.find(']', bracket_pos);
            int index = std::stoi(part.substr(bracket_pos + 1, end_bracket_pos - bracket_pos - 1));
            current = current.at(index);
        } else {
            // Handle object access
            current = current[part.c_str()];
        }
        
        if (end == std::string::npos) {
            break;
        }
        
        start = end + 1;
        end = field.find('.', start);
    }
    return current;
}

void Trie::insert(const std::string& record_string, Dictionary& dict, simdjson::dom::parser& parser) {
    simdjson::dom::element record;
    auto error = parser.parse(record_string).get(record);
    if (error) { 
        // Silently ignore parse errors for now in this high-throughput path
        return; 
    }

    TrieNode* current_node = root_.get();
    for (const auto& field : ordered_fields_) {
        try {
            simdjson::dom::element value_node = getJsonFieldSimd(record, field);
            std::string value_str;
            switch(value_node.type()) {
                case simdjson::dom::element_type::STRING:
                    value_str = std::string(value_node.get_string().value());
                    break;
                case simdjson::dom::element_type::INT64:
                    value_str = std::to_string(value_node.get_int64().value());
                    break;
                case simdjson::dom::element_type::UINT64:
                    value_str = std::to_string(value_node.get_uint64().value());
                    break;
                case simdjson::dom::element_type::DOUBLE: {
                    std::ostringstream oss;
                    oss << std::setprecision(17) << value_node.get_double().value();
                    value_str = oss.str();
                    break;
                }
                case simdjson::dom::element_type::BOOL:
                    value_str = value_node.get_bool().value() ? "true" : "false";
                    break;
                case simdjson::dom::element_type::NULL_VALUE:
                    value_str = "null";
                    break;
                default:
                    // Should not happen for valid JSON values
                    break;
            }
            uint32_t code = dict.getOrAddFieldValue(field, value_str);
            current_node = current_node->getOrCreateChild(code);

        } catch (const simdjson::simdjson_error& e) {
            // This happens if a field does not exist (e.g., trying to access a key in an object that is not there)
            // We treat this as a placeholder/null value.
            uint32_t code = dict.getOrAddFieldValue(field, "null"); // Or a special placeholder value
            current_node = current_node->getOrCreateChild(code);
            current_node->setPlaceholder(true);
        }
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

} // namespace json2
