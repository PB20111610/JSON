#include "../include/trie.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iostream>
#include <simdjson.h>
#include <functional>
#include "../include/variable_dictionary.h"
#include "../include/field_parser.h"

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

// 使用统一的字段解析工具

// 标准Trie插入（每层一个字段）
void Trie::insert(const std::string& record_string, FieldDictionaryManager& manager, simdjson::dom::parser& parser) {
    simdjson::dom::element record;
    auto error = parser.parse(record_string).get(record);
    if (error) return;

    // 1. 动态扩展ordered_fields_，支持新字段（使用重新判断的类型）
    if (record.type() == simdjson::dom::element_type::OBJECT) {
        auto obj = record.get_object();
        // 递归收集嵌套对象字段（展开为 ~a.b.c）
        std::function<void(const std::string&, simdjson::dom::element)> add_nested_fields = [&](const std::string& prefix, simdjson::dom::element elem) {
            if (elem.type() != simdjson::dom::element_type::OBJECT) return;
            auto nested_obj = elem.get_object();
            for (auto sub : nested_obj) {
                std::string base_name = prefix.empty() ? std::string(sub.key) : (prefix + "." + std::string(sub.key));
                std::string nested_name = "~" + base_name;
                FieldType nested_type = FieldParser::inferFieldType(nested_name, sub.value, manager, false);
                bool has_same_nested_name_type = false;
                for (const auto& fk : ordered_fields_) {
                    if (fk.name == nested_name && fk.type == nested_type) { has_same_nested_name_type = true; break; }
                }
                if (!has_same_nested_name_type) {
                    FieldKey nested_fk{nested_name, nested_type};
                    ordered_fields_.push_back(nested_fk);
                    Value nested_val = FieldParser::extractValue(sub.value);
                    manager.addFieldValue(nested_fk, nested_type, nested_val);
                }
                // 继续向下展开
                if (sub.value.type() == simdjson::dom::element_type::OBJECT) {
                    add_nested_fields(base_name, sub.value);
                }
            }
        };
        for (auto field : obj) {
            const std::string& field_name = std::string(field.key);
            // 对同名不同类型：允许追加新类型（按 name+type 去重）
            FieldType inferred_type = FieldParser::inferFieldType(field_name, field.value, manager, false);
            bool has_same_name_type = false;
            for (const auto& fk : ordered_fields_) {
                if (fk.name == field_name && fk.type == inferred_type) { has_same_name_type = true; break; }
            }
            // 只为非OBJECT类型字段扩展ordered_fields_
            if (field.value.type() != simdjson::dom::element_type::OBJECT) {
                if (!has_same_name_type) {
                    FieldKey new_fk{field_name, inferred_type};
                    ordered_fields_.push_back(new_fk);
                    // 使用统一的字段值提取
                    Value value = FieldParser::extractValue(field.value);
                    manager.addFieldValue(new_fk, inferred_type, value);
                }
            } else { // 对于对象类型，递归展开嵌套字段
                add_nested_fields(field_name, field.value);
            }
        }
    }
    
    // 2. 处理结构化数组产生的字段（如 tags[0]）
    // 这些字段在字段分析阶段已经被添加到 ordered_fields_ 中，但需要在这里处理
    for (const auto& key : ordered_fields_) {
        // 检查是否为结构化数组字段（包含 [）
        if (key.name.find('[') != std::string::npos) {
            // 检查是否已经在第一步中处理过（作为原始字段）
            bool already_processed = false;
            if (record.type() == simdjson::dom::element_type::OBJECT) {
                auto obj = record.get_object();
                for (auto field : obj) {
                    if (std::string(field.key) == key.name) {
                        already_processed = true;
                        break;
                    }
                }
            }
            
            // 如果没有处理过，说明这是结构化数组产生的字段
            if (!already_processed) {
                // 解析数组字段名，如 "tags[0]" -> "tags", 0
                size_t bracket_pos = key.name.find('[');
                std::string array_name = key.name.substr(0, bracket_pos);
                size_t end_bracket_pos = key.name.find(']', bracket_pos);
                int index = std::stoi(key.name.substr(bracket_pos + 1, end_bracket_pos - bracket_pos - 1));
                
                // 尝试从原始记录中获取数组
                try {
                    auto array_result = record[array_name];
                    if (!array_result.error()) {
                        auto array = array_result.value();
                        if (array.type() == simdjson::dom::element_type::ARRAY) {
                            auto array_obj = array.get_array().value();
                            size_t array_size = 0;
                            for (auto element : array_obj) { array_size++; }
                            if (index < array_size) {
                                size_t current_index = 0;
                                for (auto element : array_obj) {
                                    if (current_index == index) {
                                        Value value = FieldParser::extractValue(element);
                                        manager.addFieldValue(key, key.type, value);
                                        break;
                                    }
                                    current_index++;
                                }
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    // 如果获取失败，忽略这个字段
                    std::cout << "[DEBUG] Trie::insert - Failed to add structured array field: " << key.name << ", reason: " << e.what() << std::endl;
                }
            }
        }
    }

    std::vector<NodeValue> values;
    for (const auto& key : ordered_fields_) {
        try {
            // 尝试直接访问字段
            simdjson::dom::element value_node;
            
            // 对于嵌套字段（以 ~ 开头），在查找时去掉 ~ 前缀
            std::string lookup_name = key.name;
            if (!key.name.empty() && key.name[0] == '~') {
                lookup_name = key.name.substr(1);
            }
            
            // 检查是否为结构化数组字段（包含 [）
            if (key.name.find('[') != std::string::npos) {
                // 结构化数组字段，暂时跳过，稍后处理
                NodeValue node_value = NodeValue(nullptr);
                values.push_back(node_value);
                continue;
            }
            
            if (lookup_name.find('.') != std::string::npos) {
                // 嵌套字段，使用统一的嵌套字段访问
                value_node = FieldParser::getNestedField(record, lookup_name);
            } else {
                // 简单字段，直接访问
                auto result = record[lookup_name];
                if (result.error()) {
                    throw simdjson::simdjson_error(result.error());
                }
                value_node = result.value();
            }
            
            // 使用统一的字段值提取
            Value value = FieldParser::extractValue(value_node);
            NodeValue node_value = createNodeValue(key, value, manager);            
            values.push_back(node_value);
        } catch (const simdjson::simdjson_error& e) {
            NodeValue node_value = NodeValue(nullptr);
            values.push_back(node_value);
        }
    }
    
    // 3. 为结构化数组字段设置正确的值
    for (size_t i = 0; i < ordered_fields_.size(); ++i) {
        const auto& key = ordered_fields_[i];
        if (key.name.find('[') != std::string::npos) {
            // 解析数组字段名，如 "tags[0]" -> "tags", 0
            size_t bracket_pos = key.name.find('[');
            std::string array_name = key.name.substr(0, bracket_pos);
            size_t end_bracket_pos = key.name.find(']', bracket_pos);
            int index = std::stoi(key.name.substr(bracket_pos + 1, end_bracket_pos - bracket_pos - 1));
            
            // 尝试从原始记录中获取数组
            try {
                auto array_result = record[array_name];
                if (!array_result.error()) {
                    auto array = array_result.value();
                    if (array.type() == simdjson::dom::element_type::ARRAY) {
                        auto array_obj = array.get_array().value();
                        size_t array_size = 0;
                        for (auto element : array_obj) { array_size++; }
                        if (index < array_size) {
                            size_t current_index = 0;
                            for (auto element : array_obj) {
                                if (current_index == index) {
                                    Value value = FieldParser::extractValue(element);
                                    NodeValue node_value = createNodeValue(key, value, manager);
                                    if (i < values.size()) {
                                        values[i] = node_value;
                                    }
                                    break;
                                }
                                current_index++;
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                // 如果获取失败，保持 nullptr
                std::cout << "[DEBUG] Trie::insert - Structured array value missing: " << key.name << ", reason: " << e.what() << std::endl;
            }
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
        if (std::holds_alternative<uint32_t>(val)) type_byte = static_cast<uint8_t>(NodeValueType::UINT32);
        else if (std::holds_alternative<int64_t>(val)) type_byte = static_cast<uint8_t>(NodeValueType::INT64);
        else if (std::holds_alternative<double>(val)) type_byte = static_cast<uint8_t>(NodeValueType::DOUBLE);
        else if (std::holds_alternative<bool>(val)) type_byte = static_cast<uint8_t>(NodeValueType::BOOL);
        else if (std::holds_alternative<std::nullptr_t>(val)) type_byte = static_cast<uint8_t>(NodeValueType::NULLPTR);
        else if (std::holds_alternative<TemplateEncodedTimestamp>(val)) type_byte = static_cast<uint8_t>(NodeValueType::TEMPLATE_ENCODED_TIMESTAMP);
        else if (std::holds_alternative<EncodedLog>(val)) type_byte = static_cast<uint8_t>(NodeValueType::ENCODED_LOG);
        data.push_back(type_byte);
        switch (type_byte) {
            case static_cast<int>(NodeValueType::UINT32): {
                uint32_t v = std::get<uint32_t>(val);
                data.insert(data.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + sizeof(v));
                break;
            }
            case static_cast<int>(NodeValueType::INT64): {
                int64_t v = std::get<int64_t>(val);
                data.insert(data.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + sizeof(v));
                break;
            }
            case static_cast<int>(NodeValueType::DOUBLE): {
                double v = std::get<double>(val);
                data.insert(data.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + sizeof(v));
                break;
            }
            case static_cast<int>(NodeValueType::BOOL): {
                bool v = std::get<bool>(val);
                data.push_back(v ? 1 : 0);
                break;
            }
            case static_cast<int>(NodeValueType::TEMPLATE_ENCODED_TIMESTAMP): { // TemplateEncodedTimestamp
                const auto& ts = std::get<TemplateEncodedTimestamp>(val);
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&ts.template_id), reinterpret_cast<const uint8_t*>(&ts.template_id) + sizeof(ts.template_id));
                uint32_t var_count = static_cast<uint32_t>(ts.var_codes.size());
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&var_count), reinterpret_cast<const uint8_t*>(&var_count) + sizeof(var_count));
                for (uint32_t var_code : ts.var_codes) {
                    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&var_code), reinterpret_cast<const uint8_t*>(&var_code) + sizeof(var_code));
                }
                break;
            }
            case static_cast<int>(NodeValueType::ENCODED_LOG): { // EncodedLog
                const auto& log = std::get<EncodedLog>(val);
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&log.template_id), reinterpret_cast<const uint8_t*>(&log.template_id) + sizeof(log.template_id));
                uint32_t n = static_cast<uint32_t>(log.var_codes.size());
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + sizeof(n));
                for (uint32_t code : log.var_codes) {
                    data.insert(data.end(), reinterpret_cast<const uint8_t*>(&code), reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
                }
                break;
            }
            case static_cast<int>(NodeValueType::NULLPTR): // std::nullptr_t
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
            case static_cast<int>(NodeValueType::UINT32): {
                uint32_t v;
                std::memcpy(&v, &data[pos], sizeof(v));
                pos += sizeof(v);
                path.emplace_back(v);
                break;
            }
            case static_cast<int>(NodeValueType::INT64): {
                int64_t v;
                std::memcpy(&v, &data[pos], sizeof(v));
                pos += sizeof(v);
                path.emplace_back(v);
                break;
            }
            case static_cast<int>(NodeValueType::DOUBLE): {
                double v;
                std::memcpy(&v, &data[pos], sizeof(v));
                pos += sizeof(v);
                path.emplace_back(v);
                break;
            }
            case static_cast<int>(NodeValueType::BOOL): {
                bool v = (data[pos++] == 1);
                path.emplace_back(v);
                break;
            }
            case static_cast<int>(NodeValueType::TEMPLATE_ENCODED_TIMESTAMP): { // TemplateEncodedTimestamp
                uint32_t template_id;
                std::memcpy(&template_id, &data[pos], sizeof(template_id));
                pos += sizeof(template_id);
                uint32_t var_count;
                std::memcpy(&var_count, &data[pos], sizeof(var_count));
                pos += sizeof(var_count);
                std::vector<uint32_t> var_codes(var_count);
                for (uint32_t j = 0; j < var_count; ++j) {
                    std::memcpy(&var_codes[j], &data[pos], sizeof(uint32_t));
                    pos += sizeof(uint32_t);
                }
                path.emplace_back(TemplateEncodedTimestamp{template_id, var_codes});
                break;
            }
            case static_cast<int>(NodeValueType::ENCODED_LOG): { // EncodedLog
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
            case static_cast<int>(NodeValueType::NULLPTR): // std::nullptr_t
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
            case FieldType::DOUBLE:
                if (std::holds_alternative<double>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return static_cast<double>(std::get<int64_t>(value));
                if (std::holds_alternative<bool>(value)) return static_cast<double>(std::get<bool>(value) ? 1.0 : 0.0);
            return Value(nullptr);
            case FieldType::INT64:
                if (std::holds_alternative<int64_t>(value)) return value;
                if (std::holds_alternative<double>(value)) return static_cast<int64_t>(std::get<double>(value));
                if (std::holds_alternative<bool>(value)) return static_cast<int64_t>(std::get<bool>(value) ? 1 : 0);
            return Value(nullptr);
            case FieldType::BOOL:
                if (std::holds_alternative<bool>(value)) return value;
                if (std::holds_alternative<int64_t>(value)) return static_cast<bool>(std::get<int64_t>(value) != 0);
                if (std::holds_alternative<double>(value)) return static_cast<bool>(std::get<double>(value) != 0.0);
            return Value(nullptr);
            case FieldType::STRING:
            if (std::holds_alternative<std::string>(value)) return value;
            return Value(nullptr);
            case FieldType::NULL_TYPE:
            case FieldType::TIMESTAMP:
            case FieldType::LOGTYPE:
            case FieldType::ARRAY:
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
    if (std::holds_alternative<std::nullptr_t>(aligned)) {
        return NodeValue(nullptr);
    }
    
    // 使用传入的字段类型进行编码，避免类型漂移导致的字典不匹配
    switch (key.type) {
        case FieldType::STRING:
        case FieldType::NULL_TYPE: {
            uint32_t code = 0;
            if (std::holds_alternative<std::string>(aligned)) {
                code = manager.addFieldValue(key, key.type, std::get<std::string>(aligned));
            } else if (std::holds_alternative<std::nullptr_t>(aligned)) {
                code = manager.addFieldValue(key, key.type, nullptr);
            } else if (std::holds_alternative<int64_t>(aligned)) {
                code = manager.addFieldValue(key, key.type, std::get<int64_t>(aligned));
            } else if (std::holds_alternative<double>(aligned)) {
                code = manager.addFieldValue(key, key.type, std::get<double>(aligned));
            } else if (std::holds_alternative<bool>(aligned)) {
                code = manager.addFieldValue(key, key.type, std::get<bool>(aligned));
            } else {
                return NodeValue(nullptr);
            }
            return NodeValue(code);
        }
        case FieldType::TIMESTAMP: {
            // 快速提取字符串值
            std::string str_val;
            if (std::holds_alternative<std::string>(aligned)) {
                str_val = std::get<std::string>(aligned);
            } else {
                // 对于非字符串类型，转换为字符串
                if (std::holds_alternative<int64_t>(aligned)) {
                    str_val = std::to_string(std::get<int64_t>(aligned));
                } else if (std::holds_alternative<double>(aligned)) {
                    str_val = std::to_string(std::get<double>(aligned));
                } else if (std::holds_alternative<bool>(aligned)) {
                    str_val = std::get<bool>(aligned) ? "true" : "false";
                } else {
                    return NodeValue(nullptr);
                }
            }
            auto encoded = manager.timestampDict().encodeTemplate(key, str_val);
            return NodeValue(encoded);
        }
        case FieldType::LOGTYPE: {
            // 快速提取字符串值
            std::string str_val;
            if (std::holds_alternative<std::string>(aligned)) {
                str_val = std::get<std::string>(aligned);
            } else {
                // 对于非字符串类型，转换为字符串
                if (std::holds_alternative<int64_t>(aligned)) {
                    str_val = std::to_string(std::get<int64_t>(aligned));
                } else if (std::holds_alternative<double>(aligned)) {
                    str_val = std::to_string(std::get<double>(aligned));
                } else if (std::holds_alternative<bool>(aligned)) {
                    str_val = std::get<bool>(aligned) ? "true" : "false";
                } else {
                    return NodeValue(nullptr);
                }
            }
            auto [tmpl, vars] = manager.logtypeDict().extractTemplateAndVars(str_val);
            auto encoded = manager.logtypeDict().encodeLog(key, tmpl, vars);
            return NodeValue(encoded);
        }
        case FieldType::INT64:
            if (std::holds_alternative<int64_t>(aligned))
                return NodeValue(std::get<int64_t>(aligned));
            else
                return NodeValue(nullptr);
        case FieldType::DOUBLE:
            if (std::holds_alternative<double>(aligned))
                return NodeValue(std::get<double>(aligned));
            else
                return NodeValue(nullptr);
        case FieldType::BOOL:
            if (std::holds_alternative<bool>(aligned))
                return NodeValue(std::get<bool>(aligned));
            else
                return NodeValue(nullptr);
        case FieldType::ARRAY:
            // 数组类型作为字符串处理
            if (std::holds_alternative<std::string>(aligned)) {
                uint32_t code = manager.addFieldValue(key, key.type, std::get<std::string>(aligned));
                return NodeValue(code);
            } else if (std::holds_alternative<std::nullptr_t>(aligned)) {
                uint32_t code = manager.addFieldValue(key, key.type, nullptr);
                return NodeValue(code);
            } else if (std::holds_alternative<int64_t>(aligned)) {
                uint32_t code = manager.addFieldValue(key, key.type, std::get<int64_t>(aligned));
                return NodeValue(code);
            } else if (std::holds_alternative<double>(aligned)) {
                uint32_t code = manager.addFieldValue(key, key.type, std::get<double>(aligned));
                return NodeValue(code);
            } else if (std::holds_alternative<bool>(aligned)) {
                uint32_t code = manager.addFieldValue(key, key.type, std::get<bool>(aligned));
                return NodeValue(code);
            } else {
                return NodeValue(nullptr);
            }
        default:
            return NodeValue(nullptr);
    }
}

// 字段值重建（融合trie_type_aware的类型安全解码）
std::string Trie::reconstructFieldValue(const FieldKey& key, const NodeValue& node_value, const FieldDictionaryManager& manager) const {
    if (std::holds_alternative<std::nullptr_t>(node_value)) return "";
    
    // 对于嵌套字段（以 ~ 开头），在查找值时去掉 ~ 前缀
    std::string lookup_name = key.name;
    if (!key.name.empty() && key.name[0] == '~') {
        lookup_name = key.name.substr(1);
    }
    
    // 创建用于查找的 FieldKey（去掉 ~ 前缀）
    FieldKey lookup_key{lookup_name, key.type};
    
    switch (key.type) {
        case FieldType::TIMESTAMP:
        if (std::holds_alternative<TemplateEncodedTimestamp>(node_value)) {
            const auto& ts = std::get<TemplateEncodedTimestamp>(node_value);
            return manager.timestampDict().decodeTemplate(lookup_key, ts);
        }
            else
                return "";
        case FieldType::LOGTYPE:
            if (std::holds_alternative<EncodedLog>(node_value))
                return manager.logtypeDict().decodeLogToString(lookup_key, std::get<EncodedLog>(node_value));
            else
                return "";
        case FieldType::STRING:
        case FieldType::NULL_TYPE:
            if (std::holds_alternative<uint32_t>(node_value)) {
                auto opt_value = manager.getFieldValueByCode(lookup_key, std::get<uint32_t>(node_value));
                if (opt_value) {
                    if (std::holds_alternative<std::string>(*opt_value))
                        return std::get<std::string>(*opt_value);
                    if (std::holds_alternative<std::nullptr_t>(*opt_value))
                        return "null";
                }
            }
            return "";
        case FieldType::INT64:
            if (std::holds_alternative<int64_t>(node_value))
                return std::to_string(std::get<int64_t>(node_value));
            else
                return "";
        case FieldType::DOUBLE:
            if (std::holds_alternative<double>(node_value))
                return std::to_string(std::get<double>(node_value));
            else
                return "";
        case FieldType::BOOL:
            if (std::holds_alternative<bool>(node_value))
                return std::get<bool>(node_value) ? "true" : "false";
            else
                return "";
        case FieldType::ARRAY:
            if (std::holds_alternative<uint32_t>(node_value)) {
                auto code = std::get<uint32_t>(node_value);
                auto opt_value = manager.getFieldValueByCode(lookup_key, code);
                if (opt_value) {
                    if (std::holds_alternative<std::string>(*opt_value)) {
                        return std::get<std::string>(*opt_value);
                    }
                    if (std::holds_alternative<std::nullptr_t>(*opt_value))
                        return "null";
                }
            }
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
