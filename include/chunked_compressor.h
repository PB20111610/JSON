#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "trie.h"
#include "field_dictionary_manager.h"
#include "compress.h"

// Forward declare simdjson types
namespace simdjson {
namespace dom {
    class parser;
}
}

namespace json2 {

// 压缩统计信息
struct CompressionStats {
    size_t original_file_size;      // 原始文件大小
    size_t total_compressed_size;   // 所有块压缩后总大小
    size_t total_blocks;            // 总块数
    double compression_ratio;       // 整体压缩比
    std::vector<size_t> block_sizes; // 每块大小
    size_t total_records;           // 总记录数
    size_t records_per_block;       // 每块记录数
    size_t total_core_data_size;     // 所有块的原始数据（core data）大小
    
    // Trie structure statistics
    std::vector<double> placeholder_ratios;  // 每块的占位节点比例
    std::vector<bool> structure_appropriateness; // 每块的结构适宜性（占位节点比例<5%）
    double avg_placeholder_ratio = 0.0;      // 平均占位节点比例
    size_t appropriate_blocks = 0;           // 结构适宜的块数量
};

// 压缩器配置
struct CompressorConfig {
    size_t block_size = 5000;           // 默认块大小
    size_t chunk_size = 1000;           // 字段统计用的chunk大小，默认1000
    bool structurize_arrays = false;    // 是否启用结构化数组处理，默认false启用非结构化数组处理
    // bool enable_path_compression = false; // 是否启用路径压缩
    
    // Custom field ordering (optional)
    std::vector<FieldKey> custom_field_order;  // 用户自定义字段排序，为空时使用冗余度计算
    bool use_custom_order = false;             // 是否使用自定义字段排序
    
    // ========== 细粒度压缩配置 ==========
    bool enable_granular_compression = false;  // 是否启用细粒度压缩
    bool enable_layer_separation = false;      // 是否启用分层压缩
    bool save_to_filesystem = false;           // 是否保存到文件系统（按README_CHUNKED结构）
};

// 分块压缩器
class ChunkedTrieCompressor {
public:
    struct ChunkedBlock {
        std::vector<FieldKey> field_order;
        std::unique_ptr<FieldDictionaryManager> dict;
        std::unique_ptr<Trie> trie;
    };
    explicit ChunkedTrieCompressor(const CompressorConfig& config = CompressorConfig{});
    void setBlockSize(size_t size);
    size_t getBlockSize() const;
    void setConfig(const CompressorConfig& config);
    const CompressorConfig& getConfig() const;
    void setChunkSize(size_t size);
    size_t getChunkSize() const;
    void addRecord(const std::string& record, simdjson::dom::parser& parser);
    void finalizeCurrentBlock();
    size_t getCurrentBlockRecordCount() const;
    size_t getTotalBlocks() const;
    size_t getTotalRecords() const;
    std::vector<uint8_t> serialize();
    CompressionStats getStats() const;
    void clear();
    void setOriginalFileSize(size_t size);
    bool shouldStartNewBlock() const;
    std::vector<uint8_t> serializeBlock(size_t block_index) const;
    void setTimestampFields(const std::vector<std::string>& fields);    // 设置时间戳字段
    void setStructurizeArrays(bool structurize);                        // 设置数组处理模式
    
    // Custom field ordering configuration
    void setCustomFieldOrder(const std::vector<FieldKey>& custom_order);
    void enableCustomFieldOrder(bool enable = true);
    bool isUsingCustomOrder() const;
    const std::vector<FieldKey>& getCustomFieldOrder() const;

    // ========== 细粒度分块压缩接口 ==========
    // 启用/禁用细粒度压缩
    void enableGranularCompression(bool enable = true);
    bool isGranularCompressionEnabled() const;
    
    // 启用/禁用分层压缩
    void enableLayerSeparation(bool enable = true);
    bool isLayerSeparationEnabled() const;
    
    // 文件系统存储接口
    bool saveToDirectory(const std::string& directory_path);
    static std::vector<ChunkedBlock> loadFromDirectory(const std::string& directory_path);
    
    // 选择性加载接口
    struct SelectiveLoadOptions {
        bool load_trie = true;
        bool load_string_dict = true;
        bool load_timestamp_dict = true;
        bool load_logtype_dict = true;
        std::vector<size_t> specific_chunks; // 指定加载哪些块
        std::vector<size_t> specific_layers; // 指定加载哪些层（如果启用分层）
    };
    
    static std::vector<ChunkedBlock> loadFromDirectorySelective(const std::string& directory_path, 
                                                               const SelectiveLoadOptions& options);

    static std::vector<ChunkedBlock> deserialize(const std::vector<uint8_t>& data);

private:
    CompressorConfig config_;
    size_t current_block_count_;
    size_t total_records_;
    size_t original_file_size_;
    std::vector<std::string> block_buffer_;
    // 字段统计用chunk buffer
    std::vector<std::string> chunk_stat_buffer_;
    // 分块内存结构（支持细粒度压缩）
    struct ChunkedBlockMemory {
        // 传统压缩数据（兼容性）
        std::vector<uint8_t> compressed_data;
        
        // 细粒度压缩数据
        std::vector<uint8_t> trie_bitmap;
        std::vector<uint8_t> string_dict;
        std::vector<uint8_t> timestamp_dict;
        std::vector<uint8_t> logtype_dict;
        std::vector<std::vector<uint8_t>> layer_data_by_level;
        std::vector<uint8_t> layer_data_combined;
        std::vector<uint8_t> layer_sizes;        // 层大小信息
        std::vector<uint8_t> metadata;
        
        // 统计信息
        size_t original_size = 0;
        double placeholder_ratio = 0.0;
        bool is_structure_appropriate = true;
        bool is_granular = false; // 是否使用细粒度压缩
        bool use_layer_separation = false; // 是否使用分层压缩
        
        // 各组件原始大小
        size_t trie_original_size = 0;
        size_t string_dict_original_size = 0;
        size_t timestamp_dict_original_size = 0;
        size_t logtype_dict_original_size = 0;
        size_t layer_original_size = 0;
        size_t layer_sizes_original_size = 0;    // 层大小信息原始大小
        size_t metadata_original_size = 0;
    };
    std::vector<ChunkedBlockMemory> blocks_memory_;
    // 存储时间戳字段
    std::vector<std::string> timestamp_fields_;
    
    // 私有辅助方法
    std::vector<uint8_t> serializeGranularToMemory() const;
};

} // namespace json2