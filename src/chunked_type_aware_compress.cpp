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

// ========== 细粒度类型敏感压缩方法 ==========

void ChunkedTypeAwareCompressor::enableGranularCompression(bool enable) {
    config_.enable_granular_compression = enable;
}

bool ChunkedTypeAwareCompressor::isGranularCompressionEnabled() const {
    return config_.enable_granular_compression;
}

void ChunkedTypeAwareCompressor::enableLayerSeparation(bool enable) {
    config_.enable_layer_separation = enable;
}

bool ChunkedTypeAwareCompressor::isLayerSeparationEnabled() const {
    return config_.enable_layer_separation;
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
            // Setup analysis dictionary manager (used only for field order analysis)
            FieldDictionaryManager analysis_dict;
            analysis_dict.setTimestampFields(timestamp_fields_);
            analysis_dict.setStructurizeArrays(config_.structurize_arrays);
            
            std::vector<FieldKey> ordered_fields;
            
            // Choose field analysis method based on configuration
            if (isUsingCustomOrder()) {
                // Use custom field order provided by user
                FieldAnalyzer::analyzeWithCustomOrder(config_.custom_field_order, ordered_fields);
            } else {
                // Use traditional redundancy-based field analysis on analysis_dict (sample-only)
                FieldAnalyzer::analyzeAndSortFields(chunk_stat_buffer_, analysis_dict, ordered_fields);
            }
            
            // Create a clean dictionary for actual Trie building and compression
            FieldDictionaryManager dict;
            dict.setTimestampFields(timestamp_fields_);
            dict.setStructurizeArrays(config_.structurize_arrays);
            
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
            
            // Analyze placeholder ratio
            auto [placeholder_ratio, is_appropriate] = analyzePlaceholderRatio(trie);
            std::cerr << "[TYPE_AWARE_TRIE_STATS] Block " << blocks_memory_.size() 
                      << ": Placeholder ratio = " << std::fixed << std::setprecision(4) 
                      << (placeholder_ratio * 100) << "%, Structure " 
                      << (is_appropriate ? "appropriate" : "needs optimization") << std::endl;
            
            // 根据配置选择压缩方式
            ChunkedTypeAwareBlockMemory block_mem;
            block_mem.placeholder_ratio = placeholder_ratio;
            block_mem.is_structure_appropriate = is_appropriate;
            block_mem.block_config = config_.type_aware_config;
            
            if (config_.enable_granular_compression) {
                // 使用细粒度类型敏感压缩
                
                auto granular_compressed = TypeAwareCompressor::compressGranularLouds(
                    louds, dict, louds.getFieldOrder(), config_.type_aware_config, config_.enable_layer_separation);
                
                // 存储细粒度压缩数据
                block_mem.is_granular = true;
                block_mem.use_layer_separation = config_.enable_layer_separation;
                block_mem.trie_bitmap = granular_compressed.trie_bitmap;
                block_mem.string_dict = granular_compressed.string_dict;
                block_mem.timestamp_dict = granular_compressed.timestamp_dict;
                block_mem.logtype_dict = granular_compressed.logtype_dict;
                block_mem.metadata = granular_compressed.metadata;
                
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
                block_mem.metadata_original_size = granular_compressed.metadata_original_size;
                
                // 细粒度压缩模式下，compressed_data为空
                // 实际数据存储在各自的字段中，支持部分解压缩
                block_mem.compressed_data.clear();
                
                std::cerr << "[TYPE_AWARE_GRANULAR_STATS] Block " << blocks_memory_.size() 
                          << ": Original=" << granular_compressed.original_size 
                          << ", Compressed=" << granular_compressed.compressed_size 
                          << ", Ratio=" << std::fixed << std::setprecision(2) 
                          << TypeAwareCompressor::getGranularCompressionRatio(granular_compressed) << "x" << std::endl;
            } else {
                // 使用传统类型敏感压缩
                auto compressed = TypeAwareCompressor::compressLouds(louds, dict, louds.getFieldOrder(), config_.type_aware_config);
                block_mem.is_granular = false;
                block_mem.compressed_data = Compressor::saveToMemory(compressed);
                block_mem.original_size = compressed.original_size;
            }
            
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
    // Finalize any remaining block
    finalizeCurrentBlock();
    
    if (config_.enable_granular_compression) {
        // 细粒度压缩模式：使用目录结构存储，返回元数据
        return serializeGranularToMemory();
    } else {
        // 传统压缩模式：使用单一数据块
        std::vector<uint8_t> result;
        try {
            // 1. Serialize block count
            uint32_t block_count = static_cast<uint32_t>(blocks_memory_.size());
            result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_count), 
                          reinterpret_cast<uint8_t*>(&block_count) + sizeof(block_count));
            // 2. Serialize each block
            for (const auto& block : blocks_memory_) {
                uint32_t block_size = static_cast<uint32_t>(block.compressed_data.size());
                result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_size), 
                              reinterpret_cast<uint8_t*>(&block_size) + sizeof(block_size));
                result.insert(result.end(), block.compressed_data.begin(), block.compressed_data.end());
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Serialization failed: " << e.what() << std::endl;
            throw;
        }
        return result;
    }
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

