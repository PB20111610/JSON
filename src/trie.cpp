#include "../include/trie.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iostream>
#include <simdjson.h>
#include "../include/variable_dictionary.h"

namespace json2 {

// TrieNode实现
TrieNode::TrieNode(const std::vector<NodeValue>& path, bool is_placeholder)
    : path_(path), is_placeholder_(is_placeholder) {}

const std::vector<NodeValue>& TrieNode::getPath() const {
    return path_;
}

bool TrieNode::isPlaceholder() const {
    return is_placeholder_;
}

void TrieNode::setPlaceholder(bool is_placeholder) {
    is_placeholder_ = is_placeholder;
}

const std::vector<std::unique_ptr<TrieNode>>& TrieNode::getChildren() const {
    return children_;
}

std::vector<std::unique_ptr<TrieNode>>& TrieNode::getChildren() {
    return children_;
}

// Trie实现
Trie::Trie(const std::vector<FieldKey>& fields)
    : root_(std::make_unique<TrieNode>(std::vector<NodeValue>{}, true)), ordered_fields_(fields) {}

// 辅助：simdjson按路径取字段
static simdjson::dom::element getJsonFieldSimd(simdjson::dom::element node, const std::string& field) {
    simdjson::dom::element current = node;
    size_t start = 0;
    size_t end = field.find('.');
    while (start < field.length()) {
        std::string part = field.substr(start, end - start);
        size_t bracket_pos = part.find('[');
        if (bracket_pos != std::string::npos) {
            std::string key = part.substr(0, bracket_pos);
            if (!key.empty()) {
                current = current[key];
            }
            size_t end_bracket_pos = part.find(']', bracket_pos);
            int index = std::stoi(part.substr(bracket_pos + 1, end_bracket_pos - bracket_pos - 1));
            current = current.at(index);
        } else {
            current = current[part.c_str()];
        }
        if (end == std::string::npos) break;
        start = end + 1;
        end = field.find('.', start);
    }
    return current;
}

// 标准Trie插入（每层一个字段）
void Trie::insert(const std::string& record_string, FieldDictionaryManager& manager, simdjson::dom::parser& parser) {
    simdjson::dom::element record;
    auto error = parser.parse(record_string).get(record);
    if (error) return;
    std::vector<NodeValue> values;
    for (const auto& key : ordered_fields_) {
        try {
            simdjson::dom::element value_node = getJsonFieldSimd(record, key.name);
            Value value;
            switch(value_node.type()) {
                case simdjson::dom::element_type::STRING:
                    value = std::string(value_node.get_string().value());
                    break;
                case simdjson::dom::element_type::INT64:
                    value = value_node.get_int64().value();
                    break;
                case simdjson::dom::element_type::UINT64:
                    value = static_cast<int64_t>(value_node.get_uint64().value());
                    break;
                case simdjson::dom::element_type::DOUBLE:
                    value = value_node.get_double().value();
                    break;
                case simdjson::dom::element_type::BOOL:
                    value = value_node.get_bool().value();
                    break;
                case simdjson::dom::element_type::NULL_VALUE:
                    value = nullptr;
                    break;
                default:
                    value = std::string();
                    break;
            }
            NodeValue node_value = createNodeValue(key, value, manager);
            values.push_back(node_value);
        } catch (const simdjson::simdjson_error& e) {
            // 字段不存在时，插入nullptr占位
            NodeValue node_value = createNodeValue(key, nullptr, manager);
            values.push_back(node_value);
        }
    }
    // 标准Trie插入
    TrieNode* cur = root_.get();
    for (size_t i = 0; i < values.size(); ++i) {
        NodeValue& v = values[i];
        bool found = false;
        for (auto& child : cur->getChildren()) {
            if (child->getPath().size() == 1 && child->getPath()[0] == v) {
                cur = child.get();
                found = true;
                break;
            }
        }
        if (!found) {
            auto new_node = std::make_unique<TrieNode>(std::vector<NodeValue>{v}, false);
            TrieNode* new_ptr = new_node.get();
            cur->getChildren().push_back(std::move(new_node));
            cur = new_ptr;
        }
    }
    cur->setPlaceholder(true);
}

// 批量路径压缩递归实现
static void compressTrieNode(TrieNode* node) {
    while (node->getChildren().size() == 1) { // 允许占位节点也参与压缩
        TrieNode* child = node->getChildren()[0].get();
        // 合并child的path到node
        auto& node_path = const_cast<std::vector<NodeValue>&>(node->getPath());
        node_path.insert(node_path.end(), child->getPath().begin(), child->getPath().end());
        // 继承child的children和占位
        node->getChildren() = std::move(const_cast<std::vector<std::unique_ptr<TrieNode>>&>(child->getChildren()));
        node->setPlaceholder(child->isPlaceholder());
        // child节点析构
    }
    for (auto& child : node->getChildren()) {
        compressTrieNode(child.get());
    }
}

void Trie::compressPaths() {
    for (auto& child : root_->getChildren()) {
        compressTrieNode(child.get());
    }
}

// Trie序列化/反序列化
void Trie::serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const {
    // 写入path长度
    uint32_t path_len = static_cast<uint32_t>(node->getPath().size());
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&path_len), reinterpret_cast<uint8_t*>(&path_len) + sizeof(path_len));
    // 写入path内容
    for (size_t idx = 0; idx < node->getPath().size(); ++idx) {
        const auto& val = node->getPath()[idx];
        uint8_t type_byte = 0;
        if (std::holds_alternative<uint32_t>(val)) type_byte = 0;
        else if (std::holds_alternative<int64_t>(val)) type_byte = 1;
        else if (std::holds_alternative<double>(val)) type_byte = 2;
        else if (std::holds_alternative<bool>(val)) type_byte = 3;
        else if (std::holds_alternative<EncodedTimestamp>(val)) type_byte = 4;
        else if (std::holds_alternative<EncodedLog>(val)) type_byte = 5;
        data.push_back(type_byte);
        switch (type_byte) {
            case 0: {
                uint32_t v = std::get<uint32_t>(val);
                data.insert(data.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + sizeof(v));
                break;
            }
            case 1: {
                int64_t v = std::get<int64_t>(val);
                data.insert(data.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + sizeof(v));
                break;
            }
            case 2: {
                double v = std::get<double>(val);
                data.insert(data.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + sizeof(v));
                break;
            }
            case 3: {
                bool v = std::get<bool>(val);
                data.push_back(v ? 1 : 0);
                break;
            }
            case 4: { // EncodedTimestamp
                const auto& ts = std::get<EncodedTimestamp>(val);
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&ts.pattern_id), reinterpret_cast<const uint8_t*>(&ts.pattern_id) + sizeof(ts.pattern_id));
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&ts.epoch), reinterpret_cast<const uint8_t*>(&ts.epoch) + sizeof(ts.epoch));
                break;
            }
            case 5: { // EncodedLog
                const auto& log = std::get<EncodedLog>(val);
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&log.template_id), reinterpret_cast<const uint8_t*>(&log.template_id) + sizeof(log.template_id));
                uint32_t n = static_cast<uint32_t>(log.var_codes.size());
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + sizeof(n));
                for (uint32_t code : log.var_codes) {
                    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&code), reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
                }
                break;
            }
        }
    }
    // 写入占位标志
    data.push_back(node->isPlaceholder() ? 1 : 0);
    // 写入子节点数量
    uint32_t child_count = static_cast<uint32_t>(node->getChildren().size());
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&child_count), reinterpret_cast<uint8_t*>(&child_count) + sizeof(child_count));
    // 递归序列化子节点
    for (const auto& child : node->getChildren()) {
        serializeNode(child.get(), data);
    }
}

