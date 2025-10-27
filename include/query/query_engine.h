#pragma once

#include "query_parser.h"
#include "field_analyzer.h"
#include "chunk_selector.h"
#include "selective_decompressor.h"
#include "trie_traverser.h"
#include "result_rebuilder.h"
#include "../chunked_type_aware_compress.h"
#include "../loudsTotrie.h"  // Add this include for path reconstruction functions
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <set>
#include <map>
#include <thread>
#include <future>
#include <algorithm>

namespace json2 {
namespace query {

// Forward declaration
class QueryNode;

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

// 聚合查询结果
struct AggregateQueryResult {
    double value = 0.0;                     // 聚合值
    size_t count = 0;                       // 记录数
    std::string aggregate_type;             // 聚合类型
    std::string field_name;                 // 字段名
    size_t chunks_accessed = 0;             // 访问的块数
    double query_time_ms = 0.0;             // 查询时间（毫秒）
    bool is_complete = false;               // 是否完整查询
    std::string error_message;              // 错误信息
};

// 分组聚合查询结果
struct GroupedAggregateQueryResult {
    std::map<std::vector<std::string>, std::map<std::string, double>> grouped_values;  // 分组值映射
    std::vector<std::string> group_fields;    // 分组字段
    std::vector<std::string> aggregate_fields; // 聚合字段
    size_t total_count = 0;                   // 总记录数
    size_t groups_count = 0;                  // 分组数
    size_t chunks_accessed = 0;               // 访问的块数
    double query_time_ms = 0.0;               // 查询时间（毫秒）
    bool is_complete = false;                 // 是否完整查询
    std::string error_message;                // 错误信息
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
    // Constructor
    explicit QueryEngine(const QueryConfig& config = QueryConfig{});
    
    // Set the data directory for granular data extraction
    void setDataDirectory(const std::string& data_dir);
    
    // ========== 查询接口 ==========
    
    // 字段存在性和类型检查
    FieldExistenceResult checkFieldExistenceAndType(
        const std::string& field_name,
        FieldType expected_type,
        const GranularCompressedData& granular_data);
    
    // 字典查询（浏览或精确匹配）
    DictionaryQueryResult queryDictionary(
        const std::string& field_name,
        FieldType field_type,
        const GranularCompressedData& granular_data,
        const std::string& target_value = ""); // Empty for browsing all values
    
    // 精确匹配查询 (单块版本)
    RecordQueryResult executeExactMatchQuery(
        const std::string& field_name,
        const std::string& exact_value,
        const GranularCompressedData& granular_data);
    
    // 范围查询 (单块版本)
    RecordQueryResult executeRangeQuery(
        const std::string& field_name,
        const std::string& min_value,
        const std::string& max_value,
        const GranularCompressedData& granular_data);
    
    // 聚合查询 (单块版本)
    AggregateQueryResult executeAggregateQuery(
        AggregateFunction aggregate_func,
        const std::string& field_name,
        const GranularCompressedData& granular_data);
    
    // 分组聚合查询 (单块版本)
    GroupedAggregateQueryResult executeGroupedAggregateQuery(
        const std::vector<AggregateFunction>& aggregate_funcs,
        const std::vector<std::string>& aggregate_fields,
        const std::vector<std::string>& group_fields,
        const GranularCompressedData& granular_data);
    
    // 复杂查询（支持逻辑操作符）(单块版本)
    QueryResult executeComplexQuery(
        const std::string& complex_query,
        const GranularCompressedData& granular_data);
        
