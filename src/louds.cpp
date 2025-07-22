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

void LayeredNodeStorage::addLayer(const FieldKey& field_key, size_t start_offset) {
    LayerInfo layer;
    layer.field_key = field_key;
    layer.start_offset = start_offset;
    layer.node_count = 0;
    layers_.push_back(layer);
}

void LayeredNodeStorage::addNodeValue(const NodeValue& value, size_t layer_idx) {
    if (layers_.empty() || layer_idx >= layers_.size()) return;
    layers_[layer_idx].values.push_back(value);
    layers_[layer_idx].node_count++;
}

const NodeValue& LayeredNodeStorage::getNodeValue(size_t layer_idx, size_t node_idx) const {
    assert(layer_idx < layers_.size());
    assert(node_idx < layers_[layer_idx].values.size());
    return layers_[layer_idx].values[node_idx];
}

const LayeredNodeStorage::LayerInfo& LayeredNodeStorage::getLayerInfo(size_t layer_idx) const {
    assert(layer_idx < layers_.size());
    return layers_[layer_idx];
}

void LayeredNodeStorage::setLayerStartOffset(size_t layer_idx, size_t start_offset) {
    assert(layer_idx < layers_.size());
    layers_[layer_idx].start_offset = start_offset;
}

const NodeValue& LayeredNodeStorage::getBFSNodeValue(size_t bfs_idx) const {
    auto [layer_idx, node_idx] = bfsToLayerIndex(bfs_idx);
    return getNodeValue(layer_idx, node_idx);
}

std::pair<size_t, size_t> LayeredNodeStorage::bfsToLayerIndex(size_t bfs_idx) const {
    // 根据BFS遍历顺序，先遍历完一层再遍历下一层
    size_t current_idx = 0;
    size_t layer_idx = 0;
    size_t node_idx = 0;
    
    // 遍历每一层
    for (size_t i = 0; i < layers_.size(); ++i) {
        const auto& layer = layers_[i];
        // 如果bfs_idx在当前层的范围内
        if (bfs_idx >= current_idx && bfs_idx < current_idx + layer.node_count) {
            layer_idx = i;
            node_idx = bfs_idx - current_idx;
            break;
        }
        current_idx += layer.node_count;
    }
    
    // 确保不越界
    if (layer_idx >= layers_.size()) {
        layer_idx = layers_.size() - 1;
        node_idx = layers_[layer_idx].values.size() - 1;
    }
    
    return {layer_idx, node_idx};
}

void LayeredNodeStorage::serialize(std::ostream& out) const {
    size_t layer_count = layers_.size();
    out.write(reinterpret_cast<const char*>(&layer_count), sizeof(layer_count));
    
    for (const auto& layer : layers_) {
        // 序列化FieldKey
        size_t name_len = layer.field_key.name.length();
        out.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
        out.write(layer.field_key.name.c_str(), name_len);
        int type_int = static_cast<int>(layer.field_key.type);
        out.write(reinterpret_cast<const char*>(&type_int), sizeof(type_int));
        
        // 序列化层信息
        out.write(reinterpret_cast<const char*>(&layer.node_count), sizeof(layer.node_count));
        out.write(reinterpret_cast<const char*>(&layer.start_offset), sizeof(layer.start_offset));
        
        // 序列化节点值
        for (const auto& val : layer.values) {
            uint8_t type_byte = 0;
            if (std::holds_alternative<uint32_t>(val)) type_byte = 0;
            else if (std::holds_alternative<int64_t>(val)) type_byte = 1;
            else if (std::holds_alternative<double>(val)) type_byte = 2;
            else if (std::holds_alternative<bool>(val)) type_byte = 3;
            else if (std::holds_alternative<std::nullptr_t>(val)) type_byte = 4;
            else if (std::holds_alternative<EncodedTimestamp>(val)) type_byte = 5;
            else if (std::holds_alternative<EncodedLog>(val)) type_byte = 6;
            
            out.write(reinterpret_cast<const char*>(&type_byte), 1);
            
            switch (type_byte) {
                case 0: {
                    uint32_t v = std::get<uint32_t>(val);
                    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
                    break;
                }
                case 1: {
                    int64_t v = std::get<int64_t>(val);
                    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
                    break;
                }
                case 2: {
                    double v = std::get<double>(val);
                    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
                    break;
                }
                case 3: {
                    bool v = std::get<bool>(val);
                    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
                    break;
                }
                case 4: break; // nullptr
                case 5: {
                    const auto& ts = std::get<EncodedTimestamp>(val);
                    out.write(reinterpret_cast<const char*>(&ts.pattern_id), sizeof(ts.pattern_id));
                    out.write(reinterpret_cast<const char*>(&ts.epoch), sizeof(ts.epoch));
                    break;
                }
                case 6: {
                    const auto& log = std::get<EncodedLog>(val);
                    out.write(reinterpret_cast<const char*>(&log.template_id), sizeof(log.template_id));
                    uint32_t n = static_cast<uint32_t>(log.var_codes.size());
                    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
                    for (uint32_t code : log.var_codes) {
                        out.write(reinterpret_cast<const char*>(&code), sizeof(code));
                    }
                    break;
                }
            }
        }
    }
}

