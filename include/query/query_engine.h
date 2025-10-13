#pragma once

#include "query_parser.h"
#include "field_analyzer.h"
#include "chunk_selector.h"
#include "selective_decompressor.h"
#include "trie_traverser.h"
#include "result_rebuilder.h"
#include "../chunked_type_aware_compress.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace json2 {
namespace query {

// 查询配置
struct QueryConfig {
    size_t max_results = 1000;              // 最大结果数
    bool enable_parallel = false;           // 是否启用并行处理
    bool prune_only = false;                // 轻量模式（仅返回统计信息和少量采样）
    size_t sample_limit = 10;               // 轻量模式下的采样限制
};

// 查询结果
struct QueryResult {
    std::vector<std::string> records;       // 匹配的记录
    size_t count = 0;                       // 匹配的记录数
    size_t chunks_accessed = 0;             // 访问的块数
    size_t dict_hits = 0;                   // 字典命中数
    size_t trie_nodes_visited = 0;          // 访问的Trie节点数
    size_t matching_blocks = 0;             // 匹配的块数（轻量模式）
    double decompression_ratio = 0.0;       // 解压比例
    double selection_ratio = 0.0;           // 选择比例
    double query_time_ms = 0.0;             // 查询时间（毫秒）
    bool is_complete = false;               // 是否完整查询
    std::string error_message;              // 错误信息
};

// 字段存在性检查结果
struct FieldExistenceResult {
    bool exists = false;                    // 字段是否存在
    bool type_matches = false;              // 类型是否匹配
    FieldType field_type = FieldType::String; // 字段类型
    std::string error_message;              // 错误信息
};

// 字典查询结果
struct DictionaryQueryResult {
    bool found = false;                     // 是否找到
    std::vector<std::string> values;        // 值列表
    std::string error_message;              // 错误信息
};

// 记录查询结果（用于精确匹配和范围查询）
struct RecordQueryResult {
    std::vector<std::string> records;       // 匹配的记录
    size_t count = 0;                       // 匹配的记录数
    size_t chunks_accessed = 0;             // 访问的块数
    double decompression_ratio = 0.0;       // 解压比例
    double query_time_ms = 0.0;             // 查询时间（毫秒）
    bool is_complete = false;               // 是否完整查询
    std::string error_message;              // 错误信息
};

/**
 * 查询引擎
 * 负责执行各种类型的查询
 */
class QueryEngine {
public:
    explicit QueryEngine(const QueryConfig& config = QueryConfig());
    
    // ========== 优化的查询方法 ==========
    
    /**
     * 检查字段存在性和类型
     * @param field_name 字段名
     * @param expected_type 期望类型
     * @param granular_data 细粒度压缩数据
     * @return 字段存在性检查结果
     */
    FieldExistenceResult checkFieldExistenceAndType(const std::string& field_name,
                                                   FieldType expected_type,
                                                   const GranularCompressedData& granular_data);
    
    /**
     * 查询字典
     * @param field_name 字段名
     * @param field_type 字段类型
     * @param granular_data 细粒度压缩数据
     * @param target_value 目标值（空表示返回所有值）
     * @return 字典查询结果
     */
    DictionaryQueryResult queryDictionary(const std::string& field_name,
                                         FieldType field_type,
                                         const GranularCompressedData& granular_data,
                                         const std::string& target_value = "");
    
    /**
     * 执行精确匹配查询（优化版本）
     * @param field_name 字段名
     * @param exact_value 精确值
     * @param granular_data 细粒度压缩数据
     * @param chunks 数据块列表
     * @return 记录查询结果
     */
    RecordQueryResult executeExactMatchQuery(const std::string& field_name,
                                            const std::string& exact_value,
                                            const GranularCompressedData& granular_data,
                                            const std::vector<ChunkedTypeAwareBlock>& chunks);
    
    /**
     * 执行范围查询（优化版本）
     * @param field_name 字段名
     * @param min_value 最小值
     * @param max_value 最大值
     * @param granular_data 细粒度压缩数据
     * @param chunks 数据块列表
     * @return 记录查询结果
     */
    RecordQueryResult executeRangeQuery(const std::string& field_name,
                                       const std::string& min_value,
                                       const std::string& max_value,
                                       const GranularCompressedData& granular_data,
                                       const std::vector<ChunkedTypeAwareBlock>& chunks);
    
    // ========== 管理方法 ==========
    
    /**
     * 清除缓存
     */
    void clearCache();
    
    /**
     * 获取性能统计
     * @return 性能统计
     */
    std::unordered_map<std::string, double> getPerformanceStats() const;
    
    /**
     * 开始性能分析
     */
    void startProfiling();
    
    /**
     * 停止性能分析
     */
    void stopProfiling();

private:
    // 配置
    QueryConfig config_;
    
