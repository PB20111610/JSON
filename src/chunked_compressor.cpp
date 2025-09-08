#include "../include/chunked_compressor.h"
#include <simdjson.h>
#include "../include/compress.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
#include <functional>
#include "../include/field_analyzer.h"

namespace json2 {

// Helper function to count placeholder nodes in Trie
static size_t countPlaceholderNodes(const TrieNode* node) {
    if (!node) return 0;
    
    size_t count = 0;
    if (node->isPlaceholder()) {
        count = 1;
    }
    
    // Recursively count placeholder nodes in children
    for (const auto& child : node->getChildren()) {
        count += countPlaceholderNodes(child.second.get());
    }
    
    return count;
}

// Helper function to count total nodes in Trie
static size_t countTotalNodes(const TrieNode* node) {
    if (!node) return 0;
    
    size_t count = 1; // Count current node
    
    // Recursively count nodes in children
    for (const auto& child : node->getChildren()) {
        count += countTotalNodes(child.second.get());
    }
    
    return count;
}

// Helper function to calculate placeholder ratio and check if Trie structure is appropriate
static std::pair<double, bool> analyzePlaceholderRatio(const Trie& trie) {
    const TrieNode* root = trie.getRoot();
    if (!root) return {0.0, true};
    
    size_t total_nodes = countTotalNodes(root);
    size_t placeholder_nodes = countPlaceholderNodes(root);
    
    double ratio = (total_nodes > 0) ? static_cast<double>(placeholder_nodes) / total_nodes : 0.0;
    bool is_appropriate = ratio < 0.05; // Less than 5% indicates appropriate structure
    
    return {ratio, is_appropriate};
}

ChunkedTrieCompressor::ChunkedTrieCompressor(const CompressorConfig& config)
    : config_(config), current_block_count_(0), total_records_(0), original_file_size_(0) {
    std::cerr << "[DEBUG] ChunkedTrieCompressor constructed." << std::endl;
    chunk_stat_buffer_.clear();
}

void ChunkedTrieCompressor::setChunkSize(size_t size) {
    config_.chunk_size = size;
}

size_t ChunkedTrieCompressor::getChunkSize() const {
    return config_.chunk_size;
}

void ChunkedTrieCompressor::setBlockSize(size_t size) {
    config_.block_size = size;
    std::cerr << "[DEBUG] Block size set to " << size << std::endl;
}

size_t ChunkedTrieCompressor::getBlockSize() const {
    return config_.block_size;
}

void ChunkedTrieCompressor::setConfig(const CompressorConfig& config) {
    config_ = config;
    std::cerr << "[DEBUG] Config updated." << std::endl;
}

const CompressorConfig& ChunkedTrieCompressor::getConfig() const {
    return config_;
}

void ChunkedTrieCompressor::addRecord(const std::string& record, simdjson::dom::parser& parser) {
    block_buffer_.push_back(record);
    if (chunk_stat_buffer_.size() < config_.chunk_size) {
        chunk_stat_buffer_.push_back(record);
    }
    current_block_count_++;
    total_records_++;
    if (shouldStartNewBlock()) {
        std::cerr << "[DEBUG] Block full. Flushing current block (" << current_block_count_ << " records)." << std::endl;
        finalizeCurrentBlock();
        return;
    }
    if (total_records_ % config_.block_size == 0) {
        std::cerr << "[DEBUG] Added record " << total_records_ << ", current block should flush soon." << std::endl;
    }
}



void ChunkedTrieCompressor::setTimestampFields(const std::vector<std::string>& fields) {
    timestamp_fields_ = fields;
}

void ChunkedTrieCompressor::setStructurizeArrays(bool structurize) {
    config_.structurize_arrays = structurize;
    std::cerr << "[DEBUG] Set structurize arrays: " << (structurize ? "true" : "false") << std::endl;
}

// ========== Custom Field Ordering Methods ==========

void ChunkedTrieCompressor::setCustomFieldOrder(const std::vector<FieldKey>& custom_order) {
    config_.custom_field_order = custom_order;
    if (!custom_order.empty()) {
        config_.use_custom_order = true;
        std::cout << "[CONFIG] Custom field order set with " << custom_order.size() << " fields" << std::endl;
    } else {
        config_.use_custom_order = false;
        std::cout << "[CONFIG] Custom field order cleared, will use redundancy calculation" << std::endl;
    }
}

void ChunkedTrieCompressor::enableCustomFieldOrder(bool enable) {
    config_.use_custom_order = enable;
    if (enable && config_.custom_field_order.empty()) {
        std::cerr << "[WARNING] Custom field order enabled but no custom order provided" << std::endl;
    }
}

bool ChunkedTrieCompressor::isUsingCustomOrder() const {
    return config_.use_custom_order && !config_.custom_field_order.empty();
}

const std::vector<FieldKey>& ChunkedTrieCompressor::getCustomFieldOrder() const {
    return config_.custom_field_order;
}

void ChunkedTrieCompressor::finalizeCurrentBlock() {
    if (current_block_count_ > 0 && !block_buffer_.empty()) {
        FieldDictionaryManager dict;
        dict.setTimestampFields(timestamp_fields_);
        dict.setStructurizeArrays(config_.structurize_arrays);  // 设置数组处理模式
        std::vector<FieldKey> ordered_fields;
        
        // Choose field analysis method based on configuration
        if (isUsingCustomOrder()) {
            // Use custom field order provided by user
            FieldAnalyzer::analyzeWithCustomOrder(chunk_stat_buffer_, dict, config_.custom_field_order, ordered_fields);
        } else {
            // Use traditional redundancy-based field analysis  
            FieldAnalyzer::analyzeAndSortFields(chunk_stat_buffer_, dict, ordered_fields);
        }
        std::cerr << "[DEBUG] Finalizing block. Record count: " << current_block_count_ << ", Field count: " << ordered_fields.size() << std::endl;
        Trie trie(ordered_fields);
        simdjson::dom::parser parser;
        for (const auto& rec : block_buffer_) {
            trie.insert(rec, dict, parser);
        }
        LOUDSTrie louds(ordered_fields);
        louds.buildFromTrie(trie);
        
        // Analyze placeholder ratio
        auto [placeholder_ratio, is_appropriate] = analyzePlaceholderRatio(trie);
        std::cerr << "[TRIE_STATS] Block " << blocks_memory_.size() 
                  << ": Placeholder ratio = " << std::fixed << std::setprecision(4) 
                  << (placeholder_ratio * 100) << "%, Structure " 
                  << (is_appropriate ? "appropriate" : "needs optimization") << std::endl;
        
        auto compressed = Compressor::compressLouds(louds, dict, ordered_fields);
        // 直接序列化为内存块
        ChunkedBlockMemory block_mem;
        block_mem.compressed_data = Compressor::saveToMemory(compressed);
        block_mem.original_size = compressed.original_size;
        block_mem.placeholder_ratio = placeholder_ratio;
        block_mem.is_structure_appropriate = is_appropriate;
        blocks_memory_.push_back(std::move(block_mem));
        block_buffer_.clear();
        current_block_count_ = 0;
        chunk_stat_buffer_.clear();
    }
}

size_t ChunkedTrieCompressor::getCurrentBlockRecordCount() const {
    return current_block_count_;
}

size_t ChunkedTrieCompressor::getTotalBlocks() const {
    return blocks_memory_.size();
}

size_t ChunkedTrieCompressor::getTotalRecords() const {
    return total_records_;
}

void ChunkedTrieCompressor::clear() {
    blocks_memory_.clear();
    current_block_count_ = 0;
    total_records_ = 0;
    original_file_size_ = 0;
    std::cerr << "[DEBUG] Compressor state cleared." << std::endl;
}

void ChunkedTrieCompressor::setOriginalFileSize(size_t size) {
    original_file_size_ = size;
    std::cerr << "[DEBUG] Set original file size: " << size << std::endl;
}

bool ChunkedTrieCompressor::shouldStartNewBlock() const {
    return current_block_count_ >= config_.block_size;
}

std::vector<uint8_t> ChunkedTrieCompressor::serialize() {
    std::vector<uint8_t> result;
    finalizeCurrentBlock();
    // 1. 序列化块数
    uint32_t block_count = static_cast<uint32_t>(blocks_memory_.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_count), reinterpret_cast<uint8_t*>(&block_count) + sizeof(block_count));
    // 2. 依次写入每个block的长度和内容
    for (const auto& block : blocks_memory_) {
        uint32_t block_size = static_cast<uint32_t>(block.compressed_data.size());
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_size), reinterpret_cast<uint8_t*>(&block_size) + sizeof(block_size));
        result.insert(result.end(), block.compressed_data.begin(), block.compressed_data.end());
    }
    std::cerr << "[SER] Total blocks: " << blocks_memory_.size() << ", Total records: " << total_records_ << std::endl;
    return result;
}

