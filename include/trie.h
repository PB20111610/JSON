#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <variant>
#include "parser.h"
#include "variable_dictionary.h"
#include "timestamp_dictionary.h"

// Forward declare simdjson types to avoid including the full header here
namespace simdjson {
namespace dom {
    class parser;
}
}

namespace json2 {

// 节点值类型：可以是编码值或原始值
using NodeValue = std::variant<uint32_t, int64_t, double, bool>;

// Trie树节点
class TrieNode {
public:
    // 构造函数
    explicit TrieNode(const NodeValue& value, bool is_placeholder = false);
    
    // 获取节点值
    const NodeValue& getValue() const;
    
    // 是否为占位节点
    bool isPlaceholder() const;
    
    // 获取或创建子节点
    TrieNode* getOrCreateChild(const NodeValue& value);
    
    // 获取所有子节点
    const std::vector<std::pair<NodeValue, std::unique_ptr<TrieNode>>>& getChildren() const;

    void setPlaceholder(bool is_placeholder);

private:
    NodeValue value_;  // 节点的值（编码值或原始值）
    std::vector<std::pair<NodeValue, std::unique_ptr<TrieNode>>> children_;  // 子节点列表
    bool is_placeholder_;  // 占位标志位
};

// Trie树
class Trie {
public:
    // 构造函数
    explicit Trie(const std::vector<std::string>& fields);
    
    // 插入一条记录
    void insert(const std::string& record_string, Dictionary& dict, simdjson::dom::parser& parser);
    
    // 序列化树结构
    std::vector<uint8_t> serialize() const;
    
    // 反序列化树结构
    void deserialize(const std::vector<uint8_t>& data);
    
    // 获取有序字段列表
    const std::vector<std::string>& getOrderedFields() const;
    
    // 设置有序字段列表（用于解压缩）
    void setOrderedFields(const std::vector<std::string>& fields);
    
    // 获取根节点（用于测试和调试）
    const TrieNode* getRoot() const { return root_.get(); }
    
    // 将Trie树转换为JSON字符串
    std::string toJson(const Dictionary& dict) const;

private:
    std::unique_ptr<TrieNode> root_;  // 根节点
    std::vector<std::string> ordered_fields_;  // 只存字段名
    
    // 递归序列化节点
    void serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const;
    
    // 递归反序列化节点
    TrieNode* deserializeNode(const std::vector<uint8_t>& data, size_t& pos);
    
    // 递归复制子节点
    void copyChildren(const TrieNode* src, TrieNode* dest);
    
    // 根据字段类型创建节点值
    NodeValue createNodeValue(const std::string& field, FieldType type, const Value& value, Dictionary& dict);
    
    // 从节点值重建字段值
    std::string reconstructFieldValue(const std::string& field, FieldType type, const NodeValue& node_value, const Dictionary& dict) const;
};

} // namespace json2
