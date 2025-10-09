#ifndef LOUDS_TOTRIE_H
#define LOUDS_TOTRIE_H

#include "../include/field_dictionary_manager.h"
#include "../include/trie.h"
#include "../include/louds.h"
#include <vector>
#include <string>

namespace json2 {

// 前向声明
class LOUDSTrie;
class FieldDictionaryManager;

/**
 * @brief 从LOUDS Trie重建所有Trie路径
 * @param louds LOUDS Trie结构
 * @param all_paths 输出的所有路径集合
 */
void reconstructAllTriePaths(const LOUDSTrie& louds, std::vector<std::vector<NodeValue>>& all_paths);

/**
 * @brief 将LOUDS结构还原为Trie树
 * @param louds LOUDS Trie结构
 * @param trie 输出的Trie树
 */
void loudsToTrie(const LOUDSTrie& louds, Trie& trie);

/**
 * @brief 从中间节点BFS索引重建并解码所有路径
 * @param louds LOUDS Trie结构
 * @param bfs_idx 中间节点的BFS索引
 * @param dict_manager 字典管理器，用于解码
 * @return 解码后的路径集合
 */
std::vector<std::vector<std::string>> reconstructAndDecodePathsFromIntermediateNode(
    const LOUDSTrie& louds, 
    size_t bfs_idx,
    const FieldDictionaryManager& dict_manager);

/**
 * @brief 基于节点值在特定层中查找并重建路径（压缩数据优化版本）
 * @param louds LOUDS Trie结构
 * @param target_value 目标节点值
 * @param layer_idx 层索引
 * @param dict_manager 字典管理器，用于解码
 * @return 解码后的路径集合
 */
std::vector<std::vector<std::string>> reconstructPathsFromNodeValueInLayer(
    const LOUDSTrie& louds,
    const NodeValue& target_value,
    size_t layer_idx,
    const FieldDictionaryManager& dict_manager);

/**
 * @brief 解码节点值为字段值
 * @param field_key 字段键
 * @param node_value 节点值
 * @param dict_manager 字典管理器
 * @return 解码后的字符串值
 */
std::string decodeNodeValueToFieldValue(const FieldKey& field_key, 
                                       const NodeValue& node_value,
                                       const FieldDictionaryManager& dict_manager);

/**
 * @brief 从BFS路径获取层索引和节点值
 * @param louds LOUDS Trie结构
 * @param bfs_path BFS索引路径
 * @param field_order 字段顺序
 * @return 层索引和节点值的配对向量
 */
std::vector<std::pair<size_t, const NodeValue*>> getLayerValuesFromBFSPath(
    const LOUDSTrie& louds,
    const std::vector<size_t>& bfs_path,
    const std::vector<FieldKey>& field_order);

/**
 * @brief 解码层值路径为字符串路径
 * @param layer_values 层索引和节点值的配对向量
 * @param field_order 字段顺序
 * @param dict_manager 字典管理器
 * @return 解码后的字符串路径
 */
std::vector<std::string> decodeLayerValuesToPath(
    const std::vector<std::pair<size_t, const NodeValue*>>& layer_values,
    const std::vector<FieldKey>& field_order,
    const FieldDictionaryManager& dict_manager);

} // namespace json2

#endif // LOUDS_TOTRIE_H