CompressionStats ChunkedTrieCompressor::getStats() const {
    CompressionStats stats;
    stats.original_file_size = original_file_size_;
    stats.total_blocks = blocks_memory_.size();
    stats.total_records = total_records_;
    stats.records_per_block = config_.block_size;
    size_t total_compressed = 0;
    size_t total_core_data = 0;
    double total_placeholder_ratio = 0.0;
    size_t appropriate_count = 0;
    
    for (const auto& block : blocks_memory_) {
        total_compressed += block.compressed_data.size();
        total_core_data += block.original_size;
        stats.block_sizes.push_back(block.compressed_data.size());
        stats.placeholder_ratios.push_back(block.placeholder_ratio);
        stats.structure_appropriateness.push_back(block.is_structure_appropriate);
        total_placeholder_ratio += block.placeholder_ratio;
        if (block.is_structure_appropriate) {
            appropriate_count++;
        }
    }
    
    stats.total_compressed_size = total_compressed;
    stats.total_core_data_size = total_core_data;
    stats.avg_placeholder_ratio = blocks_memory_.empty() ? 0.0 : total_placeholder_ratio / blocks_memory_.size();
    stats.appropriate_blocks = appropriate_count;
    
    if (total_compressed > 0) {
        stats.compression_ratio = static_cast<double>(stats.original_file_size) / total_compressed;
    } else {
        stats.compression_ratio = 0.0;
    }
    return stats;
}

