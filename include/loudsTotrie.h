#pragma once
#include "louds.h"
#include "trie.h"
#include <vector>

namespace json2 {

    // 收集所有根到叶路径，每条路径即一条JSON记录的编码
    void collectAllLoudsTriePaths(const LOUDSTrie& louds, std::vector<std::vector<NodeValue>>& all_paths);
    void reconstructAllTriePaths(const LOUDSTrie& louds, std::vector<std::vector<NodeValue>>& all_paths);

    // LOUDS->Trie 还原主接口
    void loudsToTrie(const LOUDSTrie& louds, Trie& trie);

} // namespace json2 