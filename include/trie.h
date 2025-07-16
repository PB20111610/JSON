#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <variant>
#include "field_key.h"
#include "variable_dictionary.h"
#include "timestamp_dictionary.h"
#include "field_dictionary_manager.h"
#include "logtype_dictionary.h"

// Forward declare simdjson types to avoid including the full header here
namespace simdjson {
namespace dom {
    class parser;
}
}

namespace json2 {

// 节点值类型：可以是编码值或原始值
using NodeValue = std::variant<uint32_t, int64_t, double, bool, std::nullptr_t, EncodedTimestamp, EncodedLog>;

// 路径压缩Trie树节点
class TrieNode {
public:
    // 构造函数
    TrieNode(const std::vector<NodeValue>& path = {}, bool is_placeholder = false);

    // 获取路径片段
    const std::vector<NodeValue>& getPath() const;

    // 是否为占位节点
    bool isPlaceholder() const;
    void setPlaceholder(bool is_placeholder);

    // 获取所有子节点
    const std::vector<std::unique_ptr<TrieNode>>& getChildren() const;
    std::vector<std::unique_ptr<TrieNode>>& getChildren();

private:
    std::vector<NodeValue> path_;  // 路径片段（可为多个NodeValue，表示合并的多层）
    std::vector<std::unique_ptr<TrieNode>> children_;  // 子节点
    bool is_placeholder_;  // 占位标志位
};

// Trie树
class Trie {
public:
    // 构造函数
    explicit Trie(const std::vector<FieldKey>& fields);
    
    // 插入一条记录
    void insert(const std::string& record_string, FieldDictionaryManager& manager, simdjson::dom::parser& parser);
    
    // 序列化树结构
    std::vector<uint8_t> serialize() const;
    
    // 反序列化树结构
    void deserialize(const std::vector<uint8_t>& data);
    
    // 获取有序字段列表
    const std::vector<FieldKey>& getOrderedFields() const;
    
    // 设置有序字段列表（用于解压缩）
    void setOrderedFields(const std::vector<FieldKey>& fields);
    
    // 获取根节点（用于测试和调试）
    const TrieNode* getRoot() const { return root_.get(); }
    
    // 将Trie树转换为JSON字符串
    std::string toJson(const Dictionary& dict) const;

    // 从节点值重建字段值（移到public，供外部重建调用）
    std::string reconstructFieldValue(const FieldKey& key, const NodeValue& node_value, const FieldDictionaryManager& manager) const;

    // 批量路径压缩接口
    void compressPaths();

private:
    std::unique_ptr<TrieNode> root_;  // 根节点
    std::vector<FieldKey> ordered_fields_;  // 字段名+类型
    
    // 递归序列化节点
    void serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const;
    
    // 递归反序列化节点
    TrieNode* deserializeNode(const std::vector<uint8_t>& data, size_t& pos);
    
    // 递归复制子节点
    void copyChildren(const TrieNode* src, TrieNode* dest);
    
    // 根据字段类型创建节点值
    NodeValue createNodeValue(const FieldKey& key, const Value& value, FieldDictionaryManager& manager);
    
};

} // namespace json2