std::vector<uint8_t> ChunkedTrieCompressor::serializeBlock(size_t block_index) const {
    if (block_index >= blocks_memory_.size()) {
        return {};
    }
    std::vector<uint8_t> result;
    const auto& block = blocks_memory_[block_index];
    uint32_t block_size = static_cast<uint32_t>(block.compressed_data.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_size), reinterpret_cast<uint8_t*>(&block_size) + sizeof(block_size));
    result.insert(result.end(), block.compressed_data.begin(), block.compressed_data.end());
    return result;
}

std::vector<ChunkedTrieCompressor::ChunkedBlock> ChunkedTrieCompressor::deserialize(const std::vector<uint8_t>& data) {
    std::vector<ChunkedBlock> blocks;
    size_t offset = 0;
    try {
        uint32_t block_count = 0;
        std::memcpy(&block_count, &data[offset], sizeof(block_count));
        offset += sizeof(block_count);
        for (uint32_t i = 0; i < block_count; ++i) {
            uint32_t block_size = 0;
            std::memcpy(&block_size, &data[offset], sizeof(block_size));
            offset += sizeof(block_size);
            if (data.size() < offset + block_size) throw std::runtime_error("Block data out of range");
            std::vector<uint8_t> block_data(data.begin() + offset, data.begin() + offset + block_size);
            offset += block_size;
            // 反序列化为CompressedData
            CompressedData cd = Compressor::loadFromMemory(block_data);
            // 解压
            auto [louds, dict_ptr] = Compressor::decompressLouds(cd);
            auto trie = std::make_unique<Trie>(louds->getFieldOrder());
            loudsToTrie(*louds, *trie);
            blocks.push_back(ChunkedBlock{louds->getFieldOrder(), std::move(dict_ptr), std::move(trie)});
        }
        std::cerr << "[DESER] Finished deserializing " << block_count << " blocks." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception during deserialization: " << e.what() << " (offset=" << offset << ")" << std::endl;
        throw;
    }
    return blocks;
}

} // namespace json2