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
};

// 压缩器配置
struct CompressorConfig {
    size_t block_size = 5000;           // 默认块大小
    size_t chunk_size = 1000;           // 字段统计用的chunk大小，默认1000
    // bool enable_path_compression = false; // 是否启用路径压缩
};

// 分块压缩器
class ChunkedTrieCompressor {
public:
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

    struct ChunkedBlock {
        std::vector<FieldKey> field_order;
        std::unique_ptr<FieldDictionaryManager> dict;
        std::unique_ptr<Trie> trie;
    };
    static std::vector<ChunkedBlock> deserialize(const std::vector<uint8_t>& data);

private:
    CompressorConfig config_;
    size_t current_block_count_;
    size_t total_records_;
    size_t original_file_size_;
    std::vector<std::string> block_buffer_;
    // 字段统计用chunk buffer
    std::vector<std::string> chunk_stat_buffer_;
    // 只保留分块内存结构
    struct ChunkedBlockMemory {
        std::vector<uint8_t> compressed_data;
        size_t original_size = 0; // 每块的原始数据大小
    };
    std::vector<ChunkedBlockMemory> blocks_memory_;
};

} // namespace json2