// 将细粒度类型敏感压缩数据序列化为内存中的目录结构
std::vector<uint8_t> ChunkedTypeAwareCompressor::serializeGranularToMemory() const {
    std::vector<uint8_t> result;
    
    // 写入全局元数据
    // 1. 块数量
    uint32_t block_count = static_cast<uint32_t>(blocks_memory_.size());
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_count), reinterpret_cast<uint8_t*>(&block_count) + sizeof(block_count));
    
    // 2. 全局配置信息
    uint8_t granular_enabled = config_.enable_granular_compression ? 1 : 0;
    uint8_t layer_separation = config_.enable_layer_separation ? 1 : 0;
    uint32_t compression_level = static_cast<uint32_t>(config_.type_aware_config.compression_level);
    result.push_back(granular_enabled);
    result.push_back(layer_separation);
    result.insert(result.end(), reinterpret_cast<uint8_t*>(&compression_level), reinterpret_cast<uint8_t*>(&compression_level) + sizeof(compression_level));
    
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
        } else {
            uint32_t combined_size = static_cast<uint32_t>(block.layer_data_combined.size());
            result.insert(result.end(), reinterpret_cast<uint8_t*>(&combined_size), reinterpret_cast<uint8_t*>(&combined_size) + sizeof(combined_size));
        }
        
        // 块配置信息
        uint32_t block_compression_level = static_cast<uint32_t>(block.block_config.compression_level);
        result.insert(result.end(), reinterpret_cast<uint8_t*>(&block_compression_level), reinterpret_cast<uint8_t*>(&block_compression_level) + sizeof(block_compression_level));
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
        } else {
            result.insert(result.end(), block.layer_data_combined.begin(), block.layer_data_combined.end());
        }
    }
    
    std::cerr << "[SER] Type-aware granular mode: Total blocks: " << blocks_memory_.size() << ", Total records: " << total_records_ << std::endl;
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
        if (block.is_granular) {
            // 细粒度压缩：计算各组件压缩后大小
            size_t block_compressed_size = block.trie_bitmap.size() + block.string_dict.size() + 
                                         block.timestamp_dict.size() + block.logtype_dict.size() + 
                                         block.metadata.size();
            if (block.use_layer_separation) {
                for (const auto& layer : block.layer_data_by_level) {
                    block_compressed_size += layer.size();
                }
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

std::vector<ChunkedTypeAwareBlock>
ChunkedTypeAwareCompressor::deserialize(const std::vector<uint8_t>& data) {
    std::vector<ChunkedTypeAwareBlock> blocks;
    size_t offset = 0;
    try {
        if (data.size() < sizeof(uint32_t)) throw std::runtime_error("Insufficient data for block count");
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
            // 使用TypeAware解压（用默认config）
            compression::TypeAwareCompressionConfig block_config; // 默认配置
            auto [louds, dict_ptr] = TypeAwareCompressor::decompressLouds(cd, block_config);
            auto trie = std::make_unique<Trie>(louds->getFieldOrder());
            loudsToTrie(*louds, *trie);
            blocks.push_back(ChunkedTypeAwareBlock{louds->getFieldOrder(), std::move(dict_ptr), std::move(trie), block_config});
        }
        std::cerr << "[DESER] Finished type-aware traditional deserializing " << block_count << " blocks." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception during type-aware deserialization: " << e.what() << " (offset=" << offset << ")" << std::endl;
        throw;
    }
    return blocks;
}

// ========== 文件系统存储实现（类型敏感版本） ==========

bool ChunkedTypeAwareCompressor::saveToDirectory(const std::string& directory_path) {
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
        // 写入全局元数据（类型敏感版本）
        auto stats = getStats();
        uint32_t total_blocks = static_cast<uint32_t>(stats.total_blocks);
        uint64_t total_records = static_cast<uint64_t>(stats.total_records);
        uint64_t original_size = static_cast<uint64_t>(stats.original_file_size);
        uint32_t compression_level = static_cast<uint32_t>(config_.type_aware_config.compression_level);
        global_metadata.write(reinterpret_cast<const char*>(&total_blocks), sizeof(total_blocks));
        global_metadata.write(reinterpret_cast<const char*>(&total_records), sizeof(total_records));
        global_metadata.write(reinterpret_cast<const char*>(&original_size), sizeof(original_size));
        global_metadata.write(reinterpret_cast<const char*>(&compression_level), sizeof(compression_level));
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
                std::string dict_dir = block_dir + "/dictionaries";
                mkdir(dict_dir.c_str(), 0777);
                if (!block.string_dict.empty()) {
                    std::string variables_file = dict_dir + "/variables.json2";
                    std::ofstream string_file(variables_file, std::ios::binary);
                    if (string_file.is_open()) {
                        uint32_t size = static_cast<uint32_t>(block.string_dict.size());
                        string_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
                        string_file.write(reinterpret_cast<const char*>(block.string_dict.data()), size);
                        string_file.close();
                    } else {
                        std::cerr << "[ERROR] Cannot create " << variables_file << std::endl;
                    }
                }

                // 保存timestamp字典
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
                } else {
                    // 整体保存
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
                    uint32_t config_compression_level = static_cast<uint32_t>(block.block_config.compression_level);
                    block_metadata.write(reinterpret_cast<const char*>(&config_compression_level), sizeof(config_compression_level));
                    uint8_t flags = (block.is_structure_appropriate ? 1 : 0) |
                                   (block.use_layer_separation ? 2 : 0) |
                                   (block.is_granular ? 4 : 0);
                    block_metadata.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
                    block_metadata.close();
                } else {
                    std::cerr << "[ERROR] Cannot create " << block_dir << "/metadata.json2" << std::endl;
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
                // 保存块级别的元数据（与传统压缩块格式保持一致）
                std::ofstream block_metadata(block_dir + "/metadata.json2", std::ios::binary);
                if (block_metadata.is_open()) {
                    block_metadata.write(reinterpret_cast<const char*>(&block.original_size), sizeof(block.original_size));
                    block_metadata.write(reinterpret_cast<const char*>(&block.placeholder_ratio), sizeof(block.placeholder_ratio));
                    uint32_t config_compression_level = static_cast<uint32_t>(block.block_config.compression_level);
                    block_metadata.write(reinterpret_cast<const char*>(&config_compression_level), sizeof(config_compression_level));
                    uint8_t flags = (block.is_structure_appropriate ? 1 : 0) |
                                   (block.use_layer_separation ? 2 : 0) |
                                   (block.is_granular ? 4 : 0);
                    block_metadata.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
                    block_metadata.close();
                } else {
                    std::cerr << "[ERROR] Cannot create " << block_dir << "/metadata.json2" << std::endl;
                }
            }
        }
        std::cerr << "[SUCCESS] Saved " << blocks_memory_.size() << " type-aware blocks to " << directory_path << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to save type-aware blocks to directory: " << e.what() << std::endl;
        return false;
    }
}