void LayeredNodeStorage::deserialize(std::istream& in) {
    layers_.clear();
    size_t layer_count;
    in.read(reinterpret_cast<char*>(&layer_count), sizeof(layer_count));
    
    for (size_t i = 0; i < layer_count; ++i) {
        LayerInfo layer;
        
        // 反序列化FieldKey
        size_t name_len;
        in.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
        layer.field_key.name.resize(name_len);
        in.read(&layer.field_key.name[0], name_len);
        int type_int;
        in.read(reinterpret_cast<char*>(&type_int), sizeof(type_int));
        layer.field_key.type = static_cast<FieldType>(type_int);
        
        // 反序列化层信息
        in.read(reinterpret_cast<char*>(&layer.node_count), sizeof(layer.node_count));
        in.read(reinterpret_cast<char*>(&layer.start_offset), sizeof(layer.start_offset));
        
        // 反序列化节点值
        for (size_t j = 0; j < layer.node_count; ++j) {
            uint8_t type_byte;
            in.read(reinterpret_cast<char*>(&type_byte), 1);
            
            switch (type_byte) {
                case 0: {
                    uint32_t v;
                    in.read(reinterpret_cast<char*>(&v), sizeof(v));
                    layer.values.emplace_back(v);
                    break;
                }
                case 1: {
                    int64_t v;
                    in.read(reinterpret_cast<char*>(&v), sizeof(v));
                    layer.values.emplace_back(v);
                    break;
                }
                case 2: {
                    double v;
                    in.read(reinterpret_cast<char*>(&v), sizeof(v));
                    layer.values.emplace_back(v);
                    break;
                }
                case 3: {
                    bool v;
                    in.read(reinterpret_cast<char*>(&v), sizeof(v));
                    layer.values.emplace_back(v);
                    break;
                }
                case 4: {
                    layer.values.emplace_back(std::nullptr_t{});
                    break;
                }
                case 5: {
                    uint32_t pattern_id;
                    int64_t epoch;
                    in.read(reinterpret_cast<char*>(&pattern_id), sizeof(pattern_id));
                    in.read(reinterpret_cast<char*>(&epoch), sizeof(epoch));
                    layer.values.emplace_back(EncodedTimestamp{pattern_id, epoch});
                    break;
                }
                case 6: {
                    uint32_t template_id;
                    in.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
                    uint32_t n;
                    in.read(reinterpret_cast<char*>(&n), sizeof(n));
                    std::vector<uint32_t> var_codes(n);
                    for (uint32_t& code : var_codes) {
                        in.read(reinterpret_cast<char*>(&code), sizeof(code));
                    }
                    layer.values.emplace_back(EncodedLog{template_id, var_codes});
                    break;
                }
                default:
                    layer.values.emplace_back(std::nullptr_t{});
                    break;
            }
        }
        
        layers_.push_back(layer);
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
        total_nodes_ += layered_storage_.getLayerInfo(i).node_count;
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
    std::vector<size_t> layer_node_counts(field_order_.size(), 0);

    // 先计算每层的节点数（从根的所有子节点开始）
    {
        std::queue<std::pair<const TrieNode*, size_t>> q;
        for (const auto& child : root->getChildren()) {
            q.push({child.second.get(), 0});
        }
        while (!q.empty()) {
            auto [node, depth] = q.front(); q.pop();
            if (depth < field_order_.size()) {
                layer_node_counts[depth]++;
                for (const auto& child : node->getChildren()) {
                    q.push({child.second.get(), depth + 1});
                }
            }
        }
    }

    // 初始化所有层（不含ROOT）
    size_t cumulative_offset = 0;
    for (size_t i = 0; i < field_order_.size(); ++i) {
        layered_storage_.addLayer(field_order_[i], cumulative_offset);
        cumulative_offset += layer_node_counts[i];
    }

    // BFS遍历：构建LOUDS位图，同时填充分层内容
    std::cout << "\n=== 分层内容构建调试信息 ===\n";
    std::queue<std::pair<const TrieNode*, size_t>> q;
    std::vector<const TrieNode*> bfs_nodes; // 记录BFS顺序的节点指针
    std::vector<size_t> node_depths; // 记录每个节点的深度
    // 从根的所有子节点开始
    for (const auto& child : root->getChildren()) {
        q.push({child.second.get(), 0});
        bfs_nodes.push_back(child.second.get());
        node_depths.push_back(0);
    }
    while (!q.empty()) {
        auto [node, depth] = q.front(); q.pop();
        const auto& children = node->getChildren();
        size_t child_count = children.size();
        // 调试输出
        std::cout << " 节点编号: " << (bfs_nodes.size() - q.size() - 1);
        std::cout << ", 路径: ";
        for (const auto& v : node->getPath()) {
            std::visit([](auto&& arg){
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>) {
                    std::cout << arg << " ";
                } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    std::cout << "null ";
                } else if constexpr (std::is_same_v<T, json2::EncodedTimestamp>) {
                    std::cout << "EncodedTimestamp{pattern_id=" << arg.pattern_id << ", epoch=" << arg.epoch << "} ";
                } else if constexpr (std::is_same_v<T, json2::EncodedLog>) {
                    std::cout << "EncodedLog{template_id=" << arg.template_id << ", var_codes=[";
                    for (size_t i = 0; i < arg.var_codes.size(); ++i) {
                        std::cout << arg.var_codes[i];
                        if (i + 1 < arg.var_codes.size()) std::cout << ",";
                    }
                    std::cout << "]} ";
                } else {
                    std::cout << "[UnknownType] ";
                }
            }, v);
        }
        std::cout << ", 孩子数: " << child_count;
        std::cout << ", LOUDS位图下标: " << louds_bits.size() << std::endl;
        // 构建LOUDS位图
        for (size_t i = 0; i < child_count; ++i) louds_bits.push_back(1);
        louds_bits.push_back(0);
        // 入队所有子节点，并分配BFS编号和深度
        for (const auto& child : children) {
            q.push({child.second.get(), depth + 1});
            bfs_nodes.push_back(child.second.get());
            node_depths.push_back(depth + 1);
        }
    }

    // 分层内容填充：对所有BFS节点填充（不含ROOT）
    for (size_t bfs_idx = 0; bfs_idx < bfs_nodes.size(); ++bfs_idx) {
        const TrieNode* node = bfs_nodes[bfs_idx];
        size_t depth = node_depths[bfs_idx];
        const auto& path = node->getPath();
        // path 可能跨多层
        for (size_t i = 0; i < path.size(); ++i) {
            size_t layer = depth - path.size() + 1 + i;
            if (layer < field_order_.size()) {
                std::cout << "添加节点到层 " << layer << " [" << field_order_[layer].name << "], 值=";
                std::visit([](const auto& v) {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(v)>, std::nullptr_t>) {
                        std::cout << v;
                    } else {
                        std::cout << "null";
                    }
                }, path[i]);
                std::cout << std::endl;
                layered_storage_.addNodeValue(path[i], layer);
            }
        }
    }

    // 构建SDSL位向量
    louds_bv_ = sdsl::bit_vector(louds_bits.size());
    for (size_t i = 0; i < louds_bits.size(); ++i) {
        louds_bv_[i] = louds_bits[i];
    }
    // 构建Rank/Select支持
    louds_rank_ = sdsl::rank_support_v<>(&louds_bv_);
    louds_select_ = sdsl::select_support_mcl<>(&louds_bv_);
    louds_select0_ = sdsl::select_support_mcl<0>(&louds_bv_);
    louds_rank0_ = sdsl::rank_support_v<0>(&louds_bv_);
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
        total_nodes_ += layered_storage_.getLayerInfo(i).node_count;
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
        total_nodes_ += layered_storage_.getLayerInfo(i).node_count;
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
        total_nodes_ += layered_storage_.getLayerInfo(i).node_count;
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
    return louds_rank_(louds_bv_.size());
}