    // 多块遍历接口
    RecordQueryResult executeExactMatchQueryMultiBlock(
        const std::string& field_name,
        const std::string& exact_value,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    RecordQueryResult executeRangeQueryMultiBlock(
        const std::string& field_name,
        const std::string& min_value,
        const std::string& max_value,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    AggregateQueryResult executeAggregateQueryMultiBlock(
        AggregateFunction aggregate_func,
        const std::string& field_name,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    GroupedAggregateQueryResult executeGroupedAggregateQueryMultiBlock(
        const std::vector<AggregateFunction>& aggregate_funcs,
        const std::vector<std::string>& aggregate_fields,
        const std::vector<std::string>& group_fields,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    QueryResult executeComplexQueryMultiBlock(
        const std::string& complex_query,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    // 多线程多块遍历接口
    RecordQueryResult executeExactMatchQueryMultiBlockParallel(
        const std::string& field_name,
        const std::string& exact_value,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = std::thread::hardware_concurrency());
        
    RecordQueryResult executeRangeQueryMultiBlockParallel(
        const std::string& field_name,
        const std::string& min_value,
        const std::string& max_value,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = std::thread::hardware_concurrency());
        
    AggregateQueryResult executeAggregateQueryMultiBlockParallel(
        AggregateFunction aggregate_func,
        const std::string& field_name,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = std::thread::hardware_concurrency());
        
    GroupedAggregateQueryResult executeGroupedAggregateQueryMultiBlockParallel(
        const std::vector<AggregateFunction>& aggregate_funcs,
        const std::vector<std::string>& aggregate_fields,
        const std::vector<std::string>& group_fields,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = std::thread::hardware_concurrency());
        
    QueryResult executeComplexQueryMultiBlockParallel(
        const std::string& complex_query,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = std::thread::hardware_concurrency());

private:
    QueryConfig config_;
    std::unique_ptr<QueryParser> parser_;
    std::unique_ptr<FieldAnalyzer> field_analyzer_;
    std::unique_ptr<ChunkSelector> chunk_selector_;
    std::unique_ptr<SelectiveDecompressor> decompressor_;
    std::unique_ptr<TrieTraverser> trie_traverser_;
    std::unique_ptr<ResultRebuilder> result_rebuilder_;
    mutable std::unordered_map<std::string, double> performance_stats_;
    std::string data_dir_;  // Data directory for granular data extraction
    
    // ========== 内部实现方法 ==========
    
    // Helper function to map json2::FieldType to compression::FieldType
    compression::FieldType mapFieldTypeToCompressionType(FieldType json_field_type) const;
    
    // 获取字段在字段顺序中的索引
    int getFieldIndex(const std::string& field_name, const std::vector<FieldKey>& field_order) const;
    
    // 判断字段类型是否需要字典解压
    bool needsDictionaryDecompression(FieldType field_type) const;
    
    // 提取字典中的所有值
    std::vector<std::string> extractAllDictionaryValues(
        const FieldDictionaryManager& dict_manager,
        FieldType field_type) const;
    
    // 执行字典查找
    std::string performDictionaryLookup(
        const FieldDictionaryManager& dict_manager,
        FieldType field_type,
        const std::string& target_value) const;
    
    // 解码节点值为字符串
    std::string decodeNodeValueToString(
        const NodeValue& node_value,
        const FieldKey& field_key,
        const FieldDictionaryManager& dict_manager) const;
        
    // 解码节点值为数值
    double decodeNodeValueToDouble(
        const NodeValue& node_value,
        const FieldKey& field_key,
        const FieldDictionaryManager& dict_manager) const;
        
    // ========== 复杂查询辅助方法 ==========
    
    // 评估查询节点
    QueryResult evaluateQueryNode(
        const QueryNode* node,
        const GranularCompressedData& granular_data);
    
    // 评估字段节点
    QueryResult evaluateFieldNode(
        const QueryNode* node,
        const GranularCompressedData& granular_data);
    
    // 评估逻辑节点
    QueryResult evaluateLogicalNode(
        const QueryNode* node,
        const GranularCompressedData& granular_data);
        
    // 评估聚合节点
    QueryResult evaluateAggregateNode(
        const QueryNode* node,
        const GranularCompressedData& granular_data);
        
    // 评估分组节点
    QueryResult evaluateGroupByNode(
        const QueryNode* node,
        const GranularCompressedData& granular_data);
        
    // 合并两个查询结果（用于AND操作）
    QueryResult mergeResultsAND(const QueryResult& left, const QueryResult& right) const;
    
    // 合并两个查询结果（用于OR操作）
    QueryResult mergeResultsOR(const QueryResult& left, const QueryResult& right) const;
    
    // 对查询结果进行NOT操作
    QueryResult negateResult(const QueryResult& result) const;
    
    // Helper function to check if a value exists in a dictionary without full decompression
    bool checkValueInDictionary(const std::string& field_name,
                               FieldType field_type,
                               const std::string& target_value,
                               const GranularCompressedData& granular_data) const;
 
    // Helper functions to merge RecordQueryResult objects from multiple blocks
    void mergeRecordQueryResults(RecordQueryResult& target, const RecordQueryResult& source) const;
    
    // Helper functions to merge AggregateQueryResult objects from multiple blocks
    void mergeAggregateQueryResults(AggregateQueryResult& target, const AggregateQueryResult& source) const;
    
    // Helper functions to merge GroupedAggregateQueryResult objects from multiple blocks
    void mergeGroupedAggregateQueryResults(GroupedAggregateQueryResult& target, const GroupedAggregateQueryResult& source) const;
    
    // Helper function to merge QueryResult objects efficiently
    void mergeQueryResults(QueryResult& target, const QueryResult& source);
    
    // Helper function to extract granular data from a chunk
    GranularCompressedData extractGranularDataFromChunk(const ChunkedTypeAwareBlock& chunk, size_t chunk_index) const;
};

} // namespace query
} // namespace json2