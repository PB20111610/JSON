#include "../include/trie.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <functional>

namespace json2 {

// TrieNode实现
TrieNode::TrieNode(uint32_t code)
    : code_(code) {}

TrieNode* TrieNode::getOrCreateChild(uint32_t code) {
    auto it = children_.find(code);
    if (it == children_.end()) {
        auto [new_it, _] = children_.emplace(code, std::make_unique<TrieNode>(code));
        return new_it->second.get();
    }
    return it->second.get();
}

const std::unordered_map<uint32_t, std::unique_ptr<TrieNode>>& TrieNode::getChildren() const {
    return children_;
}

uint32_t TrieNode::getCode() const {
    return code_;
}

bool TrieNode::isPlaceholder() const {
    return code_ == 0;
}

// Trie实现
Trie::Trie(const std::vector<ParsedField>& fields) 
    : ordered_fields_(fields), root_(std::make_unique<TrieNode>(0)) {
}

void Trie::insert(const std::shared_ptr<JsonObject>& record, Dictionary& dict) {
    if (!record) return;
    
    TrieNode* current = root_.get();
    
    // 按照字段顺序遍历记录
    for (const auto& field : ordered_fields_) {
        const auto& value = record->getField(field.name);
        if (!value) {
            // 如果字段不存在，创建占位符节点
            current = current->getOrCreateChild(0);
            continue;
        }
        
        // 获取字段值的编码
        uint32_t code = dict.getCode(value->toString(), field.dictType);
        if (code == 0) {
            // 如果编码为0，说明是无效值，创建占位符节点
            current = current->getOrCreateChild(0);
        } else {
            // 创建或获取子节点
            current = current->getOrCreateChild(code);
        }
    }
}

// 辅助函数：写入varint
void writeVarint(std::vector<uint8_t>& data, uint32_t value) {
    while (value >= 0x80) {
        data.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    data.push_back(static_cast<uint8_t>(value));
}

// 辅助函数：读取varint
uint32_t readVarint(const std::vector<uint8_t>& data, size_t& pos) {
    uint32_t result = 0;
    uint32_t shift = 0;
    while (pos < data.size()) {
        uint8_t byte = data[pos++];
        result |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    return result;
}

// 辅助函数：检查是否是单分支路径
bool isSingleBranchPath(const TrieNode* node) {
    return node && node->getChildren().size() == 1;
}

// 辅助函数：获取单分支路径
std::vector<uint32_t> getSingleBranchPath(const TrieNode* node) {
    std::vector<uint32_t> path;
    const TrieNode* current = node;
    while (isSingleBranchPath(current)) {
        auto it = current->getChildren().begin();
        path.push_back(it->first);  // 存储code
        current = it->second.get();
    }
    return path;
}

void Trie::serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const {
    if (!node) return;
    
    // 写入节点编码（使用varint）
    writeVarint(data, node->getCode());
    
    // 收集并排序子节点
    std::vector<std::pair<uint32_t, const TrieNode*>> children;
    for (const auto& [code, child] : node->getChildren()) {
        children.emplace_back(code, child.get());
    }
    std::sort(children.begin(), children.end());
    
    // 写入子节点数量（使用varint）
    writeVarint(data, children.size());
    
    // 递归序列化子节点
    for (const auto& [code, child] : children) {
        serializeNode(child, data);
    }
}

std::vector<uint8_t> Trie::serialize() const {
    std::vector<uint8_t> result;
    serializeNode(root_.get(), result);
    return result;
}

TrieNode* Trie::deserializeNode(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos >= data.size()) return nullptr;
    
    // 读取节点编码
    uint32_t code = readVarint(data, pos);
    
    // 创建节点
    auto node = std::make_unique<TrieNode>(code);
    
    // 读取子节点数量
    uint32_t child_count = readVarint(data, pos);
    
    // 递归反序列化子节点
    for (uint32_t i = 0; i < child_count; ++i) {
        TrieNode* child = deserializeNode(data, pos);
        if (child) {
            // 创建新节点并复制子节点
            TrieNode* new_child = node->getOrCreateChild(child->getCode());
            // 递归复制子节点的子节点
            copyChildren(child, new_child);
            delete child;
        }
    }
    
    return node.release();
}

// 辅助函数：递归复制子节点
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

const std::vector<ParsedField>& Trie::getOrderedFields() const {
    return ordered_fields_;
}

// 辅助函数：将带"."的字段名拆分为路径
std::vector<std::string> splitFieldName(const std::string& fieldName) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : fieldName) {
        if (c == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

// 辅助函数：递归构建JSON对象
std::shared_ptr<JsonObject> Trie::buildJsonObject(
    const TrieNode* node,
    const std::vector<std::string>& fieldPath,
    size_t depth,
    const Dictionary& dict
) const {
    if (!node) return nullptr;
    
    auto result = std::make_shared<JsonObject>();
    
    // 如果当前节点有值，添加到结果中
    if (!node->isPlaceholder() && depth < ordered_fields_.size()) {
        const auto& field = ordered_fields_[depth];
        const std::string& value = dict.getString(node->getCode(), field.dictType);
        // 确保所有值都用双引号包住
        result->addField(field.name, std::make_shared<JsonString>("\"" + value + "\""));
    }
    
    // 处理子节点
    for (const auto& [code, child] : node->getChildren()) {
        if (depth + 1 < ordered_fields_.size()) {
            const auto& field = ordered_fields_[depth + 1];
            std::vector<std::string> parts = splitFieldName(field.name);
            
            // 如果字段名包含点号，需要创建嵌套对象
            if (parts.size() > 1) {
                auto current = result;
                // 创建或获取嵌套对象
                for (size_t i = 0; i < parts.size() - 1; ++i) {
                    auto nested = current->getField(parts[i]);
                    if (!nested) {
                        nested = std::make_shared<JsonObject>();
                        current->addField(parts[i], nested);
                    }
                    current = std::static_pointer_cast<JsonObject>(nested);
                }
                // 递归处理子节点
                auto child_obj = buildJsonObject(child.get(), parts, depth + 1, dict);
                if (child_obj) {
                    // 合并子节点的字段到当前对象
                    for (const auto& [key, value] : child_obj->getFields()) {
                        current->addField(key, value);
                    }
                }
            } else {
                // 递归处理子节点
                auto child_obj = buildJsonObject(child.get(), {field.name}, depth + 1, dict);
                if (child_obj) {
                    // 合并子节点的字段到当前对象
                    for (const auto& [key, value] : child_obj->getFields()) {
                        result->addField(key, value);
                    }
                }
            }
        }
    }
    
    return result;
}

std::string Trie::toJson(const Dictionary& dict) const {
    std::stringstream ss;
    bool first = true;
    
    // 递归遍历所有路径
    std::function<void(const TrieNode*, std::vector<std::pair<std::string, std::string>>, size_t)> traverse = 
        [&](const TrieNode* node, std::vector<std::pair<std::string, std::string>> currentRecord, size_t depth) {
        if (!node) return;
        
        // 如果不是根节点，添加当前字段到记录中
        if (depth > 0 && !node->isPlaceholder()) {
            // 根据深度获取字段名
            std::string fieldName = ordered_fields_[depth - 1].name;
            // 从字典中获取原始值
            std::string value = dict.getString(node->getCode(), ordered_fields_[depth - 1].dictType);
            currentRecord.push_back({fieldName, value});
        }
        
        // 如果是叶子节点，输出完整记录
        if (node->getChildren().empty() && !currentRecord.empty()) {
            if (!first) {
                ss << "\n";
            }
            ss << "{";
            for (size_t i = 0; i < currentRecord.size(); ++i) {
                if (i > 0) ss << ",";
                ss << "\"" << currentRecord[i].first << "\":\"" << currentRecord[i].second << "\"";
            }
            ss << "}";
            first = false;
        }
        
        // 递归处理所有子节点
        for (const auto& [code, child] : node->getChildren()) {
            traverse(child.get(), currentRecord, depth + 1);
        }
    };
    
    // 从根节点开始遍历
    traverse(root_.get(), {}, 0);
    return ss.str();
}

} // namespace json2