std::vector<ChunkedTypeAwareBlock> ChunkedTypeAwareCompressor::loadFromDirectory(const std::string& directory_path) {
    SelectiveLoadOptions options; // 默认加载所有组件
    // 使用默认配置
    ChunkedTypeAwareCompressor tmp;
    return loadFromDirectorySelective(directory_path, options, tmp.config_.type_aware_config);
}

// 新实现，带config参数
std::vector<ChunkedTypeAwareBlock> ChunkedTypeAwareCompressor::loadFromDirectorySelective(
    const std::string& directory_path,
    const SelectiveLoadOptions& options,
    const compression::TypeAwareCompressionConfig& config) {
    std::vector<ChunkedTypeAwareBlock> blocks;
    
    try {
        // 读取全局元数据
        std::ifstream global_metadata(directory_path + "/metadata.json2", std::ios::binary);
        if (!global_metadata.is_open()) {
            std::cerr << "[ERROR] Cannot open global metadata file" << std::endl;
            return blocks;
        }
        
        uint32_t total_blocks;
        uint64_t total_records, original_size;
        uint32_t compression_level;
        global_metadata.read(reinterpret_cast<char*>(&total_blocks), sizeof(total_blocks));
        global_metadata.read(reinterpret_cast<char*>(&total_records), sizeof(total_records));
        global_metadata.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
        global_metadata.read(reinterpret_cast<char*>(&compression_level), sizeof(compression_level));
        global_metadata.close();
        
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
            test_granular.close();
            
            if (is_granular) {
                // 细粒度类型敏感压缩块的加载
                try {
                    // 1. 读取块元数据
                    std::ifstream block_metadata(block_dir + "/block_metadata.json2", std::ios::binary);
                    if (!block_metadata.is_open()) {
                        std::cerr << "[ERROR] Cannot open block metadata for block " << i << std::endl;
                        continue;
                    }
                    size_t original_size;
                    double placeholder_ratio;
                    uint32_t config_compression_level;
                    uint8_t flags;
                    block_metadata.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
                    block_metadata.read(reinterpret_cast<char*>(&placeholder_ratio), sizeof(placeholder_ratio));
                    block_metadata.read(reinterpret_cast<char*>(&config_compression_level), sizeof(config_compression_level));
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
                    // 细粒度压缩的元数据文件
                    std::ifstream granular_metadata_file(block_dir + "/metadata.json2", std::ios::binary);
                    if (granular_metadata_file.is_open()) {
                        uint32_t metadata_size;
                        granular_metadata_file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
                        gdata.metadata.resize(metadata_size);
                        granular_metadata_file.read(reinterpret_cast<char*>(gdata.metadata.data()), metadata_size);
                        granular_metadata_file.close();
                    }
                    // 读取层数据
                    if (gdata.use_layer_separation) {
                        // 按层分别读取
                        size_t layer_idx = 0;
                        while (true) {
                            char layer_file_buf[256];
                            snprintf(layer_file_buf, sizeof(layer_file_buf), "%s/layer_%zu.json2", block_dir.c_str(), layer_idx);
                            std::ifstream layer_stream(layer_file_buf, std::ios::binary);
                            if (!layer_stream.is_open()) {
                                if (layer_idx == 0) {
                                    std::cerr << "[DEBUG] No layer files found for block " << i << ", expected at least one layer file (layer_0.json2)" << std::endl;
                                } else {
                                    // std::cerr << "[DEBUG] Finished loading " << layer_idx << " layers for block " << i << std::endl;
                                }
                                break;
                            }
                            uint32_t layer_size;
                            if (!layer_stream.read(reinterpret_cast<char*>(&layer_size), sizeof(layer_size))) {
                                std::cerr << "[ERROR] Failed to read layer size for layer " << layer_idx << " in block " << i << std::endl;
                                break;
                            }
                            std::vector<uint8_t> layer_data(layer_size);
                            if (!layer_stream.read(reinterpret_cast<char*>(layer_data.data()), layer_size)) {
                                std::cerr << "[ERROR] Failed to read layer data for layer " << layer_idx << " in block " << i << std::endl;
                                break;
                            }
                            gdata.layer_data_by_level.push_back(std::move(layer_data));
                            layer_stream.close();
                            // std::cerr << "[DEBUG] Loaded layer " << layer_idx << " (" << layer_size << " bytes) for block " << i << std::endl;
                            layer_idx++;
                        }
                        // std::cerr << "[DEBUG] Total layers loaded for block " << i << ": " << gdata.layer_data_by_level.size() << std::endl;
                        if (gdata.layer_data_by_level.empty()) {
                            std::cerr << "[ERROR] No layer data loaded for block " << i << " (layer_data_by_level is empty)" << std::endl;
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
                            std::cerr << "[DEBUG] Loaded combined layers (" << layers_size << " bytes) for block " << i << std::endl;
                        } else {
                            std::cerr << "[ERROR] Cannot open combined layers file for block " << i << std::endl;
                        }
                    }
                    // 调用解压前输出gdata状态
                    // std::cerr << "[DEBUG] Before decompressGranularPartial: block " << i
                    //           << ", use_layer_separation=" << gdata.use_layer_separation
                    //           << ", layer_data_by_level.size()=" << gdata.layer_data_by_level.size()
                    //           << ", layer_data_combined.size()=" << gdata.layer_data_combined.size() << std::endl;
                    // 3. 使用TypeAwareCompressor::decompressGranularPartial解压（类型敏感版本）
                    // 设置解压选项
                    Compressor::PartialDecompressionOptions decompress_options;
                    decompress_options.load_trie = true;
                    decompress_options.load_layers = true;
                    // 使用传入的配置参数
                    try {
                        auto [trie, dict_ptr] = TypeAwareCompressor::decompressGranularPartial(gdata, decompress_options, config);
                        // std::cerr << "[DEBUG] decompressGranularPartial succeeded for block " << i << std::endl;
                        ChunkedTypeAwareBlock block;
                        block.field_order = trie->getOrderedFields();
                        block.dict = std::move(dict_ptr);
                        block.trie = std::move(trie);
                        block.config = config;
                        blocks.push_back(std::move(block));
                    } catch (const std::exception& e) {
                        std::cerr << "[ERROR] decompressGranularPartial threw exception for block " << i << ": " << e.what() << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[ERROR] Failed to load granular type-aware block " << i << ": " << e.what() << std::endl;
                }
                
            } else {
                // 传统类型敏感压缩块的加载
                
                std::ifstream block_file(block_dir + "/data.json2", std::ios::binary);
                if (block_file.is_open()) {
                    uint32_t size;
                    block_file.read(reinterpret_cast<char*>(&size), sizeof(size));
                    
                    std::vector<uint8_t> block_data(size);
                    block_file.read(reinterpret_cast<char*>(block_data.data()), size);
                    block_file.close();
                    
                    // 读取块元数据获取配置
                    compression::TypeAwareCompressionConfig block_config;
                    std::ifstream block_metadata(block_dir + "/metadata.json2", std::ios::binary);
                    if (block_metadata.is_open()) {
                        size_t original_size_meta;
                        double placeholder_ratio;
                        uint32_t config_compression_level;
                        uint8_t flags;
                        
                        block_metadata.read(reinterpret_cast<char*>(&original_size_meta), sizeof(original_size_meta));
                        block_metadata.read(reinterpret_cast<char*>(&placeholder_ratio), sizeof(placeholder_ratio));
                        block_metadata.read(reinterpret_cast<char*>(&config_compression_level), sizeof(config_compression_level));
                        block_metadata.read(reinterpret_cast<char*>(&flags), sizeof(flags));
                        block_metadata.close();
                        
                        block_config.compression_level = static_cast<int>(config_compression_level);
                    }
                    
                    // 反序列化为CompressedData
                    CompressedData cd = Compressor::loadFromMemory(block_data);
                    
                    // 使用类型敏感解压
                    auto [louds, dict_ptr] = TypeAwareCompressor::decompressLouds(cd, block_config);
                    auto trie = std::make_unique<Trie>(louds->getFieldOrder());
                    loudsToTrie(*louds, *trie);
                    
                    ChunkedTypeAwareBlock block;
                    block.field_order = louds->getFieldOrder();
                    block.dict = std::move(dict_ptr);
                    block.trie = std::move(trie);
                    block.config = block_config;
                    
                    blocks.push_back(std::move(block));
                }
            }
        }
        
        std::cerr << "[SUCCESS] Loaded " << blocks.size() << " type-aware blocks from " << directory_path << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load type-aware blocks from directory: " << e.what() << std::endl;
    }
    
    return blocks;
}

} // namespace json2