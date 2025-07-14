#include "../include/trie.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <functional>
#include <iostream>
#include <cctype>
#include <simdjson.h>
#include "../include/variable_dictionary.h"

namespace json2 {

// TrieNode实现
TrieNode::TrieNode(const NodeValue& value, bool is_placeholder)
    : value_(value), is_placeholder_(is_placeholder) {}

TrieNode* TrieNode::getOrCreateChild(const NodeValue& value) {
    auto it = std::lower_bound(children_.begin(), children_.end(), value,
        [](const auto& pair, const NodeValue& val) {
            return pair.first < val;
        });
    if (it != children_.end() && it->first == value) {
        return it->second.get();
    }
    auto new_node = std::make_unique<TrieNode>(value, false);
    TrieNode* result = new_node.get();
    children_.insert(it, std::make_pair(value, std::move(new_node)));
    return result;
}

const std::vector<std::pair<NodeValue, std::unique_ptr<TrieNode>>>& TrieNode::getChildren() const {
    return children_;
}

const NodeValue& TrieNode::getValue() const {
    return value_;
}

bool TrieNode::isPlaceholder() const {
    return is_placeholder_;
}

void TrieNode::setPlaceholder(bool is_placeholder) {
    is_placeholder_ = is_placeholder;
}

Trie::Trie(const std::vector<std::string>& fields)
    : ordered_fields_(fields), root_(std::make_unique<TrieNode>(NodeValue(uint32_t(0)), true)) {}

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
            // 类型敏感化
            FieldType type;
            Value value;
            switch(value_node.type()) {
                case simdjson::dom::element_type::STRING:
                    type = FieldType::String;
                    value = std::string(value_node.get_string().value());
                    break;
                case simdjson::dom::element_type::INT64:
                    type = FieldType::Int;
                    value = value_node.get_int64().value();
                    break;
                case simdjson::dom::element_type::UINT64:
                    type = FieldType::Int;
                    value = static_cast<int64_t>(value_node.get_uint64().value());
                    break;
                case simdjson::dom::element_type::DOUBLE:
                    type = FieldType::Double;
                    value = value_node.get_double().value();
                    break;
                case simdjson::dom::element_type::BOOL:
                    type = FieldType::Bool;
                    value = value_node.get_bool().value();
                    break;
                case simdjson::dom::element_type::NULL_VALUE:
                    type = FieldType::Null;
                    value = nullptr;
                    break;
                default:
                    type = FieldType::String;
                    value = std::string();
                    break;
            }
            
            NodeValue node_value = createNodeValue(field, type, value, dict);
            current_node = current_node->getOrCreateChild(node_value);
        } catch (const simdjson::simdjson_error& e) {
            // 字段不存在，视为null
            FieldType type = FieldType::Null;
            NodeValue node_value = createNodeValue(field, type, nullptr, dict);
            current_node = current_node->getOrCreateChild(node_value);
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
    
    const NodeValue& value = node->getValue();
    
    // 写入值类型（1字节）
    uint8_t type_byte = 0;
    if (std::holds_alternative<uint32_t>(value)) {
        type_byte = 0; // 编码值
    } else if (std::holds_alternative<int64_t>(value)) {
        type_byte = 1; // 整数
    } else if (std::holds_alternative<double>(value)) {
        type_byte = 2; // 浮点
    } else if (std::holds_alternative<bool>(value)) {
        type_byte = 3; // 布尔
    }
    data.push_back(type_byte);
    
    // 写入值（使用varint或固定长度）
    switch (type_byte) {
        case 0: { // uint32_t
            uint32_t val = std::get<uint32_t>(value);
            while (val >= 0x80) {
                data.push_back(static_cast<uint8_t>(val | 0x80));
                val >>= 7;
            }
            data.push_back(static_cast<uint8_t>(val));
            break;
        }
        case 1: { // int64_t
            int64_t val = std::get<int64_t>(value);
            // 使用zigzag编码
            uint64_t zigzag = (val << 1) ^ (val >> 63);
            while (zigzag >= 0x80) {
                data.push_back(static_cast<uint8_t>(zigzag | 0x80));
                zigzag >>= 7;
            }
            data.push_back(static_cast<uint8_t>(zigzag));
            break;
        }
        case 2: { // double
            double val = std::get<double>(value);
            // 直接写入8字节
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&val);
            data.insert(data.end(), bytes, bytes + 8);
            break;
        }
        case 3: { // bool
            bool val = std::get<bool>(value);
            data.push_back(val ? 1 : 0);
            break;
        }
    }
    
    // 写入占位标志（1字节）
    data.push_back(node->isPlaceholder() ? 1 : 0);
    
    // 收集并排序子节点
    std::vector<std::pair<NodeValue, const TrieNode*>> children;
    for (const auto& [val, child] : node->getChildren()) {
        children.emplace_back(val, child.get());
    }
    std::sort(children.begin(), children.end());
    
    // 写入子节点数量（使用varint）
    uint32_t child_count = children.size();
    uint32_t val = child_count;
    while (val >= 0x80) {
        data.push_back(static_cast<uint8_t>(val | 0x80));
        val >>= 7;
    }
    data.push_back(static_cast<uint8_t>(val));
    
    // 递归序列化子节点
    for (const auto& [val, child] : children) {
        serializeNode(child, data);
    }
}