std::vector<uint8_t> Trie::serialize() const {
    std::vector<uint8_t> data;
    serializeNode(root_.get(), data);
    return data;
}

TrieNode* Trie::deserializeNode(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos + sizeof(uint32_t) > data.size()) return nullptr;
    uint32_t path_len;
    std::memcpy(&path_len, &data[pos], sizeof(path_len));
    pos += sizeof(path_len);
    std::vector<NodeValue> path;
    for (uint32_t i = 0; i < path_len; ++i) {
        if (pos >= data.size()) return nullptr;
        uint8_t type_byte = data[pos++];
        switch (type_byte) {
            case 0: {
                uint32_t v;
                std::memcpy(&v, &data[pos], sizeof(v));
                pos += sizeof(v);
                path.emplace_back(v);
                break;
            }
            case 1: {
                int64_t v;
                std::memcpy(&v, &data[pos], sizeof(v));
                pos += sizeof(v);
                path.emplace_back(v);
                break;
            }
            case 2: {
                double v;
                std::memcpy(&v, &data[pos], sizeof(v));
                pos += sizeof(v);
                path.emplace_back(v);
                break;
            }
            case 3: {
                bool v = (data[pos++] == 1);
                path.emplace_back(v);
                break;
            }
            case 4: { // EncodedTimestamp
                uint32_t pattern_id;
                int64_t epoch;
                std::memcpy(&pattern_id, &data[pos], sizeof(pattern_id));
                pos += sizeof(pattern_id);
                std::memcpy(&epoch, &data[pos], sizeof(epoch));
                pos += sizeof(epoch);
                path.emplace_back(EncodedTimestamp{pattern_id, epoch});
                break;
            }
            case 5: { // EncodedLog
                uint32_t template_id;
                std::memcpy(&template_id, &data[pos], sizeof(template_id));
                pos += sizeof(template_id);
                uint32_t n;
                std::memcpy(&n, &data[pos], sizeof(n));
                pos += sizeof(n);
                std::vector<uint32_t> var_codes(n);
                for (uint32_t j = 0; j < n; ++j) {
                    std::memcpy(&var_codes[j], &data[pos], sizeof(uint32_t));
                    pos += sizeof(uint32_t);
                }
                path.emplace_back(EncodedLog{template_id, var_codes});
                break;
            }
            default:
                return nullptr;
        }
    }
    if (pos >= data.size()) return nullptr;
    bool is_placeholder = (data[pos++] == 1);
    auto node = std::make_unique<TrieNode>(path, is_placeholder);
    if (pos + sizeof(uint32_t) > data.size()) return nullptr;
    uint32_t child_count;
    std::memcpy(&child_count, &data[pos], sizeof(child_count));
    pos += sizeof(child_count);
    for (uint32_t i = 0; i < child_count; ++i) {
        TrieNode* child = deserializeNode(data, pos);
        if (child) node->getChildren().emplace_back(child);
    }
    return node.release();
}

void Trie::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    root_.reset(deserializeNode(data, pos));
}

const std::vector<FieldKey>& Trie::getOrderedFields() const {
    return ordered_fields_;
}

void Trie::setOrderedFields(const std::vector<FieldKey>& fields) {
    ordered_fields_ = fields;
}

void Trie::copyChildren(const TrieNode* src, TrieNode* dest) {
    for (const auto& child : src->getChildren()) {
        auto new_child = std::make_unique<TrieNode>(child->getPath(), child->isPlaceholder());
        copyChildren(child.get(), new_child.get());
        dest->getChildren().push_back(std::move(new_child));
    }
}

// 类型对齐辅助（融合trie_type_aware的类型感知）
static Value alignValueType(const FieldKey& key, const Value& value) {
    try {
        switch (key.type) {
            case FieldType::Double:
                if (std::holds_alternative<double>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return static_cast<double>(std::get<int64_t>(value));
                if (std::holds_alternative<bool>(value)) return static_cast<double>(std::get<bool>(value) ? 1.0 : 0.0);
                if (std::holds_alternative<std::string>(value)) return std::stod(std::get<std::string>(value));
                break;
            case FieldType::Int:
                if (std::holds_alternative<int64_t>(value)) return value;
                if (std::holds_alternative<double>(value)) return static_cast<int64_t>(std::get<double>(value));
                if (std::holds_alternative<bool>(value)) return static_cast<int64_t>(std::get<bool>(value) ? 1 : 0);
                if (std::holds_alternative<std::string>(value)) return static_cast<int64_t>(std::stoll(std::get<std::string>(value)));
                break;
            case FieldType::Bool:
                if (std::holds_alternative<bool>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return static_cast<bool>(std::get<int64_t>(value) != 0);
                if (std::holds_alternative<double>(value)) return static_cast<bool>(std::get<double>(value) != 0.0);
                if (std::holds_alternative<std::string>(value)) {
                    const auto& s = std::get<std::string>(value);
                    return static_cast<bool>(s == "true" || s == "1");
                }
                break;
            case FieldType::String:
            case FieldType::Null:
            case FieldType::Timestamp:
            case FieldType::LogType:
                if (std::holds_alternative<std::string>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return std::to_string(std::get<int64_t>(value));
                if (std::holds_alternative<double>(value)) return std::to_string(std::get<double>(value));
                if (std::holds_alternative<bool>(value)) return std::get<bool>(value) ? "true" : "false";
                break;
            default:
                break;
        }
    } catch (...) {}
    return Value(nullptr);
}

// NodeValue创建（融合trie_type_aware的类型安全编码）
NodeValue Trie::createNodeValue(const FieldKey& key, const Value& value, FieldDictionaryManager& manager) {
    Value aligned = alignValueType(key, value);
    if (std::holds_alternative<std::nullptr_t>(aligned)) return NodeValue(nullptr);
    switch (static_cast<FieldType>(key.type)) {
        case FieldType::String:
        case FieldType::Null:
            return NodeValue(manager.variableDict().getOrAddFieldValue(key, aligned));
        case FieldType::Timestamp: {
            std::string str_val;
            if (std::holds_alternative<std::string>(aligned)) {
                str_val = std::get<std::string>(aligned);
            } else if (std::holds_alternative<int64_t>(aligned)) {
                str_val = std::to_string(std::get<int64_t>(aligned));
            } else if (std::holds_alternative<double>(aligned)) {
                str_val = std::to_string(std::get<double>(aligned));
            } else if (std::holds_alternative<bool>(aligned)) {
                str_val = std::get<bool>(aligned) ? "true" : "false";
            } else {
                return NodeValue(nullptr);
            }
            auto encoded = manager.timestampDict().encode(key, str_val);
            return NodeValue(encoded);
        }
        case FieldType::LogType: {
            std::string str_val;
            if (std::holds_alternative<std::string>(aligned)) {
                str_val = std::get<std::string>(aligned);
            } else if (std::holds_alternative<int64_t>(aligned)) {
                str_val = std::to_string(std::get<int64_t>(aligned));
            } else if (std::holds_alternative<double>(aligned)) {
                str_val = std::to_string(std::get<double>(aligned));
            } else if (std::holds_alternative<bool>(aligned)) {
                str_val = std::get<bool>(aligned) ? "true" : "false";
            } else {
                return NodeValue(nullptr);
            }
            auto [tmpl, vars] = manager.logtypeDict().extractTemplateAndVars(str_val);
            auto encoded = manager.logtypeDict().encodeLog(key, tmpl, vars);
            return NodeValue(encoded);
        }
        case FieldType::Int:
            if (std::holds_alternative<int64_t>(aligned))
                return NodeValue(std::get<int64_t>(aligned));
            else
                return NodeValue(nullptr);
        case FieldType::Double:
            if (std::holds_alternative<double>(aligned))
                return NodeValue(std::get<double>(aligned));
            else
                return NodeValue(nullptr);
        case FieldType::Bool:
            if (std::holds_alternative<bool>(aligned))
                return NodeValue(std::get<bool>(aligned));
            else
                return NodeValue(nullptr);
        default:
            return NodeValue(nullptr);
    }
}

// 字段值重建（融合trie_type_aware的类型安全解码）
std::string Trie::reconstructFieldValue(const FieldKey& key, const NodeValue& node_value, const FieldDictionaryManager& manager) const {
    if (std::holds_alternative<std::nullptr_t>(node_value)) return "";
    switch (key.type) {
        case FieldType::Timestamp:
            if (std::holds_alternative<EncodedTimestamp>(node_value))
                return manager.timestampDict().decode(key, std::get<EncodedTimestamp>(node_value));
            else
                return "";
        case FieldType::LogType:
            if (std::holds_alternative<EncodedLog>(node_value))
                return manager.logtypeDict().decodeLogToString(key, std::get<EncodedLog>(node_value));
            else
                return "";
        case FieldType::String:
        case FieldType::Null:
            if (std::holds_alternative<uint32_t>(node_value)) {
                auto opt_value = manager.getFieldValueByCode(key, std::get<uint32_t>(node_value));
                if (opt_value) {
                    if (std::holds_alternative<std::string>(*opt_value))
                        return std::get<std::string>(*opt_value);
                    if (std::holds_alternative<std::nullptr_t>(*opt_value))
                        return "null";
                }
            }
            return "";
        case FieldType::Int:
            if (std::holds_alternative<int64_t>(node_value))
                return std::to_string(std::get<int64_t>(node_value));
            else
                return "";
        case FieldType::Double:
            if (std::holds_alternative<double>(node_value))
                return std::to_string(std::get<double>(node_value));
            else
                return "";
        case FieldType::Bool:
            if (std::holds_alternative<bool>(node_value))
                return std::get<bool>(node_value) ? "true" : "false";
            else
                return "";
        default:
            return "";
    }
}

} // namespace json2
