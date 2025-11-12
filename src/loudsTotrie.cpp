#include "../include/loudsTotrie.h"
#include "../include/louds.h"
#include "../include/trie.h"
#include <vector>
#include <memory>
#include <iostream> // Added for debugging
#include <unordered_map> // Added for decodePathWithDecompressedLayerValues

namespace json2 {

// 基于 LOUDS 位图递归还原原始 Trie 树的路径（算法优化版本）
static void collectLoudsTriePaths(const LOUDSTrie& louds, size_t idx, size_t depth, 
                                  std::vector<NodeValue>& path, 
                                  std::vector<std::vector<NodeValue>>& all_paths, 
                                  size_t max_depth) {
    // 深度检查 - 算法优化：提前终止
    if (depth >= max_depth) return;
    
    // 获取节点值 - 算法优化：减少函数调用
    auto [layer, layer_idx] = louds.getLayeredStorage().bfsToLayerIndex(idx);
    const NodeValue& node_value = louds.getLayerNodeValue(layer, layer_idx);
    
    // 添加到路径
    path.push_back(node_value);
    
    // 如果是叶子节点或者达到最大深度，保存路径
    if (!louds.hasChild(idx) || depth + 1 >= max_depth) {
        all_paths.emplace_back(path);
    } else {
        // 遍历子节点 - 算法优化：直接遍历兄弟节点
        size_t child = louds.firstChild(idx);
        while (child < louds.nodeCount()) {
            collectLoudsTriePaths(louds, child, depth + 1, path, all_paths, max_depth);
            
            // 移动到下一个兄弟节点
            size_t next = louds.nextSibling(child);
            // 在LOUDS中，如果next <= child，说明没有更多兄弟节点
            if (next <= child || next >= louds.nodeCount()) {
                break;
            }
            child = next;
        }
    }
    
    // 回溯
    path.pop_back();
}

void reconstructAllTriePaths(const LOUDSTrie& louds, std::vector<std::vector<NodeValue>>& all_paths) {
    std::vector<NodeValue> path;
    // 算法优化：预先分配空间
    path.reserve(louds.getFieldOrder().size());
    
    if (louds.nodeCount() == 0) return;
    
    size_t max_depth = louds.getFieldOrder().size();
    // 算法优化：预先分配结果空间
    all_paths.reserve(louds.nodeCount());
    
    // 遍历所有根节点的子节点
    for (size_t idx = 0; idx < louds.nodeCount(); ++idx) {
        // 算法优化：只处理根节点的直接子节点
        if (louds.parent(idx) == louds.nodeCount()) { // 根节点的子节点的父节点是无效索引
            collectLoudsTriePaths(louds, idx, 0, path, all_paths, max_depth);
        }
    }
}

// 简单LOUDBitmap还原Trie结构（所有内容为nullptr）
void loudsBitmapToTrieStructure(const LOUDSTrie& louds, Trie& trie, std::vector<TrieNode*>& bfs_nodes) {
    const auto& bv = louds.getLoudsBv();
    size_t n = louds.nodeCount();
    if (n == 0) return;
    std::vector<TrieNode*> queue;
    TrieNode* root = const_cast<TrieNode*>(trie.getRoot());
    size_t idx = 0;
    size_t bv_pos = 0;
    while (bv_pos < bv.size() && bv[bv_pos]) {
        auto new_node = std::make_unique<TrieNode>(std::vector<NodeValue>{nullptr}, false);
        TrieNode* child_ptr = new_node.get();
        root->getChildren()[NodeValue(uint32_t(idx))] = std::move(new_node);
        queue.push_back(child_ptr);
        bfs_nodes.push_back(child_ptr);
        ++idx;
        ++bv_pos;
    }
    ++bv_pos;
    size_t q_front = 0;
    while (q_front < queue.size() && bv_pos < bv.size()) {
        TrieNode* parent = queue[q_front++];
        while (bv_pos < bv.size() && bv[bv_pos]) {
            auto new_node = std::make_unique<TrieNode>(std::vector<NodeValue>{nullptr}, false);
            TrieNode* child_ptr = new_node.get();
            parent->getChildren()[NodeValue(uint32_t(idx))] = std::move(new_node);
            queue.push_back(child_ptr);
            bfs_nodes.push_back(child_ptr);
            ++idx;
            ++bv_pos;
        }
        ++bv_pos;
    }
}

// 填充Trie节点内容（BFS顺序）
void fillTrieNodeValuesBFS(const std::vector<TrieNode*>& bfs_nodes, const LOUDSTrie& louds) {
    for (size_t idx = 0; idx < bfs_nodes.size(); ++idx) {
        bfs_nodes[idx]->setPath({louds.getNodeValue(idx)});
    }
}

// 结构还原和内容填充同步进行
void loudsBitmapToTrieWithContent(const LOUDSTrie& louds, Trie& trie, std::vector<TrieNode*>* bfs_nodes = nullptr) {
    const auto& bv = louds.getLoudsBv();
    size_t n = louds.nodeCount();
    if (n == 0) return;
    std::vector<TrieNode*> queue;
    TrieNode* root = const_cast<TrieNode*>(trie.getRoot());
    size_t idx = 0;
    size_t bv_pos = 0;
    // 根的所有子节点
    while (bv_pos < bv.size() && bv[bv_pos]) {
        auto new_node = std::make_unique<TrieNode>(std::vector<NodeValue>{louds.getNodeValue(idx)}, false);
        TrieNode* child_ptr = new_node.get();
        root->getChildren()[NodeValue(uint32_t(idx))] = std::move(new_node);
        queue.push_back(child_ptr);
        if (bfs_nodes) bfs_nodes->push_back(child_ptr);
        ++idx;
        ++bv_pos;
    }
    ++bv_pos;
    size_t q_front = 0;
    while (q_front < queue.size() && bv_pos < bv.size()) {
        TrieNode* parent = queue[q_front++];
        while (bv_pos < bv.size() && bv[bv_pos]) {
            auto new_node = std::make_unique<TrieNode>(std::vector<NodeValue>{louds.getNodeValue(idx)}, false);
            TrieNode* child_ptr = new_node.get();
            parent->getChildren()[NodeValue(uint32_t(idx))] = std::move(new_node);
            queue.push_back(child_ptr);
            if (bfs_nodes) bfs_nodes->push_back(child_ptr);
            ++idx;
            ++bv_pos;
        }
        ++bv_pos;
    }
}

// LOUDS->Trie 还原主接口（结构+内容还原）
void loudsToTrie(const LOUDSTrie& louds, Trie& trie) {
    std::vector<TrieNode*> bfs_nodes;
    // loudsBitmapToTrieStructure(louds, trie, bfs_nodes);
    // fillTrieNodeValuesBFS(bfs_nodes, louds);
    loudsBitmapToTrieWithContent(louds, trie, &bfs_nodes);
}

// ========== 中间节点路径集合重建和解码接口 ==========

std::string decodeNodeValueToFieldValue(const FieldKey& field_key, 
                                       const NodeValue& node_value,
                                       const FieldDictionaryManager& dict_manager) {
    // 根据字段类型和NodeValue类型进行解码
    switch (field_key.type) {
        case FieldType::STRING: {
            if (std::holds_alternative<uint32_t>(node_value)) {
                // 字典编码的字符串
                auto code = std::get<uint32_t>(node_value);
                auto opt_value = dict_manager.getFieldValueByCode(field_key, code);
                if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                    return std::get<std::string>(*opt_value);
                }
            }
            return "null";
        }
        case FieldType::INT64: {
            if (std::holds_alternative<int64_t>(node_value)) {
                return std::to_string(std::get<int64_t>(node_value));
            } else if (std::holds_alternative<uint32_t>(node_value)) {
                return std::to_string(std::get<uint32_t>(node_value));
            }
            return "null";
        }
        case FieldType::DOUBLE: {
            if (std::holds_alternative<double>(node_value)) {
                return std::to_string(std::get<double>(node_value));
            }
            return "null";
        }
        case FieldType::BOOL: {
            if (std::holds_alternative<bool>(node_value)) {
                return std::get<bool>(node_value) ? "true" : "false";
            }
            return "null";
        }
        case FieldType::TIMESTAMP: {
            if (std::holds_alternative<TemplateEncodedTimestamp>(node_value)) {
                const auto& ts = std::get<TemplateEncodedTimestamp>(node_value);
                // 使用字典管理器正确解码时间戳
                return dict_manager.timestampDict().decodeTemplate(field_key, ts);
            }
            return "null";
        }
        case FieldType::LOGTYPE: {
            if (std::holds_alternative<EncodedLog>(node_value)) {
                const auto& log = std::get<EncodedLog>(node_value);
                // 使用字典管理器正确解码日志类型
                return dict_manager.logtypeDict().decodeLogToString(field_key, log);
            }
            return "null";
        }
        case FieldType::ARRAY: {
            if (std::holds_alternative<uint32_t>(node_value)) {
                auto code = std::get<uint32_t>(node_value);
                auto opt_value = dict_manager.getFieldValueByCode(field_key, code);
                if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                    return std::get<std::string>(*opt_value);
                }
            }
            return "null";
        }
        default:
            return "null";
    }
}

