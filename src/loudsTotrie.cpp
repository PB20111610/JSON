#include "../include/loudsTotrie.h"
#include "../include/louds.h"
#include "../include/trie.h"
#include <vector>
#include <memory>
#include <iostream> // Added for debugging

namespace json2 {

// 基于 LOUDS 位图递归还原原始 Trie 树的路径（只保留push_back/pop_back递归实现）
static void collectLoudsTriePaths(const LOUDSTrie& louds, size_t idx, size_t depth, std::vector<NodeValue>& path, std::vector<std::vector<NodeValue>>& all_paths, size_t max_depth) {
    if (depth >= max_depth) return;
    auto [layer, layer_idx] = louds.getLayeredStorage().bfsToLayerIndex(idx);
    path.push_back(louds.getLayerNodeValue(layer, layer_idx));
    if (!louds.hasChild(idx)) {
        all_paths.push_back(path);
    } else if (path.size() < max_depth) {
        size_t child = louds.firstChild(idx);
        // 遍历所有兄弟节点（同一父节点）
        for (size_t sib = child; sib < louds.nodeCount() && louds.parent(sib) == idx; ++sib) {
            collectLoudsTriePaths(louds, sib, depth + 1, path, all_paths, max_depth);
        }
    }
    path.pop_back();
}

void reconstructAllTriePaths(const LOUDSTrie& louds, std::vector<std::vector<NodeValue>>& all_paths) {
    std::vector<NodeValue> path;
    if (louds.nodeCount() == 0) return;
    size_t max_depth = louds.getFieldOrder().size();
    for (size_t idx = 0; idx < louds.nodeCount(); ++idx) {
        if (louds.parent(idx) == louds.nodeCount()) {
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

// 新增：结构还原和内容填充同步进行
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

} // namespace json2
