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

std::vector<NodeValue>& TrieNode::getPath() { return path_; }

bool TrieNode::isPlaceholder() const {
    return is_placeholder_;
}

void TrieNode::setPlaceholder(bool is_placeholder) {
    is_placeholder_ = is_placeholder;
}

void TrieNode::setPath(const std::vector<NodeValue>& path) {
    path_ = path;
}

std::unordered_map<NodeValue, std::unique_ptr<TrieNode>, NodeValueHash>& TrieNode::getChildren() {
    return children_;
}
const std::unordered_map<NodeValue, std::unique_ptr<TrieNode>, NodeValueHash>& TrieNode::getChildren() const {
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

    // 1. 动态扩展ordered_fields_，支持新字段
    if (record.type() == simdjson::dom::element_type::OBJECT) {
        auto obj = record.get_object();
        for (auto field : obj) {
            const std::string& field_name = std::string(field.key);
            // 检查是否已存在
            bool found = false;
            for (const auto& fk : ordered_fields_) {
                if (fk.name == field_name) { found = true; break; }
            }
            if (!found) {
                // 推断类型（支持Timestamp/LogType）
                FieldType type;
                std::string value_str;
                if (field.value.type() == simdjson::dom::element_type::STRING) {
                    auto str_res = field.value.get_string();
                    if (!str_res.error()) value_str = std::string(str_res.value());
                    else value_str = "";
                    if (manager.isTimestampField(field_name) && manager.isTimestampValue(value_str)) {
                        type = FieldType::Timestamp;
                    } else if (manager.isLogTemplate(value_str)) {
                        type = FieldType::LogType;
                    } else {
                        type = FieldType::String;
                    }
                } else {
                    switch (field.value.type()) {
                        case simdjson::dom::element_type::INT64:
                            type = FieldType::Int; break;
                        case simdjson::dom::element_type::DOUBLE:
                            type = FieldType::Double; break;
                        case simdjson::dom::element_type::BOOL:
                            type = FieldType::Bool; break;
                        case simdjson::dom::element_type::NULL_VALUE:
                            type = FieldType::Null; break;
                        default:
                            type = FieldType::String; break;
                    }
                }
                FieldKey new_fk{field_name, type};
                ordered_fields_.push_back(new_fk);
                // 同步更新FieldDictionaryManager，插入一个空值或当前值
                switch (type) {
                    case FieldType::String: {
                        auto str_res = field.value.get_string();
                        if (!str_res.error()) manager.addFieldValue(new_fk, type, std::string(str_res.value()));
                        else manager.addFieldValue(new_fk, type, "");
                        break;
                    }
                    case FieldType::Int: {
                        auto int_res = field.value.get_int64();
                        if (!int_res.error()) manager.addFieldValue(new_fk, type, int_res.value());
                        else manager.addFieldValue(new_fk, type, int64_t(0));
                        break;
                    }
                    case FieldType::Double: {
                        auto dbl_res = field.value.get_double();
                        if (!dbl_res.error()) manager.addFieldValue(new_fk, type, dbl_res.value());
                        else manager.addFieldValue(new_fk, type, 0.0);
                        break;
                    }
                    case FieldType::Bool: {
                        auto bool_res = field.value.get_bool();
                        if (!bool_res.error()) manager.addFieldValue(new_fk, type, bool_res.value());
                        else manager.addFieldValue(new_fk, type, false);
                        break;
                    }
                    case FieldType::Null:
                        manager.addFieldValue(new_fk, type, nullptr);
                        break;
                    case FieldType::Timestamp:
                    case FieldType::LogType: {
                        auto str_res = field.value.get_string();
                        if (!str_res.error()) manager.addFieldValue(new_fk, type, std::string(str_res.value()));
                        else manager.addFieldValue(new_fk, type, "");
                        break;
                    }
                    default:
                        manager.addFieldValue(new_fk, type, "");
                        break;
                }
            }
        }
    }

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
            NodeValue node_value = NodeValue(nullptr);
            values.push_back(node_value);
        }
    }
    // 标准Trie插入
    TrieNode* cur = root_.get();
    for (size_t i = 0; i < values.size(); ++i) {
        NodeValue& v = values[i];
        auto& children = cur->getChildren();
        auto it = children.find(v);
        bool is_placeholder = std::holds_alternative<std::nullptr_t>(v);
        if (it != children.end()) {
            cur = it->second.get();
            // 如果本次插入遇到缺失字段，且节点原本不是占位，则补上
            if (is_placeholder && !cur->isPlaceholder()) {
                cur->setPlaceholder(true);
            }
        } else {
            auto new_node = std::make_unique<TrieNode>(std::vector<NodeValue>{v}, is_placeholder);
            TrieNode* new_ptr = new_node.get();
            children[v] = std::move(new_node);
            cur = new_ptr;
        }
    }
}

