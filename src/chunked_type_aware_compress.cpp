#include "../include/chunked_type_aware_compress.h"
#include <simdjson.h>
#include "../include/compress_type_aware.h"
#include "../include/loudsTotrie.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
#include <functional>
#include "../include/field_analyzer.h"
#include "../include/field_parser.h"

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

ChunkedTypeAwareCompressor::ChunkedTypeAwareCompressor(const ChunkedTypeAwareConfig& config)
    : config_(config), current_block_count_(0), total_records_(0), original_file_size_(0) {
    chunk_stat_buffer_.clear();
}

// ========== Configuration Methods ==========

void ChunkedTypeAwareCompressor::setBlockSize(size_t size) {
    config_.block_size = size;
}

size_t ChunkedTypeAwareCompressor::getBlockSize() const {
    return config_.block_size;
}

void ChunkedTypeAwareCompressor::setConfig(const ChunkedTypeAwareConfig& config) {
    config_ = config;
}

const ChunkedTypeAwareConfig& ChunkedTypeAwareCompressor::getConfig() const {
    return config_;
}

void ChunkedTypeAwareCompressor::setChunkSize(size_t size) {
    config_.chunk_size = size;
}

size_t ChunkedTypeAwareCompressor::getChunkSize() const {
    return config_.chunk_size;
}

void ChunkedTypeAwareCompressor::setTypeAwareConfig(const compression::TypeAwareCompressionConfig& config) {
    config_.type_aware_config = config;
}

const compression::TypeAwareCompressionConfig& ChunkedTypeAwareCompressor::getTypeAwareConfig() const {
    return config_.type_aware_config;
}

void ChunkedTypeAwareCompressor::setTimestampFields(const std::vector<std::string>& fields) {
    timestamp_fields_ = fields;
}

void ChunkedTypeAwareCompressor::setStructurizeArrays(bool structurize) {
    config_.structurize_arrays = structurize;
}

// ========== Custom Field Ordering Methods ==========

void ChunkedTypeAwareCompressor::setCustomFieldOrder(const std::vector<FieldKey>& custom_order) {
    config_.custom_field_order = custom_order;
    if (!custom_order.empty()) {
        config_.use_custom_order = true;
        std::cout << "[CONFIG] Custom field order set with " << custom_order.size() << " fields" << std::endl;
    } else {
        config_.use_custom_order = false;
        std::cout << "[CONFIG] Custom field order cleared, will use redundancy calculation" << std::endl;
    }
}

void ChunkedTypeAwareCompressor::enableCustomFieldOrder(bool enable) {
    config_.use_custom_order = enable;
    if (enable && config_.custom_field_order.empty()) {
        std::cerr << "[WARNING] Custom field order enabled but no custom order provided" << std::endl;
    }
}

bool ChunkedTypeAwareCompressor::isUsingCustomOrder() const {
    return config_.use_custom_order && !config_.custom_field_order.empty();
}

const std::vector<FieldKey>& ChunkedTypeAwareCompressor::getCustomFieldOrder() const {
    return config_.custom_field_order;
}

// ========== Data Processing Methods ==========

void ChunkedTypeAwareCompressor::addRecord(const std::string& record, simdjson::dom::parser& parser) {
    block_buffer_.push_back(record);
    if (chunk_stat_buffer_.size() < config_.chunk_size) {
        chunk_stat_buffer_.push_back(record);
    }
    current_block_count_++;
    total_records_++;
    
    if (shouldStartNewBlock()) {
        finalizeCurrentBlock();
        return;
    }
}

