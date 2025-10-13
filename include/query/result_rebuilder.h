#pragma once

#include "trie_traverser.h"
#include "field_dictionary_manager.h"
#include "trie.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace json2 {
namespace query {

/**
 * 重建选项
 */
struct RebuildOptions {
    bool include_metadata = true;                        // 是否包含元数据
    bool format_json = true;                             // 是否格式化为JSON
    bool include_field_types = false;                    // 是否包含字段类型
    std::unordered_set<std::string> required_fields;    // 需要的字段
    size_t max_results = 1000;                           // 最大结果数
    bool sort_results = false;                           // 是否排序结果
    std::string sort_field;                              // 排序字段
    bool ascending = true;                               // 是否升序
};

/**
 * 重建结果
 */
struct RebuildResult {
    std::vector<std::string> records;                    // 重建的记录
    std::vector<std::unordered_map<std::string, std::string>> field_maps; // 字段映射
    size_t total_count;                                  // 总记录数
    size_t rebuilt_count;                                // 重建的记录数
    double rebuild_time_ms;                              // 重建时间
    std::vector<FieldKey> field_order;                   // 字段顺序
    std::unordered_map<std::string, FieldType> field_types; // 字段类型
    bool is_complete;                                    // 是否完整重建
};

/**
 * 结果重建器
 * 将匹配的编码值重建为原始JSON
 */
class ResultRebuilder {
public:
    ResultRebuilder() = default;
    
    /**
     * 重建查询结果
     * @param traversal_result Trie遍历结果
     * @param trie Trie结构
     * @param dict_manager 字典管理器
     * @param field_order 字段顺序
     * @param options 重建选项
     * @return 重建结果
     */
    RebuildResult rebuild(const TrieTraversalResult& traversal_result,
                         const Trie& trie,
                         const FieldDictionaryManager& dict_manager,
                         const std::vector<FieldKey>& field_order,
                         const RebuildOptions& options);
    
    /**
     * 重建单个路径为JSON
     * @param path 节点值路径
     * @param trie Trie结构
     * @param dict_manager 字典管理器
     * @param field_order 字段顺序
     * @param options 重建选项
     * @return JSON字符串
     */
    std::string rebuildPath(const std::vector<NodeValue>& path,
                           const Trie& trie,
                           const FieldDictionaryManager& dict_manager,
                           const std::vector<FieldKey>& field_order,
                           const RebuildOptions& options);
    
    /**
     * 重建字段映射
     * @param path 节点值路径
     * @param trie Trie结构
     * @param dict_manager 字典管理器
     * @param field_order 字段顺序
     * @return 字段映射
     */
    std::unordered_map<std::string, std::string> rebuildFieldMap(
        const std::vector<NodeValue>& path,
        const Trie& trie,
        const FieldDictionaryManager& dict_manager,
        const std::vector<FieldKey>& field_order);
    
    /**
     * 重建特定字段的值
     * @param path 节点值路径
     * @param field_name 字段名
     * @param field_type 字段类型
     * @param trie Trie结构
     * @param dict_manager 字典管理器
     * @param field_order 字段顺序
     * @return 字段值
     */
    std::string rebuildFieldValue(const std::vector<NodeValue>& path,
                                 const std::string& field_name,
                                 FieldType field_type,
                                 const Trie& trie,
                                 const FieldDictionaryManager& dict_manager,
                                 const std::vector<FieldKey>& field_order);
    
    /**
     * 批量重建结果
     * @param paths 路径列表
     * @param trie Trie结构
     * @param dict_manager 字典管理器
     * @param field_order 字段顺序
     * @param options 重建选项
     * @return 重建结果
     */
    RebuildResult rebuildBatch(const std::vector<std::vector<NodeValue>>& paths,
                              const Trie& trie,
                              const FieldDictionaryManager& dict_manager,
                              const std::vector<FieldKey>& field_order,
                              const RebuildOptions& options);

private:
    // 从节点值重建字段值
    std::string rebuildNodeValue(const NodeValue& node_value,
                                const FieldKey& field_key,
                                const FieldDictionaryManager& dict_manager);
    
    // 重建字符串字段
    std::string rebuildStringField(const NodeValue& node_value,
                                  const FieldDictionaryManager& dict_manager);
    
    // 重建数值字段
    std::string rebuildNumericField(const NodeValue& node_value,
                                   FieldType field_type);
    
    // 重建布尔字段
    std::string rebuildBooleanField(const NodeValue& node_value);
    
    // 重建时间戳字段
    std::string rebuildTimestampField(const NodeValue& node_value,
                                     const FieldDictionaryManager& dict_manager);
    
    // 重建日志类型字段
    std::string rebuildLogTypeField(const NodeValue& node_value,
                                   const FieldDictionaryManager& dict_manager);
    
    // 重建空值字段
    std::string rebuildNullField();
    
    // 构建JSON对象
    std::string buildJsonObject(const std::unordered_map<std::string, std::string>& field_map,
                               const RebuildOptions& options);
    
    // 格式化JSON
    std::string formatJson(const std::string& json_string);
    
    // 排序结果
    void sortResults(RebuildResult& result, const RebuildOptions& options);
    
    // 过滤结果
    void filterResults(RebuildResult& result, const RebuildOptions& options);
    
    // 验证重建结果
    bool validateRebuildResult(const RebuildResult& result);
    
    // 计算重建统计
    void calculateRebuildStats(RebuildResult& result);
    
    // 优化重建过程
    void optimizeRebuild(const std::vector<std::vector<NodeValue>>& paths,
                        const RebuildOptions& options);
};

} // namespace query
} // namespace json2