TrieNode* Trie::deserializeNode(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos >= data.size()) return nullptr;
    
    // 读取值类型
    uint8_t type_byte = data[pos++];
    
    // 读取值
    NodeValue value;
    switch (type_byte) {
        case 0: { // uint32_t
            uint32_t code = 0, shift = 0;
            while (pos < data.size()) {
                uint8_t byte = data[pos++];
                code |= static_cast<uint32_t>(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0) break;
                shift += 7;
            }
            value = NodeValue(code);
            break;
        }
        case 1: { // int64_t
            uint64_t zigzag = 0, shift = 0;
            while (pos < data.size()) {
                uint8_t byte = data[pos++];
                zigzag |= static_cast<uint64_t>(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0) break;
                shift += 7;
            }
            // 解码zigzag
            int64_t val = static_cast<int64_t>((zigzag >> 1) ^ (-(zigzag & 1)));
            value = NodeValue(val);
            break;
        }
        case 2: { // double
            if (pos + 8 > data.size()) return nullptr;
            double val;
            std::memcpy(&val, &data[pos], 8);
            pos += 8;
            value = NodeValue(val);
            break;
        }
        case 3: { // bool
            if (pos >= data.size()) return nullptr;
            bool val = (data[pos++] == 1);
            value = NodeValue(val);
            break;
        }
        default:
            return nullptr;
    }
    
    // 读取占位标志
    if (pos >= data.size()) return nullptr;
    bool is_placeholder = (data[pos++] == 1);
    
    auto node = std::make_unique<TrieNode>(value, is_placeholder);
    
    // 读取子节点数量
    uint32_t child_count = 0, shift = 0;
    while (pos < data.size()) {
        uint8_t byte = data[pos++];
        child_count |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    
    for (uint32_t i = 0; i < child_count; ++i) {
        TrieNode* child = deserializeNode(data, pos);
        if (child) {
            TrieNode* new_child = node->getOrCreateChild(child->getValue());
            copyChildren(child, new_child);
            delete child;
        }
    }
    
    return node.release();
}

void Trie::copyChildren(const TrieNode* src, TrieNode* dest) {
    for (const auto& [val, child] : src->getChildren()) {
        TrieNode* new_child = dest->getOrCreateChild(val);
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

NodeValue Trie::createNodeValue(const std::string& field, FieldType type, const Value& value, Dictionary& dict) {
    switch (type) {
        case FieldType::String:
        case FieldType::Timestamp:
        case FieldType::LogType:
        case FieldType::Null:
            // 字符串类型使用字典编码
            return NodeValue(dict.getOrAddFieldValue(field, type, value));
        case FieldType::Int:
            // 整数类型直接存储原始值
            return NodeValue(std::get<int64_t>(value));
        case FieldType::Double:
            // 浮点类型直接存储原始值
            return NodeValue(std::get<double>(value));
        case FieldType::Bool:
            // 布尔类型直接存储原始值
            return NodeValue(std::get<bool>(value));
        default:
            // 默认使用字典编码
            return NodeValue(dict.getOrAddFieldValue(field, type, value));
    }
}

std::string Trie::reconstructFieldValue(const std::string& field, FieldType type, const NodeValue& node_value, const Dictionary& dict) const {
    switch (type) {
        case FieldType::String:
        case FieldType::Timestamp:
        case FieldType::LogType:
        case FieldType::Null:
            // 从字典编码重建字符串值
            {
                uint32_t code = std::get<uint32_t>(node_value);
                auto opt_value = dict.getFieldValueByCode(field, type, code);
                if (opt_value) {
                    if (std::holds_alternative<std::string>(*opt_value)) {
                        return std::get<std::string>(*opt_value);
                    } else if (std::holds_alternative<std::nullptr_t>(*opt_value)) {
                        return "null";
                    }
                }
                return "";
            }
        case FieldType::Int:
            // 直接返回整数值
            return std::to_string(std::get<int64_t>(node_value));
        case FieldType::Double:
            // 直接返回浮点值
            return std::to_string(std::get<double>(node_value));
        case FieldType::Bool:
            // 直接返回布尔值
            return std::get<bool>(node_value) ? "true" : "false";
        default:
            return "";
    }
}

} // namespace json2
