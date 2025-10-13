#pragma once
#include "trie.h"
#include <sdsl/bit_vectors.hpp>
#include <vector>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <stdexcept>  // Add this include for std::out_of_range

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
    size_t getLayerCount() const { 
        // If we have layer sizes information, use that as the layer count
        if (!layer_sizes_.empty()) {
            return layer_sizes_.size();
        }
        return layers_.size(); 
    }
    
    // 序列化/反序列化
    void serialize(std::ostream& out) const;
    void deserialize(std::istream& in);
    
    // 获取指定节点在BFS序列中的值
    const NodeValue& getBFSNodeValue(size_t bfs_idx) const;
    
    // 根据BFS索引找到对应的层和层内索引
    std::pair<size_t, size_t> bfsToLayerIndex(size_t bfs_idx) const;
    // 根据层索引和层内节点索引计算BFS索引
    size_t layerIndexToBFS(size_t layer_idx, size_t node_idx_in_layer) const;

    // 单层序列化/反序列化
    void serializeLayer(size_t layer_idx, std::ostream& out) const;
    void deserializeLayer(size_t layer_idx, std::istream& in);
    
    // 设置层大小信息（用于压缩环境下的索引计算）
    void setLayerSizes(const std::vector<size_t>& layer_sizes);
    
    // 获取层大小信息
    const std::vector<size_t>& getLayerSizes() const;

private:
    std::vector<std::vector<NodeValue>> layers_;
    // 新增：存储每层的大小信息，用于在压缩环境下正确计算索引
    std::vector<size_t> layer_sizes_;
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
    const sdsl::rank_support_v<>& getLoudsRank1() const { return louds_rank1_; }
    const sdsl::select_support_mcl<>& getLoudsSelect1() const { return louds_select1_; }
    const sdsl::rank_support_v<0>& getLoudsRank0() const { return louds_rank0_; }
    
    // 获取字段顺序（用于重建）
    const std::vector<FieldKey>& getFieldOrder() const { return field_order_; }

    void loadFromSerialized(const std::vector<bool>& bv, const std::vector<std::vector<NodeValue>>& all_layers, const std::vector<FieldKey>& field_order);

    // ========== 按需路径重建接口 ==========
    // 根据BFS索引重建到根节点的完整路径（返回BFS索引）
    std::vector<size_t> reconstructPathToRoot(size_t bfs_idx) const;
    
    // 根据BFS索引重建到指定深度的路径
    std::vector<NodeValue> reconstructPathToDepth(size_t bfs_idx, size_t target_depth) const;
    
    // 批量重建多个路径
    std::vector<std::vector<NodeValue>> reconstructMultiplePaths(
        const std::vector<size_t>& bfs_indices) const;
    
    // 获取指定字段在路径中的值
    std::optional<NodeValue> getFieldValueInPath(size_t bfs_idx, const std::string& field_name) const;
    
    // 检查路径是否包含指定字段
    bool pathContainsField(size_t bfs_idx, const std::string& field_name) const;
    
    // 获取路径中所有字段的值
    std::unordered_map<std::string, NodeValue> getPathFieldValues(size_t bfs_idx) const;
    
    // ========== 中间节点路径集合重建接口 ==========
    // 从中间节点重建所有经过该节点的完整路径（返回BFS索引路径）
    std::vector<std::vector<size_t>> reconstructPathsFromIntermediateNode(size_t bfs_idx) const;

    // LOUDS位图序列化/反序列化
    void serializeBitmap(std::ostream& out) const;
    void deserializeBitmap(std::istream& in);

    // 新增：手动设置field_order_
    void setFieldOrder(const std::vector<FieldKey>& field_order);

private:
    // LOUDS结构部分
    sdsl::bit_vector louds_bv_;                    // LOUDS位图
    sdsl::rank_support_v<> louds_rank1_;            // Rank支持
    sdsl::select_support_mcl<> louds_select1_;
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
    
    // ========== 路径重建辅助方法 ==========
    // 根据字段名找到对应的层深度
    size_t findFieldDepth(const std::string& field_name) const;
    
    // 向上重建路径到指定深度
    std::vector<NodeValue> reconstructPathUpward(size_t bfs_idx, size_t max_depth) const;
    
    // 检查BFS索引是否有效
    bool isValidBFSIndex(size_t bfs_idx) const;
    
    // 从指定节点递归收集所有路径
    void collectPathsFromNode(size_t node_idx, 
                             std::vector<size_t>& current_path, 
                             std::vector<std::vector<size_t>>& all_paths) const;
};

} // namespace json2
