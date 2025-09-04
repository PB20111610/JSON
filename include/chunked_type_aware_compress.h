#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "trie.h"
#include "field_dictionary_manager.h"
#include "compress_type_aware.h"
#include "compression/type_aware/type_aware_compressor.h"

// Forward declare simdjson types
namespace simdjson {
namespace dom {
    class parser;
}
}

namespace json2 {

// 分块类型感知压缩统计信息
struct ChunkedTypeAwareStats {
    size_t original_file_size;           // 原始文件大小
    size_t total_compressed_size;        // 所有块压缩后总大小
    size_t total_blocks;                 // 总块数
    double compression_ratio;            // 整体压缩比
    std::vector<size_t> block_sizes;     // 每块大小
    size_t total_records;                // 总记录数
    size_t records_per_block;            // 每块记录数
    size_t total_core_data_size;         // 所有块的原始数据（core data）大小
    
    // Type-aware specific stats
    std::vector<double> block_compression_ratios; // 每块的压缩比
    compression::TypeAwareCompressionConfig config; // 使用的配置
};

// 分块类型感知压缩器配置
struct ChunkedTypeAwareConfig {
    size_t block_size = 5000;               // 默认块大小
    size_t chunk_size = 1000;               // 字段统计用的chunk大小，默认1000
    bool structurize_arrays = false;        // 是否启用结构化数组处理，默认false启用非结构化数组处理
    
    // Type-aware compression configuration
    compression::TypeAwareCompressionConfig type_aware_config;
};

// 分块类型感知压缩器
class ChunkedTypeAwareCompressor {
public:
    explicit ChunkedTypeAwareCompressor(const ChunkedTypeAwareConfig& config = ChunkedTypeAwareConfig{});
    
    // Configuration methods
    void setBlockSize(size_t size);
    size_t getBlockSize() const;
    void setConfig(const ChunkedTypeAwareConfig& config);
    const ChunkedTypeAwareConfig& getConfig() const;
    void setChunkSize(size_t size);
    size_t getChunkSize() const;
    void setTypeAwareConfig(const compression::TypeAwareCompressionConfig& config);
    const compression::TypeAwareCompressionConfig& getTypeAwareConfig() const;
    
    // Data processing methods
    void addRecord(const std::string& record, simdjson::dom::parser& parser);
    void finalizeCurrentBlock();
    
    // Status methods
    size_t getCurrentBlockRecordCount() const;
    size_t getTotalBlocks() const;
    size_t getTotalRecords() const;
    bool shouldStartNewBlock() const;
    
    // Serialization methods
    std::vector<uint8_t> serialize();
    std::vector<uint8_t> serializeBlock(size_t block_index) const;
    
    // Statistics and management
    ChunkedTypeAwareStats getStats() const;
    void clear();
    void setOriginalFileSize(size_t size);
    
    // Field configuration
    void setTimestampFields(const std::vector<std::string>& fields);
    void setStructurizeArrays(bool structurize);
    
    // Compressed block structure for deserialization
    struct ChunkedTypeAwareBlock {
        std::vector<FieldKey> field_order;
        std::unique_ptr<FieldDictionaryManager> dict;
        std::unique_ptr<Trie> trie;
        compression::TypeAwareCompressionConfig config; // Configuration used for this block
    };
    
    // Deserialization method
    static std::vector<ChunkedTypeAwareBlock> deserialize(const std::vector<uint8_t>& data);

private:
    ChunkedTypeAwareConfig config_;
    size_t current_block_count_;
    size_t total_records_;
    size_t original_file_size_;
    std::vector<std::string> block_buffer_;
    
    // 字段统计用chunk buffer
    std::vector<std::string> chunk_stat_buffer_;
    
    // 只保留分块内存结构 - Type-aware version
    struct ChunkedTypeAwareBlockMemory {
        std::vector<uint8_t> compressed_data;
        size_t original_size = 0; // 每块的原始数据大小
        double compression_ratio = 1.0; // 该块的压缩比
        compression::TypeAwareCompressionConfig block_config; // 该块使用的配置
    };
    std::vector<ChunkedTypeAwareBlockMemory> blocks_memory_;
    
    // 存储时间戳字段
    std::vector<std::string> timestamp_fields_;
};

} // namespace json2