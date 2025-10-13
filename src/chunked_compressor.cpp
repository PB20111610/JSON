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
#include <sys/stat.h>

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

// ========== 细粒度分块压缩方法 ==========

void ChunkedTrieCompressor::enableGranularCompression(bool enable) {
    config_.enable_granular_compression = enable;
    // std::cerr << "[DEBUG] Granular compression " << (enable ? "enabled" : "disabled") << std::endl;
}

bool ChunkedTrieCompressor::isGranularCompressionEnabled() const {
    return config_.enable_granular_compression;
}

void ChunkedTrieCompressor::enableLayerSeparation(bool enable) {
    config_.enable_layer_separation = enable;
    // std::cerr << "[DEBUG] Layer separation " << (enable ? "enabled" : "disabled") << std::endl;
}

bool ChunkedTrieCompressor::isLayerSeparationEnabled() const {
    return config_.enable_layer_separation;
}

void ChunkedTrieCompressor::finalizeCurrentBlock() {
    if (current_block_count_ > 0 && !block_buffer_.empty()) {
        // Use a separate dictionary for analysis to avoid polluting the real compression dictionary
        FieldDictionaryManager analysis_dict;
        analysis_dict.setTimestampFields(timestamp_fields_);
        analysis_dict.setStructurizeArrays(config_.structurize_arrays);  // 设置数组处理模式
        std::vector<FieldKey> ordered_fields;
        
        // Choose field analysis method based on configuration
        if (isUsingCustomOrder()) {
            // Use custom field order provided by user
            FieldAnalyzer::analyzeWithCustomOrder(config_.custom_field_order, ordered_fields);
        } else {
            // Use traditional redundancy-based field analysis on analysis_dict (sample-only)
            FieldAnalyzer::analyzeAndSortFields(chunk_stat_buffer_, analysis_dict, ordered_fields);
        }
        std::cerr << "[DEBUG] Finalizing block. Record count: " << current_block_count_ << ", Field count: " << ordered_fields.size() << std::endl;
        
        // 调试输出：压缩前的字段序列
        // std::cerr << "[DEBUG] 压缩前字段序列:" << std::endl;
        // for (size_t i = 0; i < ordered_fields.size(); ++i) {
        //     std::cerr << "  [" << i << "] " << ordered_fields[i].name << " (type=" << static_cast<int>(ordered_fields[i].type) << ")" << std::endl;
        // }
        // Create a clean dictionary for actual Trie building and compression
        FieldDictionaryManager dict;
        dict.setTimestampFields(timestamp_fields_);
        dict.setStructurizeArrays(config_.structurize_arrays);  // 设置数组处理模式
        Trie trie(ordered_fields);
        simdjson::dom::parser parser;
        for (const auto& rec : block_buffer_) {
            trie.insert(rec, dict, parser);
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
        std::cerr << "[TRIE_STATS] Block " << blocks_memory_.size() 
                  << ": Placeholder ratio = " << std::fixed << std::setprecision(4) 
                  << (placeholder_ratio * 100) << "%, Structure " 
                  << (is_appropriate ? "appropriate" : "needs optimization") << std::endl;
        
        // 根据配置选择压缩方式
        ChunkedBlockMemory block_mem;
        block_mem.placeholder_ratio = placeholder_ratio;
        block_mem.is_structure_appropriate = is_appropriate;
        
        if (config_.enable_granular_compression) {
            // 使用细粒度压缩
            std::cerr << "[DEBUG] Using granular compression for block " << blocks_memory_.size() << std::endl;
            
            auto granular_compressed = Compressor::compressGranularLouds(louds, dict, louds.getFieldOrder(), config_.enable_layer_separation);
            
            // 存储细粒度压缩数据
            block_mem.is_granular = true;
            block_mem.use_layer_separation = config_.enable_layer_separation;
            block_mem.trie_bitmap = granular_compressed.trie_bitmap;
            block_mem.string_dict = granular_compressed.string_dict;
            block_mem.timestamp_dict = granular_compressed.timestamp_dict;
            block_mem.logtype_dict = granular_compressed.logtype_dict;
            block_mem.metadata = granular_compressed.metadata;
            block_mem.layer_sizes = granular_compressed.layer_sizes; // 添加层大小信息
            
            if (config_.enable_layer_separation) {
                block_mem.layer_data_by_level = granular_compressed.layer_data_by_level;
            } else {
                block_mem.layer_data_combined = granular_compressed.layer_data_combined;
            }
            
            // 存储统计信息
            block_mem.original_size = granular_compressed.original_size;
            block_mem.trie_original_size = granular_compressed.trie_original_size;
            block_mem.string_dict_original_size = granular_compressed.string_dict_original_size;
            block_mem.timestamp_dict_original_size = granular_compressed.timestamp_dict_original_size;
            block_mem.logtype_dict_original_size = granular_compressed.logtype_dict_original_size;
            block_mem.layer_original_size = granular_compressed.layer_original_size;
            block_mem.layer_sizes_original_size = granular_compressed.layer_sizes_original_size; // 添加层大小信息原始大小
            block_mem.metadata_original_size = granular_compressed.metadata_original_size;
            
            // 细粒度压缩模式下，compressed_data为空
            // 实际数据存储在各自的字段中，支持部分解压缩
            block_mem.compressed_data.clear();
            
            std::cerr << "[GRANULAR_STATS] Block " << blocks_memory_.size() 
                      << ": Original=" << granular_compressed.original_size 
                      << ", Compressed=" << granular_compressed.compressed_size 
                      << ", Ratio=" << std::fixed << std::setprecision(2) 
                      << Compressor::getGranularCompressionRatio(granular_compressed) << "x" << std::endl;
        } else {
            // 使用传统压缩
            std::cerr << "[DEBUG] Using traditional compression for block " << blocks_memory_.size() << std::endl;
            
            auto compressed = Compressor::compressLouds(louds, dict, louds.getFieldOrder());
            block_mem.is_granular = false;
            block_mem.compressed_data = Compressor::saveToMemory(compressed);
            block_mem.original_size = compressed.original_size;
        }
        
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

// 将细粒度压缩数据序列化为内存中的目录结构
std::vector<uint8_t> ChunkedTrieCompressor::serializeGranularToMemory() const {
    std::vector<uint8_t> result;
    
    // 写入全局元数据
    // 1. 块数量
    uint32_t block_count = static_cast<uint32_t>(blocks_memory_.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_count), reinterpret_cast<uint8_t*>(&block_count) + sizeof(block_count));
    
    // 2. 全局配置信息
    uint8_t granular_enabled = config_.enable_granular_compression ? 1 : 0;
    uint8_t layer_separation = config_.enable_layer_separation ? 1 : 0;
    result.push_back(granular_enabled);
    result.push_back(layer_separation);
    
    // 3. 每个块的元数据（块索引、大小信息等）
    for (size_t i = 0; i < blocks_memory_.size(); ++i) {
        const auto& block = blocks_memory_[i];
        
        // 块索引
        uint32_t block_index = static_cast<uint32_t>(i);
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_index), reinterpret_cast<uint8_t*>(&block_index) + sizeof(block_index));
        
        // 块大小信息
        uint32_t trie_size = static_cast<uint32_t>(block.trie_bitmap.size());
        uint32_t string_dict_size = static_cast<uint32_t>(block.string_dict.size());
        uint32_t timestamp_dict_size = static_cast<uint32_t>(block.timestamp_dict.size());
        uint32_t logtype_dict_size = static_cast<uint32_t>(block.logtype_dict.size());
        uint32_t metadata_size = static_cast<uint32_t>(block.metadata.size());
        
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&trie_size), reinterpret_cast<uint8_t*>(&trie_size) + sizeof(trie_size));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&string_dict_size), reinterpret_cast<uint8_t*>(&string_dict_size) + sizeof(string_dict_size));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&timestamp_dict_size), reinterpret_cast<uint8_t*>(&timestamp_dict_size) + sizeof(timestamp_dict_size));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&logtype_dict_size), reinterpret_cast<uint8_t*>(&logtype_dict_size) + sizeof(logtype_dict_size));
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&metadata_size), reinterpret_cast<uint8_t*>(&metadata_size) + sizeof(metadata_size));
        
        // 层数据大小
        if (block.use_layer_separation) {
            uint32_t layer_count = static_cast<uint32_t>(block.layer_data_by_level.size());
            result.insert(result.end(), reinterpret_cast<uint8_t*>(&layer_count), reinterpret_cast<uint8_t*>(&layer_count) + sizeof(layer_count));
            for (const auto& layer : block.layer_data_by_level) {
                uint32_t layer_size = static_cast<uint32_t>(layer.size());
                result.insert(result.end(), reinterpret_cast<uint8_t*>(&layer_size), reinterpret_cast<uint8_t*>(&layer_size) + sizeof(layer_size));
            }
            // 层大小信息大小
            uint32_t layer_sizes_size = static_cast<uint32_t>(block.layer_sizes.size());
            result.insert(result.end(), reinterpret_cast<uint8_t*>(&layer_sizes_size), reinterpret_cast<uint8_t*>(&layer_sizes_size) + sizeof(layer_sizes_size));
        } else {
            uint32_t combined_size = static_cast<uint32_t>(block.layer_data_combined.size());
            result.insert(result.end(), reinterpret_cast<uint8_t*>(&combined_size), reinterpret_cast<uint8_t*>(&combined_size) + sizeof(combined_size));
        }
    }
    
    // 4. 写入所有块的数据（按块组织）
    for (const auto& block : blocks_memory_) {
        // Trie位图数据
        result.insert(result.end(), block.trie_bitmap.begin(), block.trie_bitmap.end());
        // 字典数据
        result.insert(result.end(), block.string_dict.begin(), block.string_dict.end());
        result.insert(result.end(), block.timestamp_dict.begin(), block.timestamp_dict.end());
        result.insert(result.end(), block.logtype_dict.begin(), block.logtype_dict.end());
        result.insert(result.end(), block.metadata.begin(), block.metadata.end());
        // 层数据
        if (block.use_layer_separation) {
            for (const auto& layer : block.layer_data_by_level) {
                result.insert(result.end(), layer.begin(), layer.end());
            }
            // 层大小信息
            result.insert(result.end(), block.layer_sizes.begin(), block.layer_sizes.end());
        } else {
            result.insert(result.end(), block.layer_data_combined.begin(), block.layer_data_combined.end());
        }
    }
    
    std::cerr << "[SER] Granular mode: Total blocks: " << blocks_memory_.size() << ", Total records: " << total_records_ << std::endl;
    return result;
}

