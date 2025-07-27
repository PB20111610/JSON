#include "../include/chunked_compressor.h"
#include <simdjson.h>
#include "../include/compress.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <set>
#include <functional>
#include "../include/parser.h"

namespace json2 {

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



void ChunkedTrieCompressor::finalizeCurrentBlock() {
    if (current_block_count_ > 0 && !block_buffer_.empty()) {
        FieldDictionaryManager dict;
        std::vector<FieldKey> ordered_fields;
        // 字段统计只用chunk_stat_buffer_
        JsonParser::analyzeAndSortFields(chunk_stat_buffer_, dict, ordered_fields);
        std::cerr << "[DEBUG] Finalizing block. Record count: " << current_block_count_ << ", Field count: " << ordered_fields.size() << std::endl;
        Trie trie(ordered_fields);
        simdjson::dom::parser parser;
        for (const auto& rec : block_buffer_) {
            trie.insert(rec, dict, parser);
        }
        LOUDSTrie louds(ordered_fields);
        louds.buildFromTrie(trie);
        auto compressed = Compressor::compressLouds(louds, dict, ordered_fields);
        // 直接序列化为内存块
        ChunkedBlockMemory block_mem;
        block_mem.compressed_data = Compressor::saveToMemory(compressed);
        block_mem.original_size = compressed.original_size;
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
    for (const auto& block : blocks_memory_) {
        total_compressed += block.compressed_data.size();
        total_core_data += block.original_size;
        stats.block_sizes.push_back(block.compressed_data.size());
    }
    stats.total_compressed_size = total_compressed;
    stats.total_core_data_size = total_core_data;
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