std::vector<std::pair<size_t, const NodeValue*>> getLayerValuesFromBFSPath(
    const LOUDSTrie& louds,
    const std::vector<size_t>& bfs_path,
    const std::vector<FieldKey>& field_order) {
    
    std::vector<std::pair<size_t, const NodeValue*>> layer_values;
    layer_values.reserve(bfs_path.size());
    
    // 算法优化：缓存常用的值以减少重复计算
    const auto& layered_storage = louds.getLayeredStorage();
    
    for (size_t i = 0; i < bfs_path.size() && i < field_order.size(); ++i) {
        // 根据BFS索引计算层和偏移量
        auto [layer_idx, node_idx] = layered_storage.bfsToLayerIndex(bfs_path[i]);
        
        // 获取该层的值
        const NodeValue& node_value = louds.getLayerNodeValue(layer_idx, node_idx);
        
        layer_values.emplace_back(layer_idx, &node_value);
    }
    
    return layer_values;
}

std::vector<std::string> decodeLayerValuesToPath(
    const std::vector<std::pair<size_t, const NodeValue*>>& layer_values,
    const std::vector<FieldKey>& field_order,
    const FieldDictionaryManager& dict_manager) {
    
    std::vector<std::string> decoded_path;
    decoded_path.reserve(layer_values.size());
    
    for (size_t i = 0; i < layer_values.size() && i < field_order.size(); ++i) {
        try {
            // 算法优化：使用引用避免拷贝
            const FieldKey& field_key = field_order[i];
            const NodeValue* node_value = layer_values[i].second;
            
            // 解码当前字段的值
            std::string decoded_value = decodeNodeValueToFieldValue(
                field_key, *node_value, dict_manager);
            decoded_path.push_back(std::move(decoded_value)); // 算法优化：使用move避免拷贝
        } catch (const std::exception& e) {
            std::cerr << "Error decoding field " << field_order[i].name 
                      << " at position " << i << ": " << e.what() << std::endl;
            decoded_path.push_back("ERROR");
        }
    }
    
    return decoded_path;
}