const NodeValue& LOUDSTrie::getNodeValue(size_t bfs_idx) const {
    return layered_storage_.getBFSNodeValue(bfs_idx);
}

// 标准LOUDS树导航实现
bool LOUDSTrie::hasChild(size_t node_idx) const {
    if (node_idx >= nodeCount()) {
        return false;
    }
    size_t pos = louds_select_(node_idx + 1);
    return (pos + 1 < louds_bv_.size() && louds_bv_[pos + 1]);
}

size_t LOUDSTrie::firstChild(size_t node_idx) const {
    if (!hasChild(node_idx)) {
        return nodeCount();
    }
    size_t pos = louds_select_(node_idx + 1);
    return louds_rank_(pos + 1);
}

size_t LOUDSTrie::nextSibling(size_t node_idx) const {
    if (node_idx + 1 >= nodeCount()) {
        return nodeCount();
    }
    size_t pos = louds_select_(node_idx + 1);
    if (pos + 1 >= louds_bv_.size() || !louds_bv_[pos + 1]) {
        return nodeCount();
    }
    return node_idx + 1;
}

size_t LOUDSTrie::parent(size_t node_idx) const {
    if (node_idx == 0 || node_idx >= nodeCount()) {
        return nodeCount();
    }
    size_t pos = louds_select_(node_idx + 1);
    size_t q = louds_rank0_(pos);
    if (q == 0) {
        return nodeCount();
    }
    size_t parent_pos = louds_select0_(q);
    size_t parent_idx = louds_rank_(parent_pos);
    return parent_idx;
}