void ChunkedTypeAwareCompressor::finalizeCurrentBlock() {
    if (current_block_count_ > 0 && !block_buffer_.empty()) {
        try {
            // Setup dictionary manager with configuration
            FieldDictionaryManager dict;
            dict.setTimestampFields(timestamp_fields_);
            dict.setStructurizeArrays(config_.structurize_arrays);
            
            std::vector<FieldKey> ordered_fields;
            
            // Choose field analysis method based on configuration
            if (isUsingCustomOrder()) {
                // Use custom field order provided by user
                FieldAnalyzer::analyzeWithCustomOrder(config_.custom_field_order, ordered_fields);
            } else {
                // Use traditional redundancy-based field analysis
                FieldAnalyzer::analyzeAndSortFields(chunk_stat_buffer_, dict, ordered_fields);
            }
            
            // Build trie structure
            Trie trie(ordered_fields);
            {
                const auto& of = trie.getOrderedFields();
            }
            simdjson::dom::parser parser;
            for (const auto& rec : block_buffer_) {
                trie.insert(rec, dict, parser);
            }
            {
                const auto& of = trie.getOrderedFields();
            }
            
            // Build LOUDS structure
            LOUDSTrie louds(ordered_fields);
            louds.buildFromTrie(trie);
            
            // Use dynamically expanded field order from LOUDS (already contains all fields from trie)
            auto expanded_field_order = louds.getFieldOrder();
            std::cout << "[DEBUG] finalizeCurrentBlock - field order sizes: initial="
                      << ordered_fields.size() << ", expanded=" << expanded_field_order.size() << std::endl;
            
            // Analyze placeholder ratio
            auto [placeholder_ratio, is_appropriate] = analyzePlaceholderRatio(trie);
            std::cerr << "[TYPE_AWARE_TRIE_STATS] Block " << blocks_memory_.size() 
                      << ": Placeholder ratio = " << std::fixed << std::setprecision(4) 
                      << (placeholder_ratio * 100) << "%, Structure " 
                      << (is_appropriate ? "appropriate" : "needs optimization") << std::endl;
            
            // Use TypeAware compression with expanded field order
            auto compressed = TypeAwareCompressor::compressLouds(louds, dict, louds.getFieldOrder(), config_.type_aware_config);
            
            // Save to memory using the existing Compressor for serialization (delegate file I/O operations)
            ChunkedTypeAwareBlockMemory block_mem;
            block_mem.compressed_data = Compressor::saveToMemory(compressed);
            block_mem.original_size = compressed.original_size;
            block_mem.compression_ratio = TypeAwareCompressor::getCompressionRatio(compressed);
            block_mem.block_config = config_.type_aware_config; // Store the configuration used
            block_mem.placeholder_ratio = placeholder_ratio;
            block_mem.is_structure_appropriate = is_appropriate;
            
            blocks_memory_.push_back(std::move(block_mem));
            
            // Clear buffers for next block
            block_buffer_.clear();
            current_block_count_ = 0;
            chunk_stat_buffer_.clear();
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Failed to finalize type-aware block: " << e.what() << std::endl;
            throw;
        }
    }
}

// ========== Status Methods ==========

size_t ChunkedTypeAwareCompressor::getCurrentBlockRecordCount() const {
    return current_block_count_;
}

size_t ChunkedTypeAwareCompressor::getTotalBlocks() const {
    return blocks_memory_.size();
}

size_t ChunkedTypeAwareCompressor::getTotalRecords() const {
    return total_records_;
}

bool ChunkedTypeAwareCompressor::shouldStartNewBlock() const {
    return current_block_count_ >= config_.block_size;
}

// ========== Serialization Methods ==========

std::vector<uint8_t> ChunkedTypeAwareCompressor::serialize() {
    std::vector<uint8_t> result;
    
    // Finalize any remaining block
    finalizeCurrentBlock();
    
    try {
        // 1. Serialize block count
        uint32_t block_count = static_cast<uint32_t>(blocks_memory_.size());
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_count), 
                      reinterpret_cast<uint8_t*>(&block_count) + sizeof(block_count));
        
        // 2. Serialize each block with its configuration
        for (const auto& block : blocks_memory_) {
            // Write block size
            uint32_t block_size = static_cast<uint32_t>(block.compressed_data.size());
            result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_size), 
                          reinterpret_cast<uint8_t*>(&block_size) + sizeof(block_size));
            
            // Write compression level (for config serialization)
            uint32_t compression_level = static_cast<uint32_t>(block.block_config.compression_level);
            result.insert(result.end(), reinterpret_cast<uint8_t*>(&compression_level), 
                          reinterpret_cast<uint8_t*>(&compression_level) + sizeof(compression_level));
            
            // Write block content
            result.insert(result.end(), block.compressed_data.begin(), block.compressed_data.end());
        }
                  
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Serialization failed: " << e.what() << std::endl;
        throw;
    }
    
    return result;
}

