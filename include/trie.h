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

// 节点值类型：可以是编码值或原始值，std::nullptr_t用于空字段占位
using NodeValue = std::variant<uint32_t, int64_t, double, bool, std::nullptr_t, EncodedTimestamp, EncodedLog>;

// NodeValue哈希函数
struct NodeValueHash {
    std::size_t operator()(const NodeValue& v) const {
        return std::visit([](auto&& arg) -> std::size_t {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>) {
                return std::hash<T>{}(arg);
            } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                return 0;
            } else if constexpr (std::is_same_v<T, EncodedTimestamp>) {
                return std::hash<uint32_t>{}(arg.pattern_id) ^ std::hash<int64_t>{}(arg.epoch);
            } else if constexpr (std::is_same_v<T, EncodedLog>) {
                std::size_t h = std::hash<uint32_t>{}(arg.template_id);
                for (auto code : arg.var_codes) h ^= std::hash<uint32_t>{}(code) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            } else {
                return 0;
            }
        }, v);
    }
};

// 路径压缩Trie树节点
class TrieNode {
public:
    // 构造函数
    TrieNode(const std::vector<NodeValue>& path = {}, bool is_placeholder = false);

    // 获取路径片段
    const std::vector<NodeValue>& getPath() const;
    std::vector<NodeValue>& getPath(); // 新增：非 const 版本

    // 是否为占位节点
    bool isPlaceholder() const;
    void setPlaceholder(bool is_placeholder);

    // 获取所有子节点
    std::unordered_map<NodeValue, std::unique_ptr<TrieNode>, NodeValueHash>& getChildren();
    const std::unordered_map<NodeValue, std::unique_ptr<TrieNode>, NodeValueHash>& getChildren() const;

    void setPath(const std::vector<NodeValue>& path);

private:
    std::vector<NodeValue> path_;  // 路径片段（可为多个NodeValue，表示合并的多层）
    std::unordered_map<NodeValue, std::unique_ptr<TrieNode>, NodeValueHash> children_;  // 子节点
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

    // 解压路径压缩的Trie
    void expandPaths();

private:
    std::unique_ptr<TrieNode> root_;  // 根节点
    std::vector<FieldKey> ordered_fields_;  // 字段名+类型
    
    // 递归序列化节点
    void serializeNode(const TrieNode* node, std::vector<uint8_t>& data) const;
    
    // 递归反序列化节点
    TrieNode* deserializeNode(const std::vector<uint8_t>& data, size_t& pos, bool is_root);
    
    // 递归复制子节点
    void copyChildren(const TrieNode* src, TrieNode* dest);
    
    // 根据字段类型创建节点值
    NodeValue createNodeValue(const FieldKey& key, const Value& value, FieldDictionaryManager& manager);
    
};

} // namespace json2