const LayeredNodeStorage::LayerInfo& LOUDSTrie::getLayerInfo(size_t layer_idx) const {
    return layered_storage_.getLayerInfo(layer_idx);
}

const NodeValue& LOUDSTrie::getLayerNodeValue(size_t layer_idx, size_t node_idx) const {
    return layered_storage_.getNodeValue(layer_idx, node_idx);
}

// 序列化/反序列化
void LOUDSTrie::serialize(std::ostream& out) const {
    // 序列化LOUDS结构
    sdsl::serialize(louds_bv_, out);
    
    // 序列化元数据
    out.write(reinterpret_cast<const char*>(&total_nodes_), sizeof(total_nodes_));
    
    // 序列化字段顺序
    field_utils::serializeFieldList(field_order_, out);
    
    // 序列化分层存储
    layered_storage_.serialize(out);
}

void LOUDSTrie::deserialize(std::istream& in) {
    // 反序列化LOUDS结构
    sdsl::load(louds_bv_, in);
    louds_rank_ = sdsl::rank_support_v<>(&louds_bv_);
    louds_select_ = sdsl::select_support_mcl<>(&louds_bv_);
    louds_select0_ = sdsl::select_support_mcl<0>(&louds_bv_);
    louds_rank0_ = sdsl::rank_support_v<0>(&louds_bv_); // 新增
    
    // 反序列化元数据
    in.read(reinterpret_cast<char*>(&total_nodes_), sizeof(total_nodes_));
    
    // 反序列化字段顺序
    field_utils::deserializeFieldList(field_order_, in);
    
    // 反序列化分层存储
    layered_storage_.deserialize(in);
}

} // namespace json2