    // 组件
    std::unique_ptr<QueryParser> parser_;
    std::unique_ptr<FieldAnalyzer> field_analyzer_;
    std::unique_ptr<ChunkSelector> chunk_selector_;
    std::unique_ptr<SelectiveDecompressor> decompressor_;
    std::unique_ptr<TrieTraverser> trie_traverser_;
    std::unique_ptr<ResultRebuilder> result_rebuilder_;
    
    // 性能统计
    std::unordered_map<std::string, double> performance_stats_;
 
    /**
     * 获取字段索引
     * @param field_name 字段名
     * @param field_order 字段顺序
     * @return 字段索引，-1表示未找到
     */
    int getFieldIndex(const std::string& field_name, const std::vector<FieldKey>& field_order) const;
    
    /**
     * 检查字段类型是否需要字典解压
     * @param field_type 字段类型
     * @return 是否需要字典解压
     */
    bool needsDictionaryDecompression(FieldType field_type) const;
    
    /**
     * 重建记录
     * @param matched_bfs_indices 匹配的BFS索引
     * @param louds LOUDS结构
     * @param dict_manager 字典管理器
     * @return 重建的记录
     */
    std::vector<std::string> reconstructRecords(const std::vector<size_t>& matched_bfs_indices,
                                               const LOUDSTrie& louds,
                                               const FieldDictionaryManager& dict_manager) const;
    
    /**
     * 比较节点值与目标值
     * @param node_value 节点值
     * @param target_value 目标值
     * @param field_key 字段键
     * @param dict_manager 字典管理器
     * @return 是否匹配
     */
    bool compareNodeValueWithTarget(const NodeValue& node_value,
                                   const std::string& target_value,
                                   const FieldKey& field_key,
                                   const FieldDictionaryManager& dict_manager) const;
    
    /**
     * 检查节点值是否在范围内
     * @param node_value 节点值
     * @param min_value 最小值
     * @param max_value 最大值
     * @param field_key 字段键
     * @param dict_manager 字典管理器
     * @return 是否在范围内
     */
    bool isNodeValueInRange(const NodeValue& node_value,
                           const std::string& min_value,
                           const std::string& max_value,
                           const FieldKey& field_key,
                           const FieldDictionaryManager& dict_manager) const;
    
    /**
     * 提取所有字典值
     * @param dict_manager 字典管理器
     * @param field_type 字段类型
     * @return 字典值列表
     */
    std::vector<std::string> extractAllDictionaryValues(const FieldDictionaryManager& dict_manager,
                                                       FieldType field_type) const;
    
    /**
     * 执行字典查找
     * @param dict_manager 字典管理器
     * @param field_type 字段类型
     * @param target_value 目标值
     * @return 查找到的值
     */
    std::string performDictionaryLookup(const FieldDictionaryManager& dict_manager,
                                       FieldType field_type,
                                       const std::string& target_value) const;
    
    /**
     * 在层中查找匹配的节点
     * @param layer_data 层数据
     * @param layer_index 层索引
     * @param target_value 目标值
     * @param field_key 字段键
     * @param dict_manager 字典管理器
     * @return 匹配的层索引列表
     */
    std::vector<size_t> findMatchingNodesInLayer(const std::vector<uint8_t>& layer_data,
                                                size_t layer_index,
                                                const std::string& target_value,
                                                const FieldKey& field_key,
                                                const FieldDictionaryManager& dict_manager) const;
    
    /**
     * 计算解压比例
     * @param granular_data 细粒度压缩数据
     * @param layer_index 层索引
     * @param include_dict 是否包含字典
     * @return 解压比例
     */
    double calculateDecompressionRatio(const GranularCompressedData& granular_data,
                                      size_t layer_index,
                                      bool include_dict) const;
    
    /**
     * 在层中查找范围内的节点
     * @param layer_data 层数据
     * @param layer_index 层索引
     * @param min_value 最小值
     * @param max_value 最大值
     * @param field_key 字段键
     * @param dict_manager 字典管理器
     * @return 匹配的层索引列表
     */
    std::vector<size_t> findNodesInRangeInLayer(const std::vector<uint8_t>& layer_data,
                                               size_t layer_index,
                                               const std::string& min_value,
                                               const std::string& max_value,
                                               const FieldKey& field_key,
                                               const FieldDictionaryManager& dict_manager) const;
    
    /**
     * 将路径转换为JSON
     * @param path 路径
     * @param field_order 字段顺序
     * @return JSON字符串
     */
    std::string convertPathToJSON(const std::vector<std::string>& path,
                                 const std::vector<FieldKey>& field_order) const;
    
    /**
     * 映射字段类型到压缩类型
     * @param json_field_type JSON字段类型
     * @return 压缩字段类型
     */
    compression::FieldType mapFieldTypeToCompressionType(FieldType json_field_type) const;
};

} // namespace query
} // namespace json2