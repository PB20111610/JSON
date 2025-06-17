#pragma once

#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "parser.h"
#include "dictionary.h"

namespace json2 {

// Trie树节点
class TrieNode {
public:
    // 构造函数
    explicit TrieNode(uint32_t code, bool is_placeholder = false);
    
    // 获取节点编码
    uint32_t getCode() const;
    
    // 是否为占位节点
    bool isPlaceholder() const;
    
    // 获取子节点
    TrieNode* getOrCreateChild(uint32_t code);
    
    // 获取所有子节点
    const std::unordered_map<uint32_t, std::unique_ptr<TrieNode>>& getChildren() const;

    void setPlaceholder(bool is_placeholder);  // 新增：设置占位标志

private:
    uint32_t code_;  // 节点的编码值
    std::unordered_map<uint32_t, std::unique_ptr<TrieNode>> children_;  // 子节点映射
    bool is_placeholder_;  // 新增：占位标志位
};

// Trie树
class Trie {
public:
    // 构造函数
    explicit Trie(const std::vector<ParsedField>& fields);
    
    // 插入一条记录
    void insert(const std::shared_ptr<JsonObject>& record, Dictionary& dict);
    
    // 序列化树结构
    std::vector<uint8_t> serialize() const;
    
    // 反序列化树结构
    void deserialize(const std::vector<uint8_t>& data);
    
    // 获取有序字段列表
    const std::vector<ParsedField>& getOrderedFields() const;
    
    // 获取根节点（用于测试和调试）
    const TrieNode* getRoot() const { return root_.get(); }
    
    // 将Trie树转换为JSON字符串
    std::string toJson(const Dictionary& dict) const;

private:
    std::unique_ptr<TrieNode> root_;  // 根节点
    std::vector<ParsedField> ordered_fields_;  // 按冗余度排序的字段列表
    
    // 递归序列化节点
    void serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const;
    
    // 递归反序列化节点
    TrieNode* deserializeNode(const std::vector<uint8_t>& data, size_t& pos);
    
    // 递归复制子节点
    void copyChildren(const TrieNode* src, TrieNode* dest);
    
    // 辅助函数：递归构建JSON对象
    std::shared_ptr<JsonObject> buildJsonObject(
        const TrieNode* node,
        const std::vector<std::string>& fieldPath,
        size_t depth,
        const Dictionary& dict
    ) const;
};

} // namespace json2
