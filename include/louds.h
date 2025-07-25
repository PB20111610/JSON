#pragma once
#include <sdsl/bit_vectors.hpp>
#include <vector>
#include <cstdint>
#include <map>
#include <memory>
#include "trie.h"

namespace json2 {

// 分层节点数据存储 - 每层存储同类型数据
class LayeredNodeStorage {
public:
    // 添加新层
    void addLayer(const FieldKey& field_key, size_t start_offset);
    
    // 向当前层添加节点值
    void addNodeValue(const NodeValue& value, size_t layer_idx);
    
    // 获取指定层的节点值
    const NodeValue& getNodeValue(size_t layer_idx, size_t node_idx) const;
    
    // 获取层内容
    const std::vector<NodeValue>& getLayer(size_t layer_idx) const;
    
    // 获取层数
    size_t getLayerCount() const { return layers_.size(); }
    
    // 序列化/反序列化
    void serialize(std::ostream& out) const;
    void deserialize(std::istream& in);
    
    // 获取指定节点在BFS序列中的值
    const NodeValue& getBFSNodeValue(size_t bfs_idx) const;
    
    // 根据BFS索引找到对应的层和层内索引
    std::pair<size_t, size_t> bfsToLayerIndex(size_t bfs_idx) const;

    // 新增：单层序列化/反序列化
    void serializeLayer(size_t layer_idx, std::ostream& out) const;
    void deserializeLayer(size_t layer_idx, std::istream& in);

private:
    std::vector<std::vector<NodeValue>> layers_;
};

class LOUDSTrie {
public:
    LOUDSTrie();
    explicit LOUDSTrie(const std::vector<FieldKey>& field_order);
    // 构建 LOUDS Trie - 返回根节点子节点数量
    size_t buildFromTrie(const Trie& trie);

    // 查询/遍历接口
    size_t nodeCount() const;
    size_t layerCount() const { return layered_storage_.getLayerCount(); }
    
    // 获取节点内容（通过BFS索引）
    const NodeValue& getNodeValue(size_t bfs_idx) const;
    
    // 结构导航接口 - 使用SDSL的LOUDS树
    bool hasChild(size_t node_idx) const;
    size_t firstChild(size_t node_idx) const;
    size_t nextSibling(size_t node_idx) const;
    size_t parent(size_t node_idx) const;
    
    // 层访问接口
    const std::vector<NodeValue>& getLayer(size_t layer_idx) const;
    const NodeValue& getLayerNodeValue(size_t layer_idx, size_t node_idx) const;
    
    // 调试接口
    const sdsl::bit_vector& getLoudsBv() const { return louds_bv_; }
    const LayeredNodeStorage& getLayeredStorage() const { return layered_storage_; }
    LayeredNodeStorage& getLayeredStorage();
    const sdsl::select_support_mcl<0>& getLoudsSelect0() const { return louds_select0_; }
    const sdsl::rank_support_v<>& getLoudsRank() const { return louds_rank_; }
    const sdsl::select_support_mcl<>& getLoudsSelect() const { return louds_select_; }
    
    // 获取字段顺序（用于重建）
    const std::vector<FieldKey>& getFieldOrder() const { return field_order_; }

    void loadFromSerialized(const std::vector<bool>& bv, const std::vector<std::vector<NodeValue>>& all_layers, const std::vector<FieldKey>& field_order);

    // LOUDS位图序列化/反序列化
    void serializeBitmap(std::ostream& out) const;
    void deserializeBitmap(std::istream& in);

    // 新增：手动设置field_order_
    void setFieldOrder(const std::vector<FieldKey>& field_order);

private:
    // LOUDS结构部分
    sdsl::bit_vector louds_bv_;                    // LOUDS位图
    sdsl::rank_support_v<> louds_rank_;            // Rank支持
    sdsl::select_support_mcl<> louds_select_;
    sdsl::select_support_mcl<0> louds_select0_;
    sdsl::rank_support_v<0> louds_rank0_;
    
    // 分层内容存储
    LayeredNodeStorage layered_storage_;           // 分层节点存储
    
    // 元数据
    size_t total_nodes_;                           // 总节点数
    std::vector<FieldKey> field_order_;            // 字段顺序（用于重建）
    
    // 辅助方法
    void buildLoudsStructure(const Trie& trie);
    void buildLoudsRecursive(const TrieNode* node, std::vector<bool>& louds_bits);
    void buildMultiLayerLoudsRecursive(const TrieNode* node, std::vector<bool>& louds_bits, size_t depth);
    void buildLayeredContent(const Trie& trie);
    void buildLayeredContentRecursive(const TrieNode* node, size_t depth, size_t bfs_idx);
    void buildLayeredContentByField(const Trie& trie);
    void buildLayeredContentByFieldRecursive(const TrieNode* node, size_t depth, size_t bfs_idx);
    void buildLayeredContentByField_BFS(const Trie& trie);  // 新增BFS方式构建分层内容的函数声明
};

} // namespace json2
