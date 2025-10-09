#include "bitpacking_compression.h"
#include "../core/compression_utils.h"
#include <stdexcept>
#include <cstring>

namespace json2 {
namespace compression {
namespace algorithms {

// ========== Systematic Helper Functions ==========

// Helper function to write bit-packed header information
static void writeBitPackHeader(std::vector<uint8_t>& output, uint32_t count, uint8_t compression_flags) {
    utils::SerializationUtils::writeValue(output, count);
    utils::SerializationUtils::writeValue(output, compression_flags);
}

// Helper function to read bit-packed header information
std::pair<uint32_t, uint8_t> BitPackingCompression::readBitPackHeader(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos + sizeof(uint32_t) + sizeof(uint8_t) > data.size()) {
        throw std::runtime_error("BitPacking: Invalid header - insufficient data");
    }
    uint32_t count = utils::SerializationUtils::readValue<uint32_t>(data, pos);
    uint8_t flags = utils::SerializationUtils::readValue<uint8_t>(data, pos);
    return {count, flags};
}

// Helper function to validate compression effectiveness
static bool validateCompressionEffectiveness(size_t original_size, size_t compressed_size, double threshold = 0.8) {
    if (original_size == 0) return true;
    double ratio = static_cast<double>(compressed_size) / original_size;
    return ratio < threshold;
}

// Helper function to analyze data and determine optimal compression strategy
static uint8_t analyzeAndGetCompressionFlags(const std::vector<bool>& values) {
    uint8_t flags = 0;
    
    // Check sparsity
    size_t true_count = 0;
    for (bool val : values) {
        if (val) true_count++;
    }
    
    double true_ratio = static_cast<double>(true_count) / values.size();
    double sparsity_ratio = std::min(true_ratio, 1.0 - true_ratio);
    
    if (sparsity_ratio <= 0.1 && values.size() >= 64) {
        flags |= 0x01; // Use sparse encoding
    }
    
    if (values.size() >= 1024) {
        flags |= 0x02; // Large dataset flag
    }
    
    return flags;
}

// ========== Enhanced Core Compression Methods ==========

std::vector<uint8_t> BitPackingCompression::compressBool(const std::vector<bool>& values) {
    if (values.empty()) return {};
    
    try {
        // Analyze data to determine optimal strategy
        uint8_t flags = analyzeAndGetCompressionFlags(values);
        
        std::vector<uint8_t> compressed;
        writeBitPackHeader(compressed, static_cast<uint32_t>(values.size()), flags);
        
        std::vector<uint8_t> data;
        if (flags & 0x01) { // Use sparse encoding
            data = compressSparseBitmap(values);
        } else { // Use regular bit packing
            data = bitPackingCompress(values);
        }
        
        // Validate compression effectiveness
        size_t original_size = (values.size() + 7) / 8; // Approximate original bit-packed size
        if (!validateCompressionEffectiveness(original_size, data.size())) {
            // If compression isn't effective, fall back to simple bit packing
            compressed.clear();
            writeBitPackHeader(compressed, static_cast<uint32_t>(values.size()), 0);
            data = bitPackingCompress(values);
        }
        
        compressed.insert(compressed.end(), data.begin(), data.end());
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("BitPacking compression failed: " + std::string(e.what()));
    }
}

std::vector<bool> BitPackingCompression::decompressBool(const std::vector<uint8_t>& compressed, size_t count) {
    if (compressed.empty()) return {};
    
    try {
        size_t pos = 0;
        auto [actual_count, flags] = readBitPackHeader(compressed, pos);
        
        // Validate count consistency
        if (count != 0 && count != actual_count) {
            throw std::runtime_error("BitPacking: Count mismatch in decompression");
        }
        
        // Extract data portion
        std::vector<uint8_t> data(compressed.begin() + pos, compressed.end());
        
        if (flags & 0x01) { // Sparse encoding used
            return decompressSparseBitmap(data, actual_count);
        } else { // Regular bit packing
            return bitPackingDecompress(data, actual_count);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("BitPacking decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
bool BitPackingCompression::decompressBoolAt(const std::vector<uint8_t>& compressed, size_t index) {
    if (compressed.empty()) {
        throw std::runtime_error("BitPacking: Empty compressed data");
    }
    
    try {
        size_t pos = 0;
        auto [actual_count, flags] = readBitPackHeader(compressed, pos);
        
        // Validate index
        if (index >= actual_count) {
            throw std::out_of_range("BitPacking: Index out of range");
        }
        
        // Extract data portion
        std::vector<uint8_t> data(compressed.begin() + pos, compressed.end());
        
        if (flags & 0x01) { // Sparse encoding used
            return decompressSparseBitmapAt(data, index);
        } else { // Regular bit packing
            return bitPackingDecompressAt(data, index, actual_count);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("BitPacking partial decompression failed: " + std::string(e.what()));
    }
}

// ========== Core Bit Packing Implementation with Enhanced Error Handling ==========

std::vector<uint8_t> BitPackingCompression::bitPackingCompress(const std::vector<bool>& values) {
    if (values.empty()) return {};
    
    std::vector<uint8_t> compressed;
    compressed.reserve((values.size() + 7) / 8); // Pre-allocate optimal size
    
    uint8_t current_byte = 0;
    int bit_pos = 0;
    
    for (bool value : values) {
        if (value) {
            current_byte |= (1 << bit_pos);
        }
        bit_pos++;
        
        if (bit_pos == 8) {
            compressed.push_back(current_byte);
            current_byte = 0;
            bit_pos = 0;
        }
    }
    
    // Handle the last incomplete byte
    if (bit_pos > 0) {
        compressed.push_back(current_byte);
    }
    
    return compressed;
}

std::vector<bool> BitPackingCompression::bitPackingDecompress(const std::vector<uint8_t>& compressed, size_t count) {
    if (compressed.empty() && count > 0) {
        throw std::runtime_error("BitPacking: Empty compressed data but non-zero count expected");
    }
    
    std::vector<bool> values;
    values.reserve(count);
    
    for (size_t i = 0; i < compressed.size() && values.size() < count; ++i) {
        uint8_t byte = compressed[i];
        for (int bit = 0; bit < 8 && values.size() < count; ++bit) {
            values.push_back((byte & (1 << bit)) != 0);
        }
    }
    
    // Validate result size
    if (values.size() != count) {
        throw std::runtime_error("BitPacking: Decompressed size mismatch");
    }
    
    return values;
}

// 部分解压指定索引的值
bool BitPackingCompression::bitPackingDecompressAt(const std::vector<uint8_t>& compressed, size_t index, size_t count) {
    if (compressed.empty() && count > 0) {
        throw std::runtime_error("BitPacking: Empty compressed data but non-zero count expected");
    }
    
    // 计算目标字节和位的位置
    size_t byte_index = index / 8;
    size_t bit_index = index % 8;
    
    // 检查字节索引是否在范围内
    if (byte_index >= compressed.size()) {
        throw std::out_of_range("BitPacking: Index out of range");
    }
    
    // 提取目标位的值
    uint8_t byte = compressed[byte_index];
    return (byte & (1 << bit_index)) != 0;
}

// ========== Enhanced State Compression with Validation ==========

std::vector<uint8_t> BitPackingCompression::compressBoolStates(const std::vector<uint8_t>& states) {
    if (states.empty()) return {};
    
    // Validate state values
    for (size_t i = 0; i < states.size(); ++i) {
        if (states[i] > 2) {
            throw std::runtime_error("BitPacking: Invalid state value at index " + std::to_string(i) + 
                                   ": " + std::to_string(states[i]) + " (expected 0-2)");
        }
    }
    
    // Use 2-bit encoding: 00=false, 01=true, 10=null, 11=reserved
    std::vector<uint8_t> compressed;
    compressed.reserve((states.size() * 2 + 7) / 8); // Pre-allocate
    
    uint8_t current_byte = 0;
    int bit_pos = 0;
    
    for (uint8_t state : states) {
        // Limit state values to 0-2 for safety
        uint8_t encoded_state = (state > 2) ? 2 : state;
        
        current_byte |= (encoded_state << bit_pos);
        bit_pos += 2;
        
        if (bit_pos >= 8) {
            compressed.push_back(current_byte);
            current_byte = 0;
            bit_pos = 0;
        }
    }
    
    // Handle the last incomplete byte
    if (bit_pos > 0) {
        compressed.push_back(current_byte);
    }
    
    return compressed;
}

std::vector<uint8_t> BitPackingCompression::decompressBoolStates(const std::vector<uint8_t>& compressed, size_t count) {
    if (compressed.empty() && count > 0) {
        throw std::runtime_error("BitPacking: Empty compressed data but non-zero state count expected");
    }
    
    std::vector<uint8_t> states;
    states.reserve(count);
    
    for (size_t i = 0; i < compressed.size() && states.size() < count; ++i) {
        uint8_t byte = compressed[i];
        for (int bit_pair = 0; bit_pair < 8 && states.size() < count; bit_pair += 2) {
            uint8_t state = (byte >> bit_pair) & 0x03; // Extract 2 bits
            states.push_back(state);
        }
    }
    
    // Validate result size
    if (states.size() != count) {
        throw std::runtime_error("BitPacking: Decompressed state count mismatch");
    }
    
    return states;
}

// ========== Enhanced Sparse Bitmap Compression ==========

std::vector<uint8_t> BitPackingCompression::compressSparseBitmap(const std::vector<bool>& values) {
    if (values.empty()) return {};
    
    // For sparse bitmaps, use index encoding: store indices of true bits
    std::vector<uint32_t> true_indices;
    true_indices.reserve(values.size() / 10); // Assume sparse data
    
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i]) {
            if (i > UINT32_MAX) {
                throw std::runtime_error("BitPacking: Index too large for sparse encoding");
            }
            true_indices.push_back(static_cast<uint32_t>(i));
        }
    }
    
    // Serialize: total_length + true_count + index_list
    std::vector<uint8_t> compressed;
    utils::SerializationUtils::writeValue(compressed, static_cast<uint32_t>(values.size()));
    utils::SerializationUtils::writeValue(compressed, static_cast<uint32_t>(true_indices.size()));
    
    for (uint32_t index : true_indices) {
        utils::SerializationUtils::writeValue(compressed, index);
    }
    
    return compressed;
}

std::vector<bool> BitPackingCompression::decompressSparseBitmap(const std::vector<uint8_t>& compressed, size_t count) {
    if (compressed.size() < 8) {
        throw std::runtime_error("BitPacking: Invalid sparse bitmap data - insufficient header");
    }
    
    size_t pos = 0;
    uint32_t total_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    uint32_t true_count = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    
    // Validate count consistency if provided
    if (count != 0 && count != total_size) {
        throw std::runtime_error("BitPacking: Size mismatch in sparse decompression");
    }
    
    // Validate data size
    size_t expected_data_size = 8 + true_count * sizeof(uint32_t);
    if (compressed.size() < expected_data_size) {
        throw std::runtime_error("BitPacking: Insufficient data for sparse bitmap indices");
    }
    
    std::vector<bool> values(total_size, false);
    
    for (uint32_t i = 0; i < true_count && pos + sizeof(uint32_t) <= compressed.size(); ++i) {
        uint32_t index = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
        if (index >= total_size) {
            throw std::runtime_error("BitPacking: Invalid index in sparse bitmap: " + std::to_string(index));
        }
        values[index] = true;
    }
    
    return values;
}

// 部分解压稀疏位图指定索引的值
bool BitPackingCompression::decompressSparseBitmapAt(const std::vector<uint8_t>& compressed, size_t index) {
    if (compressed.size() < 8) {
        throw std::runtime_error("BitPacking: Invalid sparse bitmap data - insufficient header");
    }
    
    size_t pos = 0;
    uint32_t total_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    uint32_t true_count = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    
    // Validate index
    if (index >= total_size) {
        throw std::out_of_range("BitPacking: Index out of range");
    }
    
    // Validate data size
    size_t expected_data_size = 8 + true_count * sizeof(uint32_t);
    if (compressed.size() < expected_data_size) {
        throw std::runtime_error("BitPacking: Insufficient data for sparse bitmap indices");
    }
    
    // 查找索引是否在true_indices中
    for (uint32_t i = 0; i < true_count && pos + sizeof(uint32_t) <= compressed.size(); ++i) {
        uint32_t stored_index = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
        if (stored_index == index) {
            return true; // 找到匹配的索引，返回true
        }
        if (stored_index > index) {
            break; // 由于索引是有序的，如果存储的索引大于目标索引，则后面不可能有匹配项
        }
    }
    
    return false; // 未找到匹配的索引，返回false
}

// ========== Enhanced Analysis and Validation Functions ==========

double BitPackingCompression::estimateCompressionRatio(const std::vector<bool>& values) {
    if (values.empty()) return 1.0;
    
    try {
        BitPackingStats stats = analyzeData(values);
        return stats.estimated_ratio;
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

bool BitPackingCompression::isSparse(const std::vector<bool>& values, double threshold) {
    if (values.empty()) return false;
    
    try {
        return utils::DataAnalysisUtils::isSparse(values, threshold);
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
}

BitPackingCompression::BitPackingStats BitPackingCompression::analyzeData(const std::vector<bool>& values) {
    BitPackingStats stats = {};
    
    if (values.empty()) {
        stats.is_worth_compressing = false;
        return stats;
    }
    
    try {
        // Calculate true/false counts with bounds checking
        for (bool val : values) {
            if (val) {
                stats.true_count++;
            } else {
                stats.false_count++;
            }
        }
        
        // Calculate sparsity ratio with validation
        if (values.size() > 0) {
            double true_ratio = static_cast<double>(stats.true_count) / values.size();
            stats.sparsity_ratio = std::min(true_ratio, 1.0 - true_ratio);
        }
        
        // Estimate compression ratio with validation
        if (shouldUseSparseEncoding(values)) {
            // Sparse encoding: 8-byte header + 4 bytes per index
            size_t sparse_size = 8 + stats.true_count * 4;
            stats.estimated_ratio = static_cast<double>(sparse_size) / values.size();
            stats.recommend_sparse_encoding = true;
        } else {
            // Regular bit packing
            size_t packed_size = calculateCompressedSize(values.size());
            stats.estimated_ratio = static_cast<double>(packed_size) / values.size();
            stats.recommend_sparse_encoding = false;
        }
        
        // Conservative threshold for effectiveness
        stats.is_worth_compressing = stats.estimated_ratio < 0.8;
        
    } catch (const std::exception&) {
        stats.is_worth_compressing = false;
        stats.estimated_ratio = 1.0;
    }
    
    return stats;
}

size_t BitPackingCompression::calculateCompressedSize(size_t bool_count) {
    if (bool_count == 0) return 0;
    return (bool_count + 7) / 8; // Round up to byte boundary
}

bool BitPackingCompression::shouldUseSparseEncoding(const std::vector<bool>& values, double threshold) {
    if (values.size() < 64) return false; // Small data not suitable for sparse encoding
    
    try {
        double sparsity = utils::DataAnalysisUtils::calculateTrueFalseRatio(values);
        sparsity = std::min(sparsity, 1.0 - sparsity); // Take smaller value
        
        return sparsity <= threshold;
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
}

// ========== ICompression Interface Implementation ==========

std::vector<uint8_t> BitPackingCompression::compress(const std::vector<uint8_t>& data, int level) {
    // Convert bytes to bool values and compress using bit packing
    if (data.empty()) return {};
    
    try {
        // Convert byte data to bool array for bit packing compression
        std::vector<bool> values;
        values.reserve(data.size() * 8);
        
        for (uint8_t byte : data) {
            for (int bit = 0; bit < 8; ++bit) {
                values.push_back((byte & (1 << bit)) != 0);
            }
        }
        
        return compressBool(values);
    } catch (const std::exception& e) {
        throw std::runtime_error("BitPacking compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> BitPackingCompression::decompress(const std::vector<uint8_t>& compressed_data) {
    // Decompress bit-packed data and convert back to bytes
    if (compressed_data.empty()) return {};
    
    try {
        std::vector<bool> values = decompressBool(compressed_data, 0); // Let header determine count
        
        // Convert bool array back to bytes
        std::vector<uint8_t> result;
        result.reserve((values.size() + 7) / 8);
        
        for (size_t i = 0; i < values.size(); i += 8) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8 && i + bit < values.size(); ++bit) {
                if (values[i + bit]) {
                    byte |= (1 << bit);
                }
            }
            result.push_back(byte);
        }
        
        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error("BitPacking decompression failed: " + std::string(e.what()));
    }
}

std::string BitPackingCompression::getName() const {
    return "BitPacking";
}

bool BitPackingCompression::supportsLevel(int level) const {
    return (level >= 0 && level <= 9);  // BitPacking doesn't use levels, but accept reasonable range
}

} // namespace algorithms
} // namespace compression
} // namespace json2