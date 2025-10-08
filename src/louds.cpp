#include "../include/louds.h"
#include <queue>
#include <sdsl/io.hpp>
#include <variant>
#include <string>
#include <cstdint>
#include <cassert>
#include <algorithm>

namespace json2 {

// === LayeredNodeStorage 实现 ===

void LayeredNodeStorage::addLayer(const FieldKey& /*field_key*/, size_t /*start_offset*/) {
    layers_.emplace_back();
}

void LayeredNodeStorage::addNodeValue(const NodeValue& value, size_t layer_idx) {
    if (layers_.empty() || layer_idx >= layers_.size()) return;
    layers_[layer_idx].push_back(value);
}

const NodeValue& LayeredNodeStorage::getNodeValue(size_t layer_idx, size_t node_idx) const {
    assert(layer_idx < layers_.size());
    assert(node_idx < layers_[layer_idx].size());
    return layers_[layer_idx][node_idx];
}

const std::vector<NodeValue>& LayeredNodeStorage::getLayer(size_t layer_idx) const {
    assert(layer_idx < layers_.size());
    return layers_[layer_idx];
}

const NodeValue& LayeredNodeStorage::getBFSNodeValue(size_t bfs_idx) const {
    auto [layer_idx, node_idx] = bfsToLayerIndex(bfs_idx);
    return getNodeValue(layer_idx, node_idx);
}

std::pair<size_t, size_t> LayeredNodeStorage::bfsToLayerIndex(size_t bfs_idx) const {
    size_t current_idx = 0;
    size_t layer_idx = 0;
    size_t node_idx = 0;
    for (size_t i = 0; i < layers_.size(); ++i) {
        const auto& layer = layers_[i];
        if (bfs_idx >= current_idx && bfs_idx < current_idx + layer.size()) {
            layer_idx = i;
            node_idx = bfs_idx - current_idx;
            break;
        }
        current_idx += layer.size();
    }
    if (layer_idx >= layers_.size()) {
        layer_idx = layers_.size() - 1;
        node_idx = layers_[layer_idx].size() - 1;
    }
    return {layer_idx, node_idx};
}

size_t LayeredNodeStorage::layerIndexToBFS(size_t layer_idx, size_t node_idx_in_layer) const {
    // 验证输入参数
    if (layer_idx >= layers_.size()) {
        return 0; // 或者抛出异常
    }
    
    const auto& layer = layers_[layer_idx];
    if (node_idx_in_layer >= layer.size()) {
        return 0; // 或者抛出异常
    }
    
    // 计算BFS索引：前面所有层的节点数之和 + 当前层内的索引
    size_t bfs_idx = 0;
    for (size_t i = 0; i < layer_idx; ++i) {
        bfs_idx += layers_[i].size();
    }
    bfs_idx += node_idx_in_layer;
    
    return bfs_idx;
}

void LayeredNodeStorage::serialize(std::ostream& out) const {
    size_t layer_count = layers_.size();
    out.write(reinterpret_cast<const char*>(&layer_count), sizeof(layer_count));
    for (const auto& layer : layers_) {
        size_t node_count = layer.size();
        out.write(reinterpret_cast<const char*>(&node_count), sizeof(node_count));
        for (const auto& val : layer) {
            uint8_t type_byte = 0;
            if (std::holds_alternative<uint32_t>(val)) type_byte = 0;
            else if (std::holds_alternative<int64_t>(val)) type_byte = 1;
            else if (std::holds_alternative<double>(val)) type_byte = 2;
            else if (std::holds_alternative<bool>(val)) type_byte = 3;
            else if (std::holds_alternative<std::nullptr_t>(val)) type_byte = 4;
                    else if (std::holds_alternative<TemplateEncodedTimestamp>(val)) type_byte = 5;
            else if (std::holds_alternative<EncodedLog>(val)) type_byte = 6;
            out.write(reinterpret_cast<const char*>(&type_byte), 1);
            switch (type_byte) {
                case 0: { uint32_t v = std::get<uint32_t>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
                case 1: { int64_t v = std::get<int64_t>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
                case 2: { double v = std::get<double>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
                case 3: { bool v = std::get<bool>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
                case 4: break; // nullptr
            case 5: { const auto& ts = std::get<TemplateEncodedTimestamp>(val); out.write(reinterpret_cast<const char*>(&ts.template_id), sizeof(ts.template_id)); uint32_t n = static_cast<uint32_t>(ts.var_codes.size()); out.write(reinterpret_cast<const char*>(&n), sizeof(n)); for (uint32_t code : ts.var_codes) { out.write(reinterpret_cast<const char*>(&code), sizeof(code)); } break; }
                case 6: { const auto& log = std::get<EncodedLog>(val); out.write(reinterpret_cast<const char*>(&log.template_id), sizeof(log.template_id)); uint32_t n = static_cast<uint32_t>(log.var_codes.size()); out.write(reinterpret_cast<const char*>(&n), sizeof(n)); for (uint32_t code : log.var_codes) { out.write(reinterpret_cast<const char*>(&code), sizeof(code)); } break; }
            }
        }
    }
}

void LayeredNodeStorage::deserialize(std::istream& in) {
    layers_.clear();
    size_t layer_count;
    in.read(reinterpret_cast<char*>(&layer_count), sizeof(layer_count));
    for (size_t i = 0; i < layer_count; ++i) {
        std::vector<NodeValue> layer;
        size_t node_count;
        in.read(reinterpret_cast<char*>(&node_count), sizeof(node_count));
        for (size_t j = 0; j < node_count; ++j) {
            uint8_t type_byte;
            in.read(reinterpret_cast<char*>(&type_byte), 1);
            switch (type_byte) {
                case 0: { uint32_t v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
                case 1: { int64_t v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
                case 2: { double v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
                case 3: { bool v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
                case 4: { layer.emplace_back(std::nullptr_t{}); break; }
                case 5: { uint32_t template_id; in.read(reinterpret_cast<char*>(&template_id), sizeof(template_id)); uint32_t n; in.read(reinterpret_cast<char*>(&n), sizeof(n)); std::vector<uint32_t> var_codes(n); for (uint32_t& code : var_codes) { in.read(reinterpret_cast<char*>(&code), sizeof(code)); } layer.emplace_back(TemplateEncodedTimestamp{template_id, var_codes}); break; }
                case 6: { uint32_t template_id; in.read(reinterpret_cast<char*>(&template_id), sizeof(template_id)); uint32_t n; in.read(reinterpret_cast<char*>(&n), sizeof(n)); std::vector<uint32_t> var_codes(n); for (uint32_t& code : var_codes) { in.read(reinterpret_cast<char*>(&code), sizeof(code)); } layer.emplace_back(EncodedLog{template_id, var_codes}); break; }
                default: layer.emplace_back(std::nullptr_t{}); break; }
        }
        layers_.push_back(std::move(layer));
    }
}

void LayeredNodeStorage::serializeLayer(size_t layer_idx, std::ostream& out) const {
    if (layer_idx >= layers_.size()) return;
    const auto& layer = layers_[layer_idx];
    size_t node_count = layer.size();
    out.write(reinterpret_cast<const char*>(&node_count), sizeof(node_count));
    for (const auto& val : layer) {
        uint8_t type_byte = 0;
        if (std::holds_alternative<uint32_t>(val)) type_byte = 0;
        else if (std::holds_alternative<int64_t>(val)) type_byte = 1;
        else if (std::holds_alternative<double>(val)) type_byte = 2;
        else if (std::holds_alternative<bool>(val)) type_byte = 3;
        else if (std::holds_alternative<std::nullptr_t>(val)) type_byte = 4;
        else if (std::holds_alternative<TemplateEncodedTimestamp>(val)) type_byte = 5;
        else if (std::holds_alternative<EncodedLog>(val)) type_byte = 6;
        out.write(reinterpret_cast<const char*>(&type_byte), 1);
        switch (type_byte) {
            case 0: { uint32_t v = std::get<uint32_t>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
            case 1: { int64_t v = std::get<int64_t>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
            case 2: { double v = std::get<double>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
            case 3: { bool v = std::get<bool>(val); out.write(reinterpret_cast<const char*>(&v), sizeof(v)); break; }
            case 4: break; // nullptr
            case 5: { const auto& ts = std::get<TemplateEncodedTimestamp>(val); out.write(reinterpret_cast<const char*>(&ts.template_id), sizeof(ts.template_id)); uint32_t n = static_cast<uint32_t>(ts.var_codes.size()); out.write(reinterpret_cast<const char*>(&n), sizeof(n)); for (uint32_t code : ts.var_codes) { out.write(reinterpret_cast<const char*>(&code), sizeof(code)); } break; }
            case 6: { const auto& log = std::get<EncodedLog>(val); out.write(reinterpret_cast<const char*>(&log.template_id), sizeof(log.template_id)); uint32_t n = static_cast<uint32_t>(log.var_codes.size()); out.write(reinterpret_cast<const char*>(&n), sizeof(n)); for (uint32_t code : log.var_codes) { out.write(reinterpret_cast<const char*>(&code), sizeof(code)); } break; }
        }
    }
}

void LayeredNodeStorage::deserializeLayer(size_t layer_idx, std::istream& in) {
    if (layer_idx >= layers_.size()) return;
    std::vector<NodeValue>& layer = layers_[layer_idx];
    layer.clear();
    size_t node_count;
    in.read(reinterpret_cast<char*>(&node_count), sizeof(node_count));
    for (size_t j = 0; j < node_count; ++j) {
        uint8_t type_byte;
        in.read(reinterpret_cast<char*>(&type_byte), 1);
        switch (type_byte) {
            case 0: { uint32_t v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
            case 1: { int64_t v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
            case 2: { double v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
            case 3: { bool v; in.read(reinterpret_cast<char*>(&v), sizeof(v)); layer.emplace_back(v); break; }
            case 4: { layer.emplace_back(std::nullptr_t{}); break; }
                            case 5: { uint32_t template_id; in.read(reinterpret_cast<char*>(&template_id), sizeof(template_id)); uint32_t n; in.read(reinterpret_cast<char*>(&n), sizeof(n)); std::vector<uint32_t> var_codes(n); for (uint32_t& code : var_codes) { in.read(reinterpret_cast<char*>(&code), sizeof(code)); } layer.emplace_back(TemplateEncodedTimestamp{template_id, var_codes}); break; }
            case 6: { uint32_t template_id; in.read(reinterpret_cast<char*>(&template_id), sizeof(template_id)); uint32_t n; in.read(reinterpret_cast<char*>(&n), sizeof(n)); std::vector<uint32_t> var_codes(n); for (uint32_t& code : var_codes) { in.read(reinterpret_cast<char*>(&code), sizeof(code)); } layer.emplace_back(EncodedLog{template_id, var_codes}); break; }
            default: layer.emplace_back(std::nullptr_t{}); break; }
    }
}

// === LOUDSTrie 实现 ===

size_t LOUDSTrie::buildFromTrie(const Trie& trie) {
    // 清空现有数据
    layered_storage_ = LayeredNodeStorage();
    field_order_ = trie.getOrderedFields();
    
    const TrieNode* root = trie.getRoot();
    if (!root) return 0;
    
    // 构建LOUDS结构（同时会构建分层内容）
    buildLoudsStructure(trie);
    
    // 统计总节点数
    total_nodes_ = 0;
    for (size_t i = 0; i < layered_storage_.getLayerCount(); ++i) {
        total_nodes_ += layered_storage_.getLayer(i).size();
    }
    
    return root->getChildren().size();
}

void LOUDSTrie::buildLoudsStructure(const Trie& trie) {
    // 构建LOUDS位图
    std::vector<bool> louds_bits;
    const TrieNode* root = trie.getRoot();
    if (!root) return;

    // 先为根节点的所有子节点加 1
    size_t root_child_count = root->getChildren().size();
    for (size_t i = 0; i < root_child_count; ++i) louds_bits.push_back(1);
    louds_bits.push_back(0);

    // 初始化分层存储（不再有ROOT层）
    layered_storage_ = LayeredNodeStorage();
    for (size_t i = 0; i < field_order_.size(); ++i) {
        layered_storage_.addLayer(field_order_[i], 0);
    }

    // BFS遍历：构建LOUDS位图，同时填充分层内容
    std::queue<std::pair<const TrieNode*, size_t>> q; // node, depth
    for (const auto& child : root->getChildren()) {
        q.push({child.second.get(), 0});
    }
    while (!q.empty()) {
        auto [node, depth] = q.front(); q.pop();
        const auto& children = node->getChildren();
        size_t child_count = children.size();
        // 构建LOUDS位图
        for (size_t i = 0; i < child_count; ++i) louds_bits.push_back(1);
        louds_bits.push_back(0);
        // 分层内容：只写当前层的NodeValue
        if (depth < field_order_.size()) {
            if (!node->getPath().empty()) {
                layered_storage_.addNodeValue(node->getPath()[0], depth);
            } else {
                layered_storage_.addNodeValue(std::nullptr_t{}, depth);
            }
        }
        // 入队所有子节点
        for (const auto& child : children) {
            q.push({child.second.get(), depth + 1});
        }
    }

    // 构建SDSL位向量
    louds_bv_ = sdsl::bit_vector(louds_bits.size());
    for (size_t i = 0; i < louds_bits.size(); ++i) {
        louds_bv_[i] = louds_bits[i];
    }
    // 构建Rank/Select支持
    louds_rank1_ = sdsl::rank_support_v<>(&louds_bv_);
    louds_select1_ = sdsl::select_support_mcl<>(&louds_bv_);
    louds_select0_ = sdsl::select_support_mcl<0>(&louds_bv_);
    louds_rank0_ = sdsl::rank_support_v<0>(&louds_bv_);

    // 统计总节点数
    total_nodes_ = 0;
    for (size_t i = 0; i < layered_storage_.getLayerCount(); ++i) {
        total_nodes_ += layered_storage_.getLayer(i).size();
    }
}

// 新增：递归构建LOUDS位图的辅助方法
void LOUDSTrie::buildLoudsRecursive(const TrieNode* node, std::vector<bool>& louds_bits) {
    const auto& children = node->getChildren();
    
    // 为当前节点的所有子节点添加标记
    for (const auto& child : children) {
        louds_bits.push_back(1); // 有子节点
        // 递归处理子节点
        buildLoudsRecursive(child.second.get(), louds_bits);
    }
    
    // 如果没有子节点，添加0标记
    if (children.empty()) {
        louds_bits.push_back(0); // 无子节点
    }
}

// 新增：支持多层路径的递归LOUDS构建
void LOUDSTrie::buildMultiLayerLoudsRecursive(const TrieNode* node, std::vector<bool>& louds_bits, size_t depth) {
    const auto& children = node->getChildren();
    
    // 处理当前节点的路径（可能跨多层）
    const auto& path = node->getPath();
    for (size_t i = 0; i < path.size(); ++i) {
        // 为路径中的每个元素创建LOUDS节点
        if (i == 0) {
            // 第一个元素：检查是否有子节点
            louds_bits.push_back(!children.empty() ? 1 : 0);
        } else {
            // 后续元素：总是有子节点（下一个路径元素）
            louds_bits.push_back(1);
        }
    }
    
    // 递归处理所有子节点
    for (const auto& child : children) {
        buildMultiLayerLoudsRecursive(child.second.get(), louds_bits, depth + path.size());
    }
}

void LOUDSTrie::buildLayeredContent(const Trie& trie) {
    const TrieNode* root = trie.getRoot();
    if (!root) return;
    
    // 递归构建分层内容
    buildLayeredContentRecursive(root, 0, 0);
    
    // 计算总节点数：所有层的节点数之和
    total_nodes_ = 0;
    for (size_t i = 0; i < layered_storage_.getLayerCount(); ++i) {
        total_nodes_ += layered_storage_.getLayer(i).size();
    }
}

// 新增：递归构建分层内容的辅助方法
void LOUDSTrie::buildLayeredContentRecursive(const TrieNode* node, size_t depth, size_t bfs_idx) {
        const auto& children = node->getChildren();
    
        for (const auto& child : children) {
            const auto& path = child.second->getPath();
            
        // 处理路径中的每个元素（可能跨多层）
        for (size_t i = 0; i < path.size(); ++i) {
            size_t current_depth = depth + i;
            
            // 确保层存在
            while (current_depth >= layered_storage_.getLayerCount()) {
                if (current_depth < field_order_.size()) {
                    layered_storage_.addLayer(field_order_[current_depth], bfs_idx);
                } else {
                    // 如果字段顺序不够，创建默认层
                    FieldKey default_key{"unknown", FieldType::String};
                    layered_storage_.addLayer(default_key, bfs_idx);
                }
                }
                
            // 添加节点值到对应层
            layered_storage_.addNodeValue(path[i], current_depth);
                bfs_idx++;
            }
            
        // 递归处理子节点
        buildLayeredContentRecursive(child.second.get(), depth + path.size(), bfs_idx);
        }
    }
    
// 新增：BFS方式按字段顺序构建分层内容
void LOUDSTrie::buildLayeredContentByField_BFS(const Trie& trie) {
    const TrieNode* root = trie.getRoot();
    if (!root) return;

    // 第三步：按BFS顺序填充节点值（使用预计算的start_offset）
    std::cout << "\n=== 分层内容构建调试信息 ===\n";
    std::vector<size_t> current_layer_indices(field_order_.size(), 0);  // 每层当前填充位置

    std::queue<std::pair<const TrieNode*, size_t>> q;  // <node, depth>
    for (const auto& child : root->getChildren()) {
        q.push({child.second.get(), 0});
    }

    while (!q.empty()) {
        auto [node, depth] = q.front(); q.pop();
        if (depth >= field_order_.size()) continue;

        const auto& path = node->getPath();
        if (!path.empty()) {
            std::cout << "添加节点到层 " << depth << " [" 
                     << field_order_[depth].name << "], 值=";
            std::visit([](const auto& v) {
                if constexpr (!std::is_same_v<std::decay_t<decltype(v)>, std::nullptr_t>) {
                    std::cout << v;
                } else {
                    std::cout << "null";
                }
            }, path[0]);
            std::cout << std::endl;

            // 在当前层的正确位置添加值
            layered_storage_.addNodeValue(path[0], depth);
            current_layer_indices[depth]++;
        }

        // 入队所有子节点
        for (const auto& child : node->getChildren()) {
            q.push({child.second.get(), depth + 1});
        }
    }

    // 统计总节点数
    total_nodes_ = 0;
    for (size_t i = 0; i < layered_storage_.getLayerCount(); ++i) {
        total_nodes_ += layered_storage_.getLayer(i).size();
    }
}

// 修正：基于字段名称的分层构建方法
void LOUDSTrie::buildLayeredContentByField(const Trie& trie) {
    const TrieNode* root = trie.getRoot();
    if (!root) return;

    // 初始化所有层，每层对应一个字段
    for (size_t i = 0; i < field_order_.size(); ++i) {
        layered_storage_.addLayer(field_order_[i], 0);
    }

    // 只递归根节点的子节点
    for (const auto& child : root->getChildren()) {
        buildLayeredContentByFieldRecursive(child.second.get(), 0, 0);
    }

    // 计算总节点数：所有层的节点数之和
    total_nodes_ = 0;
    for (size_t i = 0; i < layered_storage_.getLayerCount(); ++i) {
        total_nodes_ += layered_storage_.getLayer(i).size();
    }
}

// 修正：基于字段名称的递归构建方法
void LOUDSTrie::buildLayeredContentByFieldRecursive(const TrieNode* node, size_t depth, size_t bfs_idx) {
    // 确保当前层存在
    while (depth >= layered_storage_.getLayerCount() && depth < field_order_.size()) {
        layered_storage_.addLayer(field_order_[depth], bfs_idx);
    }
    // 加入实际值或占位符
    const auto& path = node->getPath();
    if (!path.empty()) {
        // 如果节点有path值，使用实际值
        layered_storage_.addNodeValue(path[0], depth);
    } else {
        // 如果是根节点或path为空，添加nullptr作为占位符
        layered_storage_.addNodeValue(std::nullptr_t{}, depth);
    }
    bfs_idx++;
    // 递归处理子节点
    for (const auto& child : node->getChildren()) {
        buildLayeredContentByFieldRecursive(child.second.get(), depth + 1, bfs_idx);
    }
}

// 查询接口实现
size_t LOUDSTrie::nodeCount() const {
    // LOUDS节点数 = LOUDS位图中1的数量
    return louds_rank1_(louds_bv_.size());
}

const NodeValue& LOUDSTrie::getNodeValue(size_t bfs_idx) const {
    return layered_storage_.getBFSNodeValue(bfs_idx);
}

LayeredNodeStorage& LOUDSTrie::getLayeredStorage() {
    return layered_storage_;
}

// 标准LOUDS树导航实现
bool LOUDSTrie::hasChild(size_t node_idx) const {
    if (node_idx >= nodeCount()) {
        return false;
    }
    size_t pos = louds_select0_(node_idx + 1);
    return (pos + 1 < louds_bv_.size() && louds_bv_[pos + 1]);
}

size_t LOUDSTrie::parent(size_t node_idx) const {
    if (node_idx == 0 || node_idx >= nodeCount()) {
        return nodeCount();
    }
    
    // In LOUDS, to find the parent of node_idx:
    // 1. Find the position of node_idx's 1 bit in the bit vector
    size_t node_pos = louds_select1_(node_idx + 1);
    
    // 2. Find the nearest 0 before this position (marks end of parent's children)
    //    We can do this by finding the rank of 0s up to this position
    size_t zero_rank = louds_rank0_(node_pos);
    
    // 3. The parent's index is zero_rank - 1
    if (zero_rank > 0) {
        return zero_rank - 1;
    }
    
    return nodeCount(); // Should not happen in a valid LOUDS structure
}

size_t LOUDSTrie::firstChild(size_t node_idx) const {
    if (!hasChild(node_idx)) {
        return nodeCount();
    }
    
    // In LOUDS, to find the first child of node_idx:
    // 1. Find the position of the 0 that marks the end of this node's children
    size_t pos = louds_select0_(node_idx + 1);
    
    // 2. The first child is the node represented by the first 1 after this position
    // 3. Its index is the rank of 1s up to (but not including) this position+1
    return louds_rank1_(pos + 1);
}

size_t LOUDSTrie::nextSibling(size_t node_idx) const {
    if (node_idx >= nodeCount()) {
        return nodeCount();
    }
    
    // In LOUDS, to find the next sibling of node_idx:
    // 1. Find the position of the 0 that marks the end of this node's children
    size_t pos = louds_select0_(node_idx + 1);
    
    // 2. Check if there's a next sibling (if the next bit is 1)
    if (pos + 1 >= louds_bv_.size() || louds_bv_[pos + 1] == 0) {
        return nodeCount(); // No next sibling
    }
    
    // 3. In LOUDS, siblings are consecutive in the BFS node ordering
    //    So the next sibling of node_idx is simply node_idx + 1
    //    But we need to verify they have the same parent
    if (node_idx + 1 < nodeCount()) {
        if (parent(node_idx + 1) == parent(node_idx)) {
            return node_idx + 1;
        }
    }
    
    return nodeCount();
}

const std::vector<NodeValue>& LOUDSTrie::getLayer(size_t layer_idx) const {
    return layered_storage_.getLayer(layer_idx);
}

const NodeValue& LOUDSTrie::getLayerNodeValue(size_t layer_idx, size_t node_idx) const {
    return layered_storage_.getNodeValue(layer_idx, node_idx);
}

void LOUDSTrie::loadFromSerialized(const std::vector<bool>& bv, const std::vector<std::vector<NodeValue>>& all_layers, const std::vector<FieldKey>& field_order) {
    // 1. 设置 field_order_
    field_order_ = field_order;
    // 2. 设置 LOUDS 位图
    louds_bv_ = sdsl::bit_vector(bv.size());
    for (size_t i = 0; i < bv.size(); ++i) {
        louds_bv_[i] = bv[i];
    }
    // 3. 构建 Rank/Select 支持
    louds_rank1_ = sdsl::rank_support_v<>(&louds_bv_);
    louds_select1_ = sdsl::select_support_mcl<>(&louds_bv_);
    louds_select0_ = sdsl::select_support_mcl<0>(&louds_bv_);
    louds_rank0_ = sdsl::rank_support_v<0>(&louds_bv_);
    // 4. 重建分层内容
    layered_storage_ = LayeredNodeStorage();
    for (size_t l = 0; l < all_layers.size(); ++l) {
        FieldKey fk = (l < field_order.size()) ? field_order[l] : FieldKey{"unknown", FieldType::String};
        layered_storage_.addLayer(fk, 0);
        for (const auto& val : all_layers[l]) {
            layered_storage_.addNodeValue(val, l);
        }
    }
    // 5. 统计总节点数
    total_nodes_ = 0;
    for (size_t i = 0; i < layered_storage_.getLayerCount(); ++i) {
        total_nodes_ += layered_storage_.getLayer(i).size();
    }
}

void LOUDSTrie::serializeBitmap(std::ostream& out) const {
    // 直接使用 SDSL 的高效序列化
    louds_bv_.serialize(out);
}

void LOUDSTrie::deserializeBitmap(std::istream& in) {
    // 直接使用 SDSL 的高效反序列化
    louds_bv_.load(in);
    
    // 重建辅助索引结构
    louds_rank1_ = sdsl::rank_support_v<>(&louds_bv_);
    louds_select1_ = sdsl::select_support_mcl<>(&louds_bv_);
    louds_select0_ = sdsl::select_support_mcl<0>(&louds_bv_);
    louds_rank0_ = sdsl::rank_support_v<0>(&louds_bv_);
}

// 新增：手动设置field_order_
void LOUDSTrie::setFieldOrder(const std::vector<FieldKey>& field_order) {
    field_order_ = field_order;
}

LOUDSTrie::LOUDSTrie() = default;
LOUDSTrie::LOUDSTrie(const std::vector<FieldKey>& field_order)
    : field_order_(field_order) {}

// ========== 路径重建算法实现 ==========

std::vector<size_t> LOUDSTrie::reconstructPathToRoot(size_t bfs_idx) const {
    if (!isValidBFSIndex(bfs_idx)) {
        return {};
    }
    
    std::vector<size_t> path;
    size_t current_idx = bfs_idx;
    
    // 向上遍历到根节点
    while (current_idx < nodeCount()) {
        // 添加当前节点的BFS索引
        path.push_back(current_idx);
        
        // 移动到父节点
        current_idx = parent(current_idx);
    }
    
    // 反转路径（从根到目标节点）
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<NodeValue> LOUDSTrie::reconstructPathToDepth(size_t bfs_idx, size_t target_depth) const {
    if (!isValidBFSIndex(bfs_idx)) {
        return {};
    }
    
    return reconstructPathUpward(bfs_idx, target_depth);
}

std::vector<std::vector<NodeValue>> LOUDSTrie::reconstructMultiplePaths(
    const std::vector<size_t>& bfs_indices) const {
    
    std::vector<std::vector<NodeValue>> paths;
    paths.reserve(bfs_indices.size());
    
    for (size_t bfs_idx : bfs_indices) {
        // 获取BFS索引路径，然后转换为NodeValue路径
        std::vector<size_t> bfs_path = reconstructPathToRoot(bfs_idx);
        std::vector<NodeValue> node_value_path;
        node_value_path.reserve(bfs_path.size());
        
        for (size_t idx : bfs_path) {
            node_value_path.push_back(getNodeValue(idx));
        }
        
        paths.push_back(std::move(node_value_path));
    }
    
    return paths;
}

// ========== 中间节点路径集合重建算法 ==========

std::vector<std::vector<size_t>> LOUDSTrie::reconstructPathsFromIntermediateNode(size_t bfs_idx) const {
    if (!isValidBFSIndex(bfs_idx)) {
        return {};
    }
    
    std::vector<std::vector<size_t>> all_paths;
    
    // 优化：预先分配空间以减少重新分配
    all_paths.reserve(100); // 根据实际情况调整预估容量
    
    // 1. 重建从根到当前节点的完整路径（包含当前节点）
    std::vector<size_t> path_prefix = reconstructPathToRoot(bfs_idx);
    
    // 2. 从当前节点开始，递归收集所有到叶子的路径
    // 创建一个新的路径，从当前节点开始
    std::vector<size_t> current_path = std::move(path_prefix);
    current_path.reserve(field_order_.size()); // 预分配路径空间
    
    collectPathsFromNode(bfs_idx, current_path, all_paths);
    
    return all_paths;
}

void LOUDSTrie::collectPathsFromNode(size_t node_idx, 
                                   std::vector<size_t>& current_path, 
                                   std::vector<std::vector<size_t>>& all_paths) const {
    if (!isValidBFSIndex(node_idx)) {
        return;
    }
    
    // 防止无限递归：检查路径长度是否超过合理范围
    if (current_path.size() > field_order_.size() * 2) {
        return; // 防止异常情况下的无限递归
    }
    
    // 如果当前节点是叶子节点，保存完整路径
    if (!hasChild(node_idx)) {
        all_paths.push_back(current_path);
        return;
    }
    
    // 遍历所有子节点 - 算法优化版本
    size_t child_idx = firstChild(node_idx);
    
    // 在LOUDS结构中，兄弟节点是连续的，且具有相同的父节点
    while (child_idx < nodeCount()) {
        // 添加子节点BFS索引到当前路径
        current_path.push_back(child_idx);
        
        // 递归处理子节点
        collectPathsFromNode(child_idx, current_path, all_paths);
        
        // 回溯：移除刚添加的子节点索引
        current_path.pop_back();
        
        // 移动到下一个兄弟节点
        size_t next_child = nextSibling(child_idx);
        // 在LOUDS中，如果next_child <= child_idx，说明没有更多兄弟节点
        if (next_child <= child_idx || next_child >= nodeCount()) {
            break; // 没有更多兄弟节点
        }
        child_idx = next_child;
    }
}

std::optional<NodeValue> LOUDSTrie::getFieldValueInPath(size_t bfs_idx, const std::string& field_name) const {
    if (!isValidBFSIndex(bfs_idx)) {
        return std::nullopt;
    }
    
    // 找到字段对应的层深度
    size_t field_depth = findFieldDepth(field_name);
    if (field_depth >= field_order_.size()) {
        return std::nullopt;
    }
    
    // 重建到该深度的路径
    std::vector<NodeValue> path = reconstructPathToDepth(bfs_idx, field_depth);
    
    // 如果路径长度足够，返回对应深度的值
    if (path.size() > field_depth) {
        return path[field_depth];
    }
    
    return std::nullopt;
}

bool LOUDSTrie::pathContainsField(size_t bfs_idx, const std::string& field_name) const {
    return getFieldValueInPath(bfs_idx, field_name).has_value();
}

std::unordered_map<std::string, NodeValue> LOUDSTrie::getPathFieldValues(size_t bfs_idx) const {
    std::unordered_map<std::string, NodeValue> field_values;
    
    if (!isValidBFSIndex(bfs_idx)) {
        return field_values;
    }
    
    // 重建完整路径（BFS索引）
    std::vector<size_t> bfs_path = reconstructPathToRoot(bfs_idx);
    
    // 将路径值与字段名映射
    for (size_t i = 0; i < std::min(bfs_path.size(), field_order_.size()); ++i) {
        const std::string& field_name = field_order_[i].name;
        field_values[field_name] = getNodeValue(bfs_path[i]);
    }
    
    return field_values;
}

// ========== 私有辅助方法实现 ==========

size_t LOUDSTrie::findFieldDepth(const std::string& field_name) const {
    for (size_t i = 0; i < field_order_.size(); ++i) {
        if (field_order_[i].name == field_name) {
            return i;
        }
    }
    return field_order_.size(); // 返回无效深度
}

std::vector<NodeValue> LOUDSTrie::reconstructPathUpward(size_t bfs_idx, size_t max_depth) const {
    std::vector<NodeValue> path;
    size_t current_idx = bfs_idx;
    size_t current_depth = 0;
    
    // 向上遍历到指定深度或根节点
    while (current_idx < nodeCount() && current_depth <= max_depth) {
        // 获取当前节点的层和层内索引
        auto [layer, layer_idx] = layered_storage_.bfsToLayerIndex(current_idx);
        
        // 获取节点值并添加到路径
        const NodeValue& node_value = getLayerNodeValue(layer, layer_idx);
        path.push_back(node_value);
        
        // 移动到父节点
        current_idx = parent(current_idx);
        current_depth++;
        
        // 如果到达根节点，停止
        if (current_idx >= nodeCount()) break;
    }
    
    // 反转路径（从根到目标节点）
    std::reverse(path.begin(), path.end());
    return path;
}

bool LOUDSTrie::isValidBFSIndex(size_t bfs_idx) const {
    return bfs_idx < nodeCount();
}

} // namespace json2