std::vector<uint8_t> ChunkedTypeAwareCompressor::serializeBlock(size_t block_index) const {
    if (block_index >= blocks_memory_.size()) {
        return {};
    }
    
    std::vector<uint8_t> result;
    const auto& block = blocks_memory_[block_index];
    
    // Write block size
    uint32_t block_size = static_cast<uint32_t>(block.compressed_data.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_size), 
                  reinterpret_cast<uint8_t*>(&block_size) + sizeof(block_size));
    
    // Write compression level
    uint32_t compression_level = static_cast<uint32_t>(block.block_config.compression_level);
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&compression_level), 
                  reinterpret_cast<uint8_t*>(&compression_level) + sizeof(compression_level));
    
    // Write block content
    result.insert(result.end(), block.compressed_data.begin(), block.compressed_data.end());
    
    return result;
}

// ========== Statistics and Management ==========

ChunkedTypeAwareStats ChunkedTypeAwareCompressor::getStats() const {
    ChunkedTypeAwareStats stats;
    stats.original_file_size = original_file_size_;
    stats.total_blocks = blocks_memory_.size();
    stats.total_records = total_records_;
    stats.records_per_block = config_.block_size;
    stats.config = config_.type_aware_config;
    
    size_t total_compressed = 0;
    size_t total_core_data = 0;
    double total_placeholder_ratio = 0.0;
    size_t appropriate_count = 0;
    
    for (const auto& block : blocks_memory_) {
        total_compressed += block.compressed_data.size();
        total_core_data += block.original_size;
        stats.block_sizes.push_back(block.compressed_data.size());
        stats.block_compression_ratios.push_back(block.compression_ratio);
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

void ChunkedTypeAwareCompressor::clear() {
    blocks_memory_.clear();
    current_block_count_ = 0;
    total_records_ = 0;
    original_file_size_ = 0;
    block_buffer_.clear();
    chunk_stat_buffer_.clear();
}

void ChunkedTypeAwareCompressor::setOriginalFileSize(size_t size) {
    original_file_size_ = size;
}

// ========== Deserialization Methods ==========

std::vector<ChunkedTypeAwareCompressor::ChunkedTypeAwareBlock> 
ChunkedTypeAwareCompressor::deserialize(const std::vector<uint8_t>& data) {
    std::vector<ChunkedTypeAwareBlock> blocks;
    size_t offset = 0;
    
    try {
        // Read block count
        if (data.size() < sizeof(uint32_t)) {
            throw std::runtime_error("Insufficient data for block count");
        }
        
        uint32_t block_count = 0;
        std::memcpy(&block_count, &data[offset], sizeof(block_count));
        offset += sizeof(block_count);
        
        for (uint32_t i = 0; i < block_count; ++i) {
            // Read block size
            if (data.size() < offset + sizeof(uint32_t)) {
                throw std::runtime_error("Insufficient data for block " + std::to_string(i) + " size");
            }
            
            uint32_t block_size = 0;
            std::memcpy(&block_size, &data[offset], sizeof(block_size));
            offset += sizeof(block_size);
            
            // Read compression level
            if (data.size() < offset + sizeof(uint32_t)) {
                throw std::runtime_error("Insufficient data for block " + std::to_string(i) + " compression level");
            }
            
            uint32_t compression_level = 0;
            std::memcpy(&compression_level, &data[offset], sizeof(compression_level));
            offset += sizeof(compression_level);
            
            // Validate block data availability
            if (data.size() < offset + block_size) {
                throw std::runtime_error("Block " + std::to_string(i) + " data out of range");
            }
            
            // Extract block data
            std::vector<uint8_t> block_data(data.begin() + offset, data.begin() + offset + block_size);
            offset += block_size;
            
            // Reconstruct configuration
            compression::TypeAwareCompressionConfig config;
            config.compression_level = static_cast<int>(compression_level);
            
            // Deserialize as CompressedData
            CompressedData cd = Compressor::loadFromMemory(block_data);
            
            // Use TypeAware decompression
            auto [louds, dict_ptr] = TypeAwareCompressor::decompressLouds(cd, config);
            
            // Convert to Trie
            auto trie = std::make_unique<Trie>(louds->getFieldOrder());
            loudsToTrie(*louds, *trie);
            
            // Create block structure
            ChunkedTypeAwareBlock block;
            block.field_order = louds->getFieldOrder();
            block.dict = std::move(dict_ptr);
            block.trie = std::move(trie);
            block.config = config;
            
            blocks.push_back(std::move(block));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception during type-aware deserialization: " << e.what() 
                  << " (offset=" << offset << ")" << std::endl;
        throw;
    }
    
    return blocks;
}

} // namespace json2