// 根据BFS索引重建并解码路径的辅助函数
std::vector<std::vector<std::string>> reconstructAndDecodePathsFromIntermediateNode(
    const LOUDSTrie& louds, 
    size_t bfs_idx,
    const FieldDictionaryManager& dict_manager) {
    
    std::vector<std::vector<std::string>> decoded_paths;
    
    // 输入验证
    if (bfs_idx >= louds.nodeCount()) {
        std::cerr << "Error: BFS index " << bfs_idx << " is out of range (max: " 
                  << louds.nodeCount() - 1 << ")" << std::endl;
        return decoded_paths;
    }
    
    // 1. 从LOUDS重建所有路径（BFS索引路径）
    std::vector<std::vector<size_t>> bfs_paths = 
        louds.reconstructPathsFromIntermediateNode(bfs_idx);
    
    if (bfs_paths.empty()) {
        // 如果节点没有子节点，这是正常的，不需要警告
        if (louds.hasChild(bfs_idx)) {
            std::cerr << "Warning: No paths found for BFS index " << bfs_idx << " (has children but no paths)" << std::endl;
        }
        return decoded_paths;
    }
    
    // 2. 解码每条路径
    const auto& field_order = louds.getFieldOrder();
    // 算法优化：预先分配空间
    decoded_paths.reserve(bfs_paths.size());
    
    for (size_t path_idx = 0; path_idx < bfs_paths.size(); ++path_idx) {
        const auto& bfs_path = bfs_paths[path_idx];
        
        // 获取层值
        auto layer_values = getLayerValuesFromBFSPath(louds, bfs_path, field_order);
        
        // 解码层值为路径
        std::vector<std::string> decoded_path = decodeLayerValuesToPath(layer_values, field_order, dict_manager);
        
        decoded_paths.emplace_back(std::move(decoded_path));
    }
    
    return decoded_paths;
}

} // namespace json2