// 修正后的批量路径压缩递归实现（合并path和children）
static void compressTrieNode(TrieNode* node) {
    for (auto& child : node->getChildren()) {
        compressTrieNode(child.second.get());
    }
    // 只在链式结构上合并：当前节点有且仅有一个子节点，且子节点也只有0或1个子节点
    while (node->getChildren().size() == 1) {
        auto it = node->getChildren().begin();
        TrieNode* only_child_ptr = it->second.get();
        // 如果子节点有多个分支，停止合并
        if (only_child_ptr->getChildren().size() > 1) break;

        std::unique_ptr<TrieNode> only_child = std::move(it->second);
        node->getChildren().clear();

        // 合并 path
        auto& node_path = node->getPath();
        const auto& child_path = only_child->getPath();
        node_path.insert(node_path.end(), child_path.begin(), child_path.end());

        // 合并 children
        auto& child_children = only_child->getChildren();
        for (auto& kv : child_children) {
            node->getChildren()[kv.first] = std::move(kv.second);
        }
        node->setPlaceholder(only_child->isPlaceholder());
    }
}

void Trie::compressPaths() {
    for (auto& child : root_->getChildren()) {
        compressTrieNode(child.second.get());
    }
}

// Trie序列化/反序列化
void Trie::serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const {
    uint32_t path_len = static_cast<uint32_t>(node->getPath().size());
    // std::cerr << "[SER] path_len=" << path_len << ", data_pos=" << data.size() << ", child_count=" << node->getChildren().size() << std::endl;
    // 写入path长度
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
        else if (std::holds_alternative<std::nullptr_t>(val)) type_byte = 6;
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
            case 6: // std::nullptr_t
                // 空值不写内容
                break;
        }
    }
    // 写入占位标志
    data.push_back(node->isPlaceholder() ? 1 : 0);
    // 写入子节点数量
    uint32_t child_count = static_cast<uint32_t>(node->getChildren().size());
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&child_count), reinterpret_cast<uint8_t*>(&child_count) + sizeof(child_count));
    // 递归序列化子节点
    for (const auto& child : node->getChildren()) {
        serializeNode(child.second.get(), data);
    }
}

std::vector<uint8_t> Trie::serialize() const {
    std::vector<uint8_t> data;
    serializeNode(root_.get(), data);
    return data;
}

// 新增is_root参数，根节点允许path.size()==0，非根节点要求path.size()==1
TrieNode* Trie::deserializeNode(const std::vector<uint8_t>& data, size_t& pos, bool is_root) {
    if (pos + sizeof(uint32_t) > data.size()) return nullptr;
    uint32_t path_len;
    std::memcpy(&path_len, &data[pos], sizeof(path_len));
    // std::cerr << "[DESER] path_len=" << path_len << ", pos=" << pos << std::endl;
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
            case 6: // std::nullptr_t
                path.emplace_back(std::nullptr_t{});
                break;
            default:
                return nullptr;
        }
    }
    // 不再断言，允许 path.size() > 1（压缩trie兼容）
    bool is_placeholder = (data[pos++] == 1);
    auto node = std::make_unique<TrieNode>(path, is_placeholder);
    if (pos + sizeof(uint32_t) > data.size()) return nullptr;
    uint32_t child_count;
    std::memcpy(&child_count, &data[pos], sizeof(child_count));
    // std::cerr << "[DESER] child_count=" << child_count << ", pos=" << pos << std::endl;
    pos += sizeof(child_count);
    for (uint32_t i = 0; i < child_count; ++i) {
        TrieNode* child = deserializeNode(data, pos, false);
        if (child) {
            auto key = child->getPath()[0];
            auto& children = node->getChildren();
            if (children.count(key)) {
                throw std::runtime_error("Duplicate child key in TrieNode deserialization!");
            }
            children[key] = std::unique_ptr<TrieNode>(child);
        }
    }
    return node.release();
}

