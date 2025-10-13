#pragma once

#include "field_analyzer.h"
#include "chunked_compressor.h"
#include "chunked_type_aware_compress.h"
#include <vector>
#include <unordered_set>
#include <memory>

namespace json2 {
namespace query {

/**
 * 块元数据
 */
struct ChunkMetadata {
    size_t chunk_id;                                    // 块ID
    std::vector<FieldKey> schema;                       // 块中的字段模式
    std::unordered_set<std::string> fields;             // 字段存在性
    size_t record_count;                                // 记录数量
    size_t compressed_size;                             // 压缩后大小
    double compression_ratio;                           // 压缩比
    std::unordered_map<std::string, FieldType> field_types; // 字段类型映射
    bool has_timestamp;                                 // 是否包含时间戳字段
    bool has_template;                                  // 是否包含模板字段
    uint64_t min_timestamp;                             // 最小时间戳（如果有）
    uint64_t max_timestamp;                             // 最大时间戳（如果有）
};

/**
 * 块选择结果
 */
struct ChunkSelection {
    std::vector<size_t> selected_chunks;                // 选中的块ID列表
    std::vector<ChunkMetadata> chunk_metadata;          // 块元数据
    size_t total_chunks;                                // 总块数
    double selection_ratio;                             // 选择比例
    std::unordered_set<std::string> required_fields;    // 查询需要的字段
    bool needs_full_scan;                               // 是否需要全扫描
};

/**
 * 块选择器
 * 基于字段存在性和查询特征选择相关数据块
 */
class ChunkSelector {
public:
    ChunkSelector() = default;
    
    /**
     * 选择相关块
     * @param analysis 字段分析结果
     * @param chunks 可用块列表
     * @return 块选择结果
     */
    ChunkSelection selectChunks(const FieldAnalysis& analysis, 
                               const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks);
    
    /**
     * 选择相关块（类型感知版本）
     * @param analysis 字段分析结果
     * @param chunks 可用块列表
     * @return 块选择结果
     */
    ChunkSelection selectChunks(const FieldAnalysis& analysis,
                               const std::vector<ChunkedTypeAwareBlock>& chunks);
    
    /**
     * 基于字段存在性选择块
     * @param required_fields 需要的字段
     * @param chunks 可用块列表
     * @return 选中的块ID列表
     */
    std::vector<size_t> selectByFieldPresence(const std::unordered_set<std::string>& required_fields,
                                             const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks);
    
    /**
     * 基于字段存在性选择块（类型感知版本）
     * @param required_fields 需要的字段
     * @param chunks 可用块列表
     * @return 选中的块ID列表
     */
    std::vector<size_t> selectByFieldPresence(const std::unordered_set<std::string>& required_fields,
                                             const std::vector<ChunkedTypeAwareBlock>& chunks);
    
    /**
     * 基于时间戳范围选择块
     * @param min_timestamp 最小时间戳
     * @param max_timestamp 最大时间戳
     * @param chunks 可用块列表
     * @return 选中的块ID列表
     */
    std::vector<size_t> selectByTimestampRange(uint64_t min_timestamp, uint64_t max_timestamp,
                                              const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks);
    
    /**
     * 基于时间戳范围选择块（类型感知版本）
     * @param min_timestamp 最小时间戳
     * @param max_timestamp 最大时间戳
     * @param chunks 可用块列表
     * @return 选中的块ID列表
     */
    std::vector<size_t> selectByTimestampRange(uint64_t min_timestamp, uint64_t max_timestamp,
                                              const std::vector<ChunkedTypeAwareBlock>& chunks);
    
    /**
     * 基于字段类型选择块
     * @param field_types 字段类型映射
     * @param chunks 可用块列表
     * @return 选中的块ID列表
     */
    std::vector<size_t> selectByFieldTypes(const std::unordered_map<std::string, FieldType>& field_types,
                                          const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks);
    
    /**
     * 基于字段类型选择块（类型感知版本）
     * @param field_types 字段类型映射
     * @param chunks 可用块列表
     * @return 选中的块ID列表
     */
    std::vector<size_t> selectByFieldTypes(const std::unordered_map<std::string, FieldType>& field_types,
                                          const std::vector<ChunkedTypeAwareBlock>& chunks);

private:
    // 提取块元数据
    ChunkMetadata extractMetadata(size_t chunk_id, const ChunkedTrieCompressor::ChunkedBlock& chunk);
    ChunkMetadata extractMetadata(size_t chunk_id, const ChunkedTypeAwareBlock& chunk);
    
    // 字段匹配检查
    bool hasRequiredFields(const ChunkMetadata& metadata, 
                          const std::unordered_set<std::string>& required_fields) const;
    bool hasFieldType(const ChunkMetadata& metadata, 
                     const std::string& field_name, FieldType field_type) const;
    
    // 时间戳范围检查
    bool isInTimestampRange(const ChunkMetadata& metadata, 
                           uint64_t min_timestamp, uint64_t max_timestamp) const;
    
    // 选择策略
    std::vector<size_t> applySelectionStrategy(const FieldAnalysis& analysis,
                                              const std::vector<ChunkMetadata>& metadata_list);
    
    // 优化选择
    void optimizeSelection(ChunkSelection& selection, const FieldAnalysis& analysis);
};

} // namespace query
} // namespace json2
