#pragma once

#include "query_parser.h"
#include "selective_decompressor.h"
#include "path_cache.h"
#include "trie.h"
#include "louds.h"
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>

namespace json2 {
namespace query {

/**
 * Trie遍历结果
 */
struct TrieTraversalResult {
    std::vector<std::vector<NodeValue>> matching_paths;  // 匹配的路径
    std::vector<std::vector<std::string>> decoded_paths; // 解码后的路径（新增）
    std::vector<size_t> path_indices;                    // 路径索引
    size_t nodes_visited;                                // 访问的节点数
    size_t paths_found;                                  // 找到的路径数
    double traversal_time_ms;                            // 遍历时间
    bool is_complete;                                    // 是否完整遍历
};

/**
 * 路径匹配条件
 */
struct PathMatchCondition {
    std::string field_name;                              // 字段名
    FieldType field_type;                                // 字段类型
    std::string expected_value;                          // 期望值
    QueryOperator operator_;                             // 操作符
    std::string min_value;                              // 最小值（范围查询）
    std::string max_value;                              // 最大值（范围查询）
    bool is_template_match;                              // 是否模板匹配
    std::string template_pattern;                        // 模板模式
};

/**
 * Trie遍历器
 * 在Trie中查找匹配的路径和值
 */
class TrieTraverser {
public:
    TrieTraverser() = default;
    explicit TrieTraverser(std::shared_ptr<PathCache> cache) : path_cache_(cache) {}
    
    /**
     * 遍历Trie查找匹配路径（使用LOUDS）
     * @param louds LOUDS结构
     * @param dict_manager 字典管理器
     * @param query_root 查询AST根节点
     * @return 遍历结果
     */
    TrieTraversalResult traverse(const LOUDSTrie& louds,
                                const FieldDictionaryManager& dict_manager,
                                const QueryNode& query_root);

    // ========== 新增：适配新查询思路的方法 ==========
    
    /**
     * 使用新查询思路进行精确匹配查询
     * @param louds LOUDS结构
     * @param dict_manager 字典管理器
     * @param field_name 字段名
     * @param field_type 字段类型
     * @param value 期望值
     * @return 遍历结果
     */
    TrieTraversalResult findExactMatchesWithNewApproach(const LOUDSTrie& louds,
                                                       const FieldDictionaryManager& dict_manager,
                                                       const std::string& field_name,
                                                       FieldType field_type,
                                                       const std::string& value);
    
    /**
     * 使用新查询思路进行范围查询
     * @param louds LOUDS结构
     * @param dict_manager 字典管理器
     * @param field_name 字段名
     * @param field_type 字段类型
     * @param min_value 最小值
     * @param max_value 最大值
     * @return 遍历结果
     */
    TrieTraversalResult findRangeMatchesWithNewApproach(const LOUDSTrie& louds,
                                                       const FieldDictionaryManager& dict_manager,
                                                       const std::string& field_name,
                                                       FieldType field_type,
                                                       const std::string& min_value,
                                                       const std::string& max_value);
    
    /**
     * 使用新查询思路进行字段存在性查询
     * @param louds LOUDS结构
     * @param field_name 字段名
     * @return 遍历结果
     */
    TrieTraversalResult findFieldExistsWithNewApproach(const LOUDSTrie& louds,
                                                      const std::string& field_name);

    // ========== 新增：缓存相关方法 ==========
    
    /**
     * 设置路径缓存
     * @param cache 路径缓存实例
     */
    void setPathCache(std::shared_ptr<PathCache> cache);
    
    /**
     * 获取路径缓存
     * @return 路径缓存实例
     */
    std::shared_ptr<PathCache> getPathCache() const;
    
    /**
     * 预热缓存
     * @param louds LOUDS结构
     * @param bfs_indices 要预热的BFS索引列表
     * @param max_items 最大预热项数
     */
    void warmupCache(const LOUDSTrie& louds, 
                    const std::vector<size_t>& bfs_indices,
                    size_t max_items = 1000);

private:
    // 遍历LOUDS结构
    void traverseLouds(const LOUDSTrie& louds,
                      const FieldDictionaryManager& dict_manager,
                      const QueryNode& query_node,
                      TrieTraversalResult& result);
    
    // 检查路径是否匹配查询条件
    bool matchesQuery(const std::vector<NodeValue>& path,
                     const FieldDictionaryManager& dict_manager,
                     const QueryNode& query_node);
    
    // 检查逻辑操作
    bool matchesLogical(const std::vector<NodeValue>& path,
                       const FieldDictionaryManager& dict_manager,
                       const QueryNode& logical_node);
    
    // 从路径中提取字段值
    std::string extractFieldValue(const std::vector<NodeValue>& path,
                                 const FieldDictionaryManager& dict_manager,
                                 const std::string& field_name,
                                 FieldType field_type);
    
    // 比较值
    bool compareValues(const std::string& actual_value,
                      const std::string& expected_value,
                      QueryOperator operator_);
    
    // 优化遍历
    void optimizeTraversal(const QueryNode& query_node, 
                          std::vector<PathMatchCondition>& conditions);
    
    // 早期终止检查
    bool shouldTerminate(const TrieTraversalResult& result, 
                        size_t max_results) const;
    
    // ========== 新增：适配新查询思路的辅助方法 ==========
    
    // 根据字段名找到对应的层索引
    size_t findFieldLayerIndex(const std::string& field_name, const LOUDSTrie& louds) const;
    
    // 解码路径值为字符串
    std::vector<std::string> decodePathValues(const std::vector<NodeValue>& path,
                                             const LOUDSTrie& louds,
                                             const FieldDictionaryManager& dict_manager) const;
    
    // ========== 新增：缓存相关私有方法 ==========
    
    // 使用缓存获取路径
    std::vector<NodeValue> getPathWithCache(size_t bfs_idx, const LOUDSTrie& louds);
    
    // 批量使用缓存获取路径
    std::unordered_map<size_t, std::vector<NodeValue>> 
    getBatchPathsWithCache(const std::vector<size_t>& bfs_indices, const LOUDSTrie& louds);
    
    // 路径缓存实例
    std::shared_ptr<PathCache> path_cache_;
};

} // namespace query
} // namespace json2