std::vector<uint8_t> ChunkedTrieCompressor::serialize() {
    finalizeCurrentBlock();
    
    if (config_.enable_granular_compression) {
        // 细粒度压缩模式：使用目录结构存储，返回元数据
        return serializeGranularToMemory();
    } else {
        // 传统压缩模式：使用单一数据块
        std::vector<uint8_t> result;
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
        if (block.is_granular) {
            // 细粒度压缩：计算各组件压缩后大小
            size_t block_compressed_size = block.trie_bitmap.size() + block.string_dict.size() + 
                                         block.timestamp_dict.size() + block.logtype_dict.size() + 
                                         block.metadata.size();
            if (block.use_layer_separation) {
                for (const auto& layer : block.layer_data_by_level) {
                    block_compressed_size += layer.size();
                }
                block_compressed_size += block.layer_sizes.size(); // 添加层大小信息的压缩大小
            } else {
                block_compressed_size += block.layer_data_combined.size();
            }
            total_compressed += block_compressed_size;
            stats.block_sizes.push_back(block_compressed_size);
        } else {
            // 传统压缩
            total_compressed += block.compressed_data.size();
            stats.block_sizes.push_back(block.compressed_data.size());
        }
        
        total_core_data += block.original_size;
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
        if (data.size() < sizeof(uint32_t)) throw std::runtime_error("Insufficient data for block count");
        uint32_t block_count = 0;
        std::memcpy(&block_count, &data[offset], sizeof(block_count));
        offset += sizeof(block_count);
        // 判断是否有granular_enabled字段
        if (data.size() > offset + 1) {
            uint8_t granular_enabled = data[offset];
            uint8_t layer_separation = data[offset + 1];
            std::cerr << "[DESER] block_count=" << block_count << ", granular_enabled=" << (int)granular_enabled << ", layer_separation=" << (int)layer_separation << ", offset=" << offset << std::endl;
            // 细粒度格式magic: granular_enabled==1
            if (granular_enabled == 1) {
                offset += 2;
                // 读取每个块的元数据
                struct BlockMeta {
                    uint32_t trie_size, string_dict_size, timestamp_dict_size, logtype_dict_size, metadata_size;
                    uint32_t layer_count_or_combined_size;
                    uint32_t layer_sizes_size; // 层大小信息大小
                    std::vector<uint32_t> layer_sizes; // 如果分层
                };
                std::vector<BlockMeta> metas(block_count);
                for (uint32_t i = 0; i < block_count; ++i) {
                    BlockMeta meta;
                    uint32_t block_index = 0;
                    std::memcpy(&block_index, &data[offset], sizeof(block_index));
                    std::cerr << "[DESER] Block " << i << " index=" << block_index << ", offset=" << offset << std::endl;
                    offset += sizeof(block_index);
                    std::memcpy(&meta.trie_size, &data[offset], sizeof(meta.trie_size));
                    std::cerr << "[DESER] Block " << i << " trie_size=" << meta.trie_size << ", offset=" << offset << std::endl;
                    offset += sizeof(meta.trie_size);
                    std::memcpy(&meta.string_dict_size, &data[offset], sizeof(meta.string_dict_size));
                    offset += sizeof(meta.string_dict_size);
                    std::memcpy(&meta.timestamp_dict_size, &data[offset], sizeof(meta.timestamp_dict_size));
                    offset += sizeof(meta.timestamp_dict_size);
                    std::memcpy(&meta.logtype_dict_size, &data[offset], sizeof(meta.logtype_dict_size));
                    offset += sizeof(meta.logtype_dict_size);
                    std::memcpy(&meta.metadata_size, &data[offset], sizeof(meta.metadata_size));
                    offset += sizeof(meta.metadata_size);
                    if (layer_separation) {
                        std::memcpy(&meta.layer_count_or_combined_size, &data[offset], sizeof(meta.layer_count_or_combined_size));
                        std::cerr << "[DESER] Block " << i << " layer_count=" << meta.layer_count_or_combined_size << ", offset=" << offset << std::endl;
                        offset += sizeof(meta.layer_count_or_combined_size);
                        meta.layer_sizes.resize(meta.layer_count_or_combined_size);
                        for (uint32_t l = 0; l < meta.layer_count_or_combined_size; ++l) {
                            std::memcpy(&meta.layer_sizes[l], &data[offset], sizeof(uint32_t));
                            offset += sizeof(uint32_t);
                        }
                        // 读取层大小信息大小
                        std::memcpy(&meta.layer_sizes_size, &data[offset], sizeof(meta.layer_sizes_size));
                        std::cerr << "[DESER] Block " << i << " layer_sizes_size=" << meta.layer_sizes_size << ", offset=" << offset << std::endl;
                        offset += sizeof(meta.layer_sizes_size);
                    } else {
                        std::memcpy(&meta.layer_count_or_combined_size, &data[offset], sizeof(meta.layer_count_or_combined_size));
                        std::cerr << "[DESER] Block " << i << " combined_layer_size=" << meta.layer_count_or_combined_size << ", offset=" << offset << std::endl;
                        offset += sizeof(meta.layer_count_or_combined_size);
                    }
                    metas[i] = meta;
                }
                // 读取每个块的数据
                for (uint32_t i = 0; i < block_count; ++i) {
                    const auto& meta = metas[i];
                    GranularCompressedData gdata;
                    gdata.use_layer_separation = layer_separation;
                    // 依次读取各组件
                    auto read_vec = [&](uint32_t sz, const char* name) {
                        std::cerr << "[DESER] Block " << i << " reading " << name << " size=" << sz << ", offset=" << offset << std::endl;
                        if (sz == 0) return std::vector<uint8_t>{};
                        if (data.size() < offset + sz) throw std::runtime_error("Buffer too small for component");
                        std::vector<uint8_t> v(data.begin() + offset, data.begin() + offset + sz);
                        offset += sz;
                        return v;
                    };
                    gdata.trie_bitmap = read_vec(meta.trie_size, "trie_bitmap");
                    gdata.string_dict = read_vec(meta.string_dict_size, "string_dict");
                    gdata.timestamp_dict = read_vec(meta.timestamp_dict_size, "timestamp_dict");
                    gdata.logtype_dict = read_vec(meta.logtype_dict_size, "logtype_dict");
                    gdata.metadata = read_vec(meta.metadata_size, "metadata");
                    if (layer_separation) {
                        gdata.layer_data_by_level.resize(meta.layer_count_or_combined_size);
                        for (uint32_t l = 0; l < meta.layer_count_or_combined_size; ++l) {
                            gdata.layer_data_by_level[l] = read_vec(meta.layer_sizes[l], "layer");
                        }
                        // 读取层大小信息
                        gdata.layer_sizes = read_vec(meta.layer_sizes_size, "layer_sizes");
                    } else {
                        gdata.layer_data_combined = read_vec(meta.layer_count_or_combined_size, "layer_combined");
                    }
                    // 解压
                    std::cerr << "[DESER] Block " << i << " decompressGranular..." << std::endl;
                    auto [trie, dict_ptr] = Compressor::decompressGranular(gdata);
                    blocks.push_back(ChunkedBlock{trie->getOrderedFields(), std::move(dict_ptr), std::move(trie)});
                }
                std::cerr << "[DESER] Finished granular deserializing " << block_count << " blocks." << std::endl;
                return blocks;
            }
        }
        // 否则走传统格式
        offset -= sizeof(block_count); // 回退到block_count位置
        uint32_t block_count2 = 0;
        std::memcpy(&block_count2, &data[offset], sizeof(block_count2));
        offset += sizeof(block_count2);
        for (uint32_t i = 0; i < block_count2; ++i) {
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
        std::cerr << "[DESER] Finished traditional deserializing " << block_count2 << " blocks." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception during deserialization: " << e.what() << " (offset=" << offset << ")" << std::endl;
        throw;
    }
    return blocks;
}

// ========== 文件系统存储实现 ==========

bool ChunkedTrieCompressor::saveToDirectory(const std::string& directory_path) {
    try {
        // 确保最后一个块被处理
        finalizeCurrentBlock();

        // 创建主目录和chunks目录
        mkdir(directory_path.c_str(), 0777);
        std::string chunks_dir = directory_path + "/chunks";
        mkdir(chunks_dir.c_str(), 0777);

        // 保存全局元数据
        std::ofstream global_metadata(directory_path + "/metadata.json2", std::ios::binary);
        if (!global_metadata.is_open()) {
            std::cerr << "[ERROR] Cannot create global metadata file" << std::endl;
            return false;
        }
        // 写入全局元数据
        auto stats = getStats();
        uint32_t total_blocks = static_cast<uint32_t>(stats.total_blocks);
        uint64_t total_records = static_cast<uint64_t>(stats.total_records);
        uint64_t original_size = static_cast<uint64_t>(stats.original_file_size);
        global_metadata.write(reinterpret_cast<const char*>(&total_blocks), sizeof(total_blocks));
        global_metadata.write(reinterpret_cast<const char*>(&total_records), sizeof(total_records));
        global_metadata.write(reinterpret_cast<const char*>(&original_size), sizeof(original_size));
        global_metadata.close();

        // 保存每个块
        for (size_t i = 0; i < blocks_memory_.size(); ++i) {
            const auto& block = blocks_memory_[i];
            char chunk_dir_buf[256];
            snprintf(chunk_dir_buf, sizeof(chunk_dir_buf), "%s/chunk_%06zu", chunks_dir.c_str(), i);
            std::string block_dir = chunk_dir_buf;
            mkdir(block_dir.c_str(), 0777);

            if (block.is_granular) {
                // 细粒度分组件写入（保持原有实现）
                // 保存Trie位图
                std::ofstream trie_file(block_dir + "/louds.json2", std::ios::binary);
                if (trie_file.is_open()) {
                    uint32_t size = static_cast<uint32_t>(block.trie_bitmap.size());
                    trie_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                    trie_file.write(reinterpret_cast<const char*>(block.trie_bitmap.data()), size);
                    trie_file.close();
                } else {
                    std::cerr << "[ERROR] Cannot create " << block_dir << "/data.json2" << std::endl;
                }
                
                // 创建字典目录
                std::string dict_dir = block_dir + "/dictionaries";
                mkdir(dict_dir.c_str(), 0777);
                if (!block.string_dict.empty()) {
                    std::ofstream string_file(dict_dir + "/variables.json2", std::ios::binary);
                    if (string_file.is_open()) {
                        uint32_t size = static_cast<uint32_t>(block.string_dict.size());
                        string_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        string_file.write(reinterpret_cast<const char*>(block.string_dict.data()), size);
                        string_file.close();
                    } else {
                        std::cerr << "[ERROR] Cannot create " << dict_dir << "/variables.json2" << std::endl;
                    }
                }
                
                // 保存Timestamp字典
                if (!block.timestamp_dict.empty()) {
                    std::ofstream ts_file(dict_dir + "/timestamps.json2", std::ios::binary);
                    if (ts_file.is_open()) {
                        uint32_t size = static_cast<uint32_t>(block.timestamp_dict.size());
                        ts_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        ts_file.write(reinterpret_cast<const char*>(block.timestamp_dict.data()), size);
                        ts_file.close();
                    } else {
                        std::cerr << "[ERROR] Cannot create " << dict_dir << "/timestamps.json2" << std::endl;
                    }
                }
                
                // 保存LogType字典
                if (!block.logtype_dict.empty()) {
                    std::ofstream log_file(dict_dir + "/logtypes.json2", std::ios::binary);
                    if (log_file.is_open()) {
                        uint32_t size = static_cast<uint32_t>(block.logtype_dict.size());
                        log_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        log_file.write(reinterpret_cast<const char*>(block.logtype_dict.data()), size);
                        log_file.close();
                    } else {
                        std::cerr << "[ERROR] Cannot create " << dict_dir << "/logtypes.json2" << std::endl;
                    }
                }
                
                // 保存分层数据
                if (block.use_layer_separation) {
                    // 按层分别保存
                    for (size_t layer_idx = 0; layer_idx < block.layer_data_by_level.size(); ++layer_idx) {
                        char layer_file_buf[256];
                        snprintf(layer_file_buf, sizeof(layer_file_buf), "%s/layer_%zu.json2", block_dir.c_str(), layer_idx);
                        std::ofstream layer_stream(layer_file_buf, std::ios::binary);
                        if (layer_stream.is_open()) {
                            const auto& layer_data = block.layer_data_by_level[layer_idx];
                            uint32_t size = static_cast<uint32_t>(layer_data.size());
                            layer_stream.write(reinterpret_cast<const char*>(&size), sizeof(size));
                            layer_stream.write(reinterpret_cast<const char*>(layer_data.data()), size);
                            layer_stream.close();
                        } else {
                            std::cerr << "[ERROR] Cannot create " << layer_file_buf << std::endl;
                        }
                    }
                    // 保存层大小信息
                    std::ofstream layer_sizes_file(block_dir + "/layer_sizes.json2", std::ios::binary);
                    if (layer_sizes_file.is_open()) {
                        uint32_t size = static_cast<uint32_t>(block.layer_sizes.size());
                        layer_sizes_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        layer_sizes_file.write(reinterpret_cast<const char*>(block.layer_sizes.data()), size);
                        layer_sizes_file.close();
                    } else {
                        std::cerr << "[ERROR] Cannot create " << block_dir << "/layer_sizes.json2" << std::endl;
                    }
                } else {
                    std::ofstream layers_file(block_dir + "/layers.json2", std::ios::binary);
                    if (layers_file.is_open()) {
                        uint32_t size = static_cast<uint32_t>(block.layer_data_combined.size());
                        layers_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        layers_file.write(reinterpret_cast<const char*>(block.layer_data_combined.data()), size);
                        layers_file.close();
                    } else {
                        std::cerr << "[ERROR] Cannot create " << block_dir << "/layers.json2" << std::endl;
                    }
                }
                
                // 保存细粒度压缩的元数据文件
                if (!block.metadata.empty()) {
                    std::ofstream metadata_file(block_dir + "/metadata.json2", std::ios::binary);
                    if (metadata_file.is_open()) {
                        uint32_t size = static_cast<uint32_t>(block.metadata.size());
                        metadata_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        metadata_file.write(reinterpret_cast<const char*>(block.metadata.data()), size);
                        metadata_file.close();
                    } else {
                        std::cerr << "[ERROR] Cannot create " << block_dir << "/metadata.json2" << std::endl;
                    }
                }
                
                // 保存块级别的元数据
                std::ofstream block_metadata(block_dir + "/block_metadata.json2", std::ios::binary);
                if (block_metadata.is_open()) {
                    block_metadata.write(reinterpret_cast<const char*>(&block.original_size), sizeof(block.original_size));
                    block_metadata.write(reinterpret_cast<const char*>(&block.placeholder_ratio), sizeof(block.placeholder_ratio));
                    uint8_t flags = (block.is_structure_appropriate ? 1 : 0) |
                                   (block.use_layer_separation ? 2 : 0);
                    block_metadata.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
                    block_metadata.close();
                } else {
                    std::cerr << "[ERROR] Cannot create " << block_dir << "/block_metadata.json2" << std::endl;
                }
            } else {
                // 传统压缩：单文件写入
                std::ofstream block_file(block_dir + "/data.json2", std::ios::binary);
                if (block_file.is_open()) {
                    uint32_t size = static_cast<uint32_t>(block.compressed_data.size());
                    block_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                    block_file.write(reinterpret_cast<const char*>(block.compressed_data.data()), size);
                    block_file.close();
                } else {
                    std::cerr << "[ERROR] Cannot create " << block_dir << "/data.json2" << std::endl;
                }
            }
        }
        std::cerr << "[SUCCESS] Saved " << blocks_memory_.size() << " blocks to " << directory_path << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to save to directory: " << e.what() << std::endl;
        return false;
    }
}

std::vector<ChunkedTrieCompressor::ChunkedBlock> ChunkedTrieCompressor::loadFromDirectory(const std::string& directory_path) {
    SelectiveLoadOptions options; // 默认加载所有组件
    return loadFromDirectorySelective(directory_path, options);
}

std::vector<ChunkedTrieCompressor::ChunkedBlock> ChunkedTrieCompressor::loadFromDirectorySelective(const std::string& directory_path, const SelectiveLoadOptions& options) {
    std::vector<ChunkedBlock> blocks;
    
    try {
        // 读取全局元数据
        std::ifstream global_metadata(directory_path + "/metadata.json2", std::ios::binary);
        if (!global_metadata.is_open()) {
            std::cerr << "[ERROR] Cannot open global metadata file" << std::endl;
            return blocks;
        }
        
        uint32_t total_blocks;
        uint64_t total_records, original_size;
        global_metadata.read(reinterpret_cast<char*>(&total_blocks), sizeof(total_blocks));
        global_metadata.read(reinterpret_cast<char*>(&total_records), sizeof(total_records));
        global_metadata.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
        global_metadata.close();
        
        std::cerr << "[DEBUG] Loading " << total_blocks << " blocks from " << directory_path << std::endl;
        
        // 加载指定的块
        for (uint32_t i = 0; i < total_blocks; ++i) {
            // 检查是否需要加载这个块
            if (!options.specific_chunks.empty() && 
                std::find(options.specific_chunks.begin(), options.specific_chunks.end(), i) == options.specific_chunks.end()) {
                continue;
            }
            
            char chunk_dir_buf[256];
            snprintf(chunk_dir_buf, sizeof(chunk_dir_buf), "%s/chunks/chunk_%06u", directory_path.c_str(), i);
            std::string block_dir = chunk_dir_buf;
            
            // 尝试检测是否为细粒度压缩
            std::string dict_dir = block_dir + "/dictionaries";
            std::string variables_file = dict_dir + "/variables.json2";
            std::ifstream test_granular(variables_file, std::ios::binary);
            bool is_granular = test_granular.is_open();
            // std::cerr << "[DEBUG] Checking granular detection: file=" << variables_file << ", exists=" << is_granular << std::endl;
            test_granular.close();
            
            if (is_granular) {
                // 细粒度压缩块的加载
                // std::cerr << "[DEBUG] Loading granular block " << i << std::endl;
                
                try {
                    // 1. 读取块元数据
                    std::ifstream block_metadata(block_dir + "/block_metadata.json2", std::ios::binary);
                    if (!block_metadata.is_open()) {
                        std::cerr << "[ERROR] Cannot open block metadata for block " << i << std::endl;
                        continue;
                    }
                    
                    size_t original_size;
                    double placeholder_ratio;
                    uint8_t flags;
                    block_metadata.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
                    block_metadata.read(reinterpret_cast<char*>(&placeholder_ratio), sizeof(placeholder_ratio));
                    block_metadata.read(reinterpret_cast<char*>(&flags), sizeof(flags));
                    block_metadata.close();
                    
                    // 2. 重建GranularCompressedData结构
                    GranularCompressedData gdata;
                    gdata.use_layer_separation = (flags & 2) != 0;
                    
                    // 读取Trie位图
                    std::ifstream trie_file(block_dir + "/louds.json2", std::ios::binary);
                    if (trie_file.is_open()) {
                        uint32_t trie_size;
                        trie_file.read(reinterpret_cast<char*>(&trie_size), sizeof(trie_size));
                        gdata.trie_bitmap.resize(trie_size);
                        trie_file.read(reinterpret_cast<char*>(gdata.trie_bitmap.data()), trie_size);
                        trie_file.close();
                    }
                    
                    // 读取字典数据
                    std::string dict_dir = block_dir + "/dictionaries";
                    
                    // 字符串字典
                    std::ifstream string_file(dict_dir + "/variables.json2", std::ios::binary);
                    if (string_file.is_open()) {
                        uint32_t string_size;
                        string_file.read(reinterpret_cast<char*>(&string_size), sizeof(string_size));
                        gdata.string_dict.resize(string_size);
                        string_file.read(reinterpret_cast<char*>(gdata.string_dict.data()), string_size);
                        string_file.close();
                    }
                    
                    // 时间戳字典
                    std::ifstream ts_file(dict_dir + "/timestamps.json2", std::ios::binary);
                    if (ts_file.is_open()) {
                        uint32_t ts_size;
                        ts_file.read(reinterpret_cast<char*>(&ts_size), sizeof(ts_size));
                        gdata.timestamp_dict.resize(ts_size);
                        ts_file.read(reinterpret_cast<char*>(gdata.timestamp_dict.data()), ts_size);
                        ts_file.close();
                    }
                    
                    // LogType字典
                    std::ifstream log_file(dict_dir + "/logtypes.json2", std::ios::binary);
                    if (log_file.is_open()) {
                        uint32_t log_size;
                        log_file.read(reinterpret_cast<char*>(&log_size), sizeof(log_size));
                        gdata.logtype_dict.resize(log_size);
                        log_file.read(reinterpret_cast<char*>(gdata.logtype_dict.data()), log_size);
                        log_file.close();
                    }
                    
                    // 元数据文件
                    std::ifstream metadata_file(block_dir + "/metadata.json2", std::ios::binary);
                    if (metadata_file.is_open()) {
                        uint32_t metadata_size;
                        metadata_file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
                        gdata.metadata.resize(metadata_size);
                        metadata_file.read(reinterpret_cast<char*>(gdata.metadata.data()), metadata_size);
                        metadata_file.close();
                    }
                    
                    // 读取层数据
                    if (gdata.use_layer_separation) {
                        // 按层分别读取
                        size_t layer_idx = 0;
                        while (true) {
                            char layer_file_buf[256];
                            snprintf(layer_file_buf, sizeof(layer_file_buf), "%s/layer_%zu.json2", block_dir.c_str(), layer_idx);
                            std::ifstream layer_stream(layer_file_buf, std::ios::binary);
                            if (!layer_stream.is_open()) break;
                            
                            uint32_t layer_size;
                            layer_stream.read(reinterpret_cast<char*>(&layer_size), sizeof(layer_size));
                            std::vector<uint8_t> layer_data(layer_size);
                            layer_stream.read(reinterpret_cast<char*>(layer_data.data()), layer_size);
                            gdata.layer_data_by_level.push_back(std::move(layer_data));
                            layer_stream.close();
                            layer_idx++;
                        }
                        // 读取层大小信息
                        std::ifstream layer_sizes_file(block_dir + "/layer_sizes.json2", std::ios::binary);
                        if (layer_sizes_file.is_open()) {
                            uint32_t layer_sizes_size;
                            layer_sizes_file.read(reinterpret_cast<char*>(&layer_sizes_size), sizeof(layer_sizes_size));
                            gdata.layer_sizes.resize(layer_sizes_size);
                            layer_sizes_file.read(reinterpret_cast<char*>(gdata.layer_sizes.data()), layer_sizes_size);
                            layer_sizes_file.close();
                        }
                    } else {
                        // 整体读取
                        std::ifstream layers_file(block_dir + "/layers.json2", std::ios::binary);
                        if (layers_file.is_open()) {
                            uint32_t layers_size;
                            layers_file.read(reinterpret_cast<char*>(&layers_size), sizeof(layers_size));
                            gdata.layer_data_combined.resize(layers_size);
                            layers_file.read(reinterpret_cast<char*>(gdata.layer_data_combined.data()), layers_size);
                            layers_file.close();
                        }
                    }
                    
                    // 3. 使用Compressor::decompressGranular解压
                    // std::cerr << "[DEBUG] Decompressing granular block " << i << std::endl;
                    // std::cerr << "[DEBUG] Granular data sizes: trie=" << gdata.trie_bitmap.size() 
                    //           << ", string=" << gdata.string_dict.size() 
                    //           << ", timestamp=" << gdata.timestamp_dict.size() 
                    //           << ", logtype=" << gdata.logtype_dict.size() 
                    //           << ", metadata=" << gdata.metadata.size() << std::endl;
                    auto [trie, dict_ptr] = Compressor::decompressGranular(gdata);
                    
                    blocks.push_back(ChunkedBlock{trie->getOrderedFields(), std::move(dict_ptr), std::move(trie)});
                    // std::cerr << "[DEBUG] Successfully loaded granular block " << i << std::endl;
                    
                } catch (const std::exception& e) {
                    std::cerr << "[ERROR] Failed to load granular block " << i << ": " << e.what() << std::endl;
                }
                
            } else {
                // 传统压缩块的加载
                std::cerr << "[DEBUG] Loading traditional block " << i << std::endl;
                
                std::ifstream block_file(block_dir + "/data.json2", std::ios::binary);
                if (block_file.is_open()) {
                    uint32_t size;
                    block_file.read(reinterpret_cast<char*>(&size), sizeof(size));
                    
                    std::vector<uint8_t> block_data(size);
                    block_file.read(reinterpret_cast<char*>(block_data.data()), size);
                    block_file.close();
                    
                    // 反序列化为CompressedData
                    CompressedData cd = Compressor::loadFromMemory(block_data);
                    
                    // 解压
                    auto [louds, dict_ptr] = Compressor::decompressLouds(cd);
                    auto trie = std::make_unique<Trie>(louds->getFieldOrder());
                    loudsToTrie(*louds, *trie);
                    
                    blocks.push_back(ChunkedBlock{louds->getFieldOrder(), std::move(dict_ptr), std::move(trie)});
                }
            }
        }
        
        std::cerr << "[SUCCESS] Loaded " << blocks.size() << " blocks from " << directory_path << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load from directory: " << e.what() << std::endl;
    }
    
    return blocks;
}

} // namespace json2