void Trie::deserialize(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    root_.reset(deserializeNode(data, pos, true)); // 根节点允许path.size()==0
    // 反序列化后自动展开路径，恢复为标准trie
    expandPaths();
}

const std::vector<FieldKey>& Trie::getOrderedFields() const {
    return ordered_fields_;
}

void Trie::setOrderedFields(const std::vector<FieldKey>& fields) {
    ordered_fields_ = fields;
}

void Trie::copyChildren(const TrieNode* src, TrieNode* dest) {
    for (const auto& child : src->getChildren()) {
        auto new_child = std::make_unique<TrieNode>(child.second->getPath(), child.second->isPlaceholder());
        copyChildren(child.second.get(), new_child.get());
        dest->getChildren()[child.second->getPath()[0]] = std::move(new_child);
    }
}

// 类型对齐辅助（融合trie_type_aware的类型感知）
static Value alignValueType(const FieldKey& key, const Value& value) {
        switch (key.type) {
            case FieldType::Double:
                if (std::holds_alternative<double>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return static_cast<double>(std::get<int64_t>(value));
                if (std::holds_alternative<bool>(value)) return static_cast<double>(std::get<bool>(value) ? 1.0 : 0.0);
            return Value(nullptr);
            case FieldType::Int:
                if (std::holds_alternative<int64_t>(value)) return value;
                if (std::holds_alternative<double>(value)) return static_cast<int64_t>(std::get<double>(value));
                if (std::holds_alternative<bool>(value)) return static_cast<int64_t>(std::get<bool>(value) ? 1 : 0);
            return Value(nullptr);
            case FieldType::Bool:
                if (std::holds_alternative<bool>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return static_cast<bool>(std::get<int64_t>(value) != 0);
                if (std::holds_alternative<double>(value)) return static_cast<bool>(std::get<double>(value) != 0.0);
            return Value(nullptr);
            case FieldType::String:
            if (std::holds_alternative<std::string>(value)) return value;
            return Value(nullptr);
            case FieldType::Null:
            case FieldType::Timestamp:
            case FieldType::LogType:
            // 保持原有逻辑
                if (std::holds_alternative<std::string>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return std::to_string(std::get<int64_t>(value));
                if (std::holds_alternative<double>(value)) return std::to_string(std::get<double>(value));
                if (std::holds_alternative<bool>(value)) return std::get<bool>(value) ? "true" : "false";
            return Value(nullptr);
            default:
    return Value(nullptr);
    }
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

void expandTrieNode(TrieNode* node) {
    // 先拆分 path
    while (node->getPath().size() > 1) {
        std::vector<NodeValue> path = node->getPath();
        NodeValue first = path[0];
        std::vector<NodeValue> rest(path.begin() + 1, path.end());
        auto new_child = std::make_unique<TrieNode>(rest, node->isPlaceholder());
        new_child->getChildren() = std::move(node->getChildren());
        node->getChildren().clear();
        node->getChildren()[first] = std::move(new_child);
        node->setPlaceholder(false);
        node->setPath({first});
    }
    // 再递归展开所有子节点
    for (auto& child : node->getChildren()) {
        expandTrieNode(child.second.get());
    }
}

void Trie::expandPaths() {
    for (auto& child : root_->getChildren()) {
        expandTrieNode(child.second.get());
    }
}

} // namespace json2
