#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include "parser.h"
#include "dictionary.h"
#include <nlohmann/json.hpp>

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
    
    // 获取或创建子节点
    TrieNode* getOrCreateChild(uint32_t code);
    
    // 获取所有子节点
    const std::vector<std::pair<uint32_t, std::unique_ptr<TrieNode>>>& getChildren() const;

    void setPlaceholder(bool is_placeholder);

private:
    uint32_t code_;  // 节点的编码值
    std::vector<std::pair<uint32_t, std::unique_ptr<TrieNode>>> children_;  // 子节点列表
    bool is_placeholder_;  // 占位标志位
};

// 预处理的字段路径信息
struct FieldPath {
    std::string original_name;  // 原始字段名
    std::vector<std::string> parts;  // 拆分后的路径
    bool is_nested;  // 是否是嵌套字段
};

// Trie树
class Trie {
public:
    // 构造函数
    explicit Trie(const std::vector<std::string>& fields);
    
    // 插入一条记录
    void insert(const nlohmann::json& record, Dictionary& dict);
    
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
    std::vector<FieldPath> field_paths_;  // 预处理的字段路径
    
    // 预处理字段路径
    void preprocessFieldPaths();
    
    // 递归序列化节点
    void serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const;
    
    // 递归反序列化节点
    TrieNode* deserializeNode(const std::vector<uint8_t>& data, size_t& pos);
    
    // 递归复制子节点
    void copyChildren(const TrieNode* src, TrieNode* dest);
};

} // namespace json2
