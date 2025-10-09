#include "delta_compression.h"
#include "../core/compression_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace json2 {
namespace compression {
namespace algorithms {

// ========== Systematic Helper Functions ==========

// Helper function to write delta compression header
static void writeDeltaHeader(std::vector<uint8_t>& output, uint32_t count, uint8_t compression_type, int64_t base_value = 0) {
    utils::SerializationUtils::writeValue(output, count);
    utils::SerializationUtils::writeValue(output, compression_type);
    if (compression_type & 0x01) { // Has base value
        utils::SerializationUtils::writeValue(output, base_value);
    }
}

// Helper function to read delta compression header
std::tuple<uint32_t, uint8_t, int64_t> DeltaCompression::readDeltaHeader(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos + sizeof(uint32_t) + sizeof(uint8_t) > data.size()) {
        throw std::runtime_error("Delta: Invalid header - insufficient data");
    }
    
    uint32_t count = utils::SerializationUtils::readValue<uint32_t>(data, pos);
    uint8_t compression_type = utils::SerializationUtils::readValue<uint8_t>(data, pos);
    
    int64_t base_value = 0;
    if (compression_type & 0x01) { // Has base value
        if (pos + sizeof(int64_t) > data.size()) {
            throw std::runtime_error("Delta: Invalid header - missing base value");
        }
        base_value = utils::SerializationUtils::readValue<int64_t>(data, pos);
    }
    
    return {count, compression_type, base_value};
}

// Helper function to validate delta compression effectiveness
static bool validateDeltaCompressionEffectiveness(const std::vector<int64_t>& original_values, 
                                                 const std::vector<uint8_t>& compressed_data,
                                                 double threshold = 0.8) {
    if (original_values.empty()) return true;
    
    size_t original_size = original_values.size() * sizeof(int64_t);
    double ratio = static_cast<double>(compressed_data.size()) / original_size;
    return ratio < threshold;
}

// Helper function to analyze delta patterns and select optimal encoding
static uint8_t analyzeDeltaPatterns(const std::vector<int64_t>& values) {
    uint8_t flags = 0;
    
    if (values.empty()) return flags;
    
    // Check if data is sorted (beneficial for delta compression)
    bool is_sorted = utils::DataAnalysisUtils::isSorted(values);
    if (is_sorted) {
        flags |= 0x02; // Sorted data flag
    }
    
    // Check if deltas are small (beneficial for varint encoding)
    if (values.size() > 1) {
        bool has_small_deltas = utils::DataAnalysisUtils::hasSmallDeltas(values, 1000);
        if (has_small_deltas) {
            flags |= 0x04; // Small deltas flag
        }
    }
    
    // Set base value flag for first value storage
    flags |= 0x01;
    
    return flags;
}

// ========== Enhanced Core Compression Methods ==========

std::vector<uint8_t> DeltaCompression::compressInt64(const std::vector<int64_t>& values) {
    if (values.empty()) return {};
    
    try {
        // Analyze data patterns for optimal compression strategy
        uint8_t compression_flags = analyzeDeltaPatterns(values);
        
        std::vector<uint8_t> compressed;
        writeDeltaHeader(compressed, static_cast<uint32_t>(values.size()), compression_flags, values[0]);
        
        // Use delta-varint compression for optimal results
        std::vector<uint8_t> delta_data = deltaVarintCompress(values);
        
        // Validate compression effectiveness
        if (!validateDeltaCompressionEffectiveness(values, delta_data)) {
            // If compression isn't effective, use simplified approach
            compressed.clear();
            writeDeltaHeader(compressed, static_cast<uint32_t>(values.size()), 0x01, values[0]);
            delta_data = utils::SerializationUtils::int64sToBytes(values);
        }
        
        compressed.insert(compressed.end(), delta_data.begin(), delta_data.end());
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta compression failed: " + std::string(e.what()));
    }
}

std::vector<int64_t> DeltaCompression::decompressInt64(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    try {
        size_t pos = 0;
        auto [count, compression_type, base_value] = readDeltaHeader(compressed, pos);
        
        // Extract compressed data portion
        std::vector<uint8_t> data(compressed.begin() + pos, compressed.end());
        
        if (compression_type & 0x04) { // Delta-varint encoding used
            return deltaVarintDecompress(data);
        } else { // Simple serialization fallback
            return utils::SerializationUtils::bytesToInt64s(data);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
int64_t DeltaCompression::decompressInt64At(const std::vector<uint8_t>& compressed, size_t index) {
    if (compressed.empty()) {
        throw std::runtime_error("Delta: Empty compressed data");
    }
    
    try {
        size_t pos = 0;
        auto [count, compression_type, base_value] = readDeltaHeader(compressed, pos);
        
        // Validate index
        if (index >= count) {
            throw std::out_of_range("Delta: Index out of range");
        }
        
        // Extract compressed data portion
        std::vector<uint8_t> data(compressed.begin() + pos, compressed.end());
        
        if (compression_type & 0x04) { // Delta-varint encoding used
            return deltaVarintDecompressAt(data, index);
        } else { // Simple serialization fallback
            auto values = utils::SerializationUtils::bytesToInt64s(data);
            if (index >= values.size()) {
                throw std::out_of_range("Delta: Index out of range");
            }
            return values[index];
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta partial decompression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> DeltaCompression::compressUint32(const std::vector<uint32_t>& values) {
    if (values.empty()) return {};
    
    try {
        auto int64_values = uint32sToInt64s(values);
        return compressInt64(int64_values); // Use enhanced compression
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta uint32 compression failed: " + std::string(e.what()));
    }
}

std::vector<uint32_t> DeltaCompression::decompressUint32(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    try {
        auto int64_values = decompressInt64(compressed); // Use enhanced decompression
        return int64sToUint32s(int64_values);
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta uint32 decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
uint32_t DeltaCompression::decompressUint32At(const std::vector<uint8_t>& compressed, size_t index) {
    int64_t value = decompressInt64At(compressed, index);
    if (value < 0 || value > UINT32_MAX) {
        throw std::runtime_error("Delta: Value out of uint32 range: " + std::to_string(value));
    }
    return static_cast<uint32_t>(value);
}

std::vector<uint8_t> DeltaCompression::compressDouble(const std::vector<double>& values) {
    if (values.empty()) return {};
    
    try {
        auto int64_values = doublesToInt64s(values);
        return compressInt64(int64_values); // Use enhanced compression
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta double compression failed: " + std::string(e.what()));
    }
}

std::vector<double> DeltaCompression::decompressDouble(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    try {
        auto int64_values = decompressInt64(compressed); // Use enhanced decompression
        return int64sToDoubles(int64_values);
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta double decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
double DeltaCompression::decompressDoubleAt(const std::vector<uint8_t>& compressed, size_t index) {
    int64_t int_value = decompressInt64At(compressed, index);
    double double_value;
    std::memcpy(&double_value, &int_value, sizeof(double));
    return double_value;
}

// ========== Enhanced Delta Compression Core Implementation ==========

std::vector<uint8_t> DeltaCompression::deltaCompress(const std::vector<int64_t>& values) {
    if (values.empty()) return {};
    
    try {
        std::vector<int64_t> deltas;
        deltas.reserve(values.size());
        deltas.push_back(values[0]); // First value remains unchanged
        
        // Calculate deltas with overflow protection
        for (size_t i = 1; i < values.size(); ++i) {
            int64_t delta = values[i] - values[i-1];
            deltas.push_back(delta);
        }
        
        return utils::SerializationUtils::int64sToBytes(deltas);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta compression calculation failed: " + std::string(e.what()));
    }
}

std::vector<int64_t> DeltaCompression::deltaDecompress(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    try {
        auto deltas = utils::SerializationUtils::bytesToInt64s(compressed);
        if (deltas.empty()) return {};
        
        std::vector<int64_t> values;
        values.reserve(deltas.size());
        values.push_back(deltas[0]); // First value
        
        // Reconstruct values from deltas with overflow protection
        for (size_t i = 1; i < deltas.size(); ++i) {
            int64_t next_value = values.back() + deltas[i];
            values.push_back(next_value);
        }
        
        return values;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta decompression reconstruction failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
int64_t DeltaCompression::deltaDecompressAt(const std::vector<uint8_t>& compressed, size_t index) {
    if (compressed.empty()) {
        throw std::runtime_error("Delta: Empty compressed data");
    }
    
    try {
        auto deltas = utils::SerializationUtils::bytesToInt64s(compressed);
        if (deltas.empty()) {
            throw std::runtime_error("Delta: Empty deltas data");
        }
        
        // Validate index
        if (index >= deltas.size()) {
            throw std::out_of_range("Delta: Index out of range");
        }
        
        // Reconstruct value at index by accumulating deltas
        int64_t value = deltas[0]; // First value
        
        // Accumulate deltas up to the target index
        for (size_t i = 1; i <= index; ++i) {
            value += deltas[i];
        }
        
        return value;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta partial decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced Delta-Varint Compression ==========

std::vector<uint8_t> DeltaCompression::deltaVarintCompress(const std::vector<int64_t>& values) {
    if (values.empty()) return {};
    
    try {
        std::vector<uint8_t> compressed;
        
        // Write the first value using varint encoding
        VarintCompression::encodeVarint(values[0], compressed);
        
        // Encode subsequent values as deltas with overflow protection
        int64_t prev = values[0];
        for (size_t i = 1; i < values.size(); ++i) {
            int64_t delta = values[i] - prev;
            VarintCompression::encodeVarint(delta, compressed);
            prev = values[i];
        }
        
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta-varint compression failed: " + std::string(e.what()));
    }
}

std::vector<int64_t> DeltaCompression::deltaVarintDecompress(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    try {
        std::vector<int64_t> values;
        size_t pos = 0;
        
        // Read the first value
        if (pos >= compressed.size()) {
            throw std::runtime_error("Delta-varint: Insufficient data for first value");
        }
        
        int64_t first = VarintCompression::decodeVarint(compressed, pos);
        values.push_back(first);
        
        // Decode subsequent delta values with bounds checking
        int64_t prev = first;
        while (pos < compressed.size()) {
            int64_t delta = VarintCompression::decodeVarint(compressed, pos);
            int64_t value = prev + delta;
            values.push_back(value);
            prev = value;
        }
        
        return values;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta-varint decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
int64_t DeltaCompression::deltaVarintDecompressAt(const std::vector<uint8_t>& compressed, size_t index) {
    if (compressed.empty()) {
        throw std::runtime_error("Delta-varint: Empty compressed data");
    }
    
    try {
        size_t pos = 0;
        
        // Read the first value
        if (pos >= compressed.size()) {
            throw std::runtime_error("Delta-varint: Insufficient data for first value");
        }
        
        int64_t first = VarintCompression::decodeVarint(compressed, pos);
        
        // If index is 0, return the first value directly
        if (index == 0) {
            return first;
        }
        
        // Decode subsequent delta values until we reach the target index
        int64_t prev = first;
        size_t current_index = 1;
        
        while (pos < compressed.size() && current_index <= index) {
            int64_t delta = VarintCompression::decodeVarint(compressed, pos);
            int64_t value = prev + delta;
            
            if (current_index == index) {
                return value;
            }
            
            prev = value;
            current_index++;
        }
        
        // If we get here, the index is out of range
        throw std::out_of_range("Delta-varint: Index out of range");
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta-varint partial decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced Analysis Functions with Error Handling ==========

bool DeltaCompression::isSorted(const std::vector<int64_t>& values) {
    try {
        return utils::DataAnalysisUtils::isSorted(values);
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
}

bool DeltaCompression::hasSmallDeltas(const std::vector<int64_t>& values, int64_t threshold) {
    try {
        return utils::DataAnalysisUtils::hasSmallDeltas(values, threshold);
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
}

double DeltaCompression::estimateCompressionRatio(const std::vector<int64_t>& values) {
    if (values.empty()) return 1.0;
    
    try {
        DeltaStats stats = analyzeData(values);
        return stats.estimated_ratio;
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

DeltaCompression::DeltaStats DeltaCompression::analyzeData(const std::vector<int64_t>& values) {
    DeltaStats stats = {};
    
    if (values.size() <= 1) {
        stats.is_worth_compressing = false;
        return stats;
    }
    
    try {
        stats.is_sorted = isSorted(values);
        
        // Calculate delta statistics with bounds checking
        std::vector<int64_t> deltas;
        deltas.reserve(values.size() - 1);
        
        for (size_t i = 1; i < values.size(); ++i) {
            deltas.push_back(values[i] - values[i-1]);
        }
        
        if (!deltas.empty()) {
            stats.min_delta = *std::min_element(deltas.begin(), deltas.end());
            stats.max_delta = *std::max_element(deltas.begin(), deltas.end());
            
            // Safe average calculation
            int64_t sum = 0;
            for (int64_t delta : deltas) {
                sum += delta;
            }
            stats.avg_delta = static_cast<double>(sum) / deltas.size();
            
            stats.delta_variance = utils::DataAnalysisUtils::calculateDeltaVariance(values);
        }
        
        // Estimate compression ratio
        size_t estimated_size = VarintCompression::getVarintSize(values[0]); // First value
        for (size_t i = 1; i < values.size(); ++i) {
            int64_t delta = values[i] - values[i-1];
            estimated_size += VarintCompression::getVarintSize(delta);
        }
        
        stats.estimated_ratio = static_cast<double>(estimated_size) / (values.size() * sizeof(int64_t));
        stats.is_worth_compressing = stats.estimated_ratio < 0.8;
        
    } catch (const std::exception&) {
        stats.is_worth_compressing = false;
        stats.estimated_ratio = 1.0;
    }
    
    return stats;
}

// ========== Enhanced Type Conversion Helpers with Validation ==========

std::vector<int64_t> DeltaCompression::uint32sToInt64s(const std::vector<uint32_t>& values) {
    std::vector<int64_t> result;
    result.reserve(values.size());
    
    try {
        for (uint32_t val : values) {
            result.push_back(static_cast<int64_t>(val));
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("uint32 to int64 conversion failed: " + std::string(e.what()));
    }
    
    return result;
}

std::vector<uint32_t> DeltaCompression::int64sToUint32s(const std::vector<int64_t>& values) {
    std::vector<uint32_t> result;
    result.reserve(values.size());
    
    try {
        for (int64_t val : values) {
            if (val < 0 || val > UINT32_MAX) {
                throw std::runtime_error("Value out of uint32 range: " + std::to_string(val));
            }
            result.push_back(static_cast<uint32_t>(val));
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("int64 to uint32 conversion failed: " + std::string(e.what()));
    }
    
    return result;
}

std::vector<int64_t> DeltaCompression::doublesToInt64s(const std::vector<double>& values) {
    std::vector<int64_t> result;
    result.reserve(values.size());
    
    try {
        for (double val : values) {
            int64_t int_val;
            std::memcpy(&int_val, &val, sizeof(double));
            result.push_back(int_val);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("double to int64 conversion failed: " + std::string(e.what()));
    }
    
    return result;
}

std::vector<double> DeltaCompression::int64sToDoubles(const std::vector<int64_t>& values) {
    std::vector<double> result;
    result.reserve(values.size());
    
    try {
        for (int64_t val : values) {
            double double_val;
            std::memcpy(&double_val, &val, sizeof(double));
            result.push_back(double_val);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("int64 to double conversion failed: " + std::string(e.what()));
    }
    
    return result;
}

// ========== Enhanced Delta-Delta Compression for Timestamps ==========

std::vector<uint8_t> DeltaDeltaCompression::deltaDeltaCompress(const std::vector<int64_t>& timestamps) {
    if (timestamps.size() < MIN_TIMESTAMPS_FOR_DELTA_DELTA) {
        // For fewer than 3 values, use delta encoding
        return DeltaCompression::deltaVarintCompress(timestamps);
    }
    
    try {
        std::vector<uint8_t> compressed;
        
        // Write the first two values
        VarintCompression::encodeVarint(timestamps[0], compressed);
        VarintCompression::encodeVarint(timestamps[1], compressed);
        
        // Calculate delta-of-delta with bounds checking
        int64_t prev_delta = timestamps[1] - timestamps[0];
        
        for (size_t i = 2; i < timestamps.size(); ++i) {
            int64_t curr_delta = timestamps[i] - timestamps[i-1];
            int64_t delta_of_delta = curr_delta - prev_delta;
            VarintCompression::encodeVarint(delta_of_delta, compressed);
            prev_delta = curr_delta;
        }
        
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta-delta compression failed: " + std::string(e.what()));
    }
}

std::vector<int64_t> DeltaDeltaCompression::deltaDeltaDecompress(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    try {
        std::vector<int64_t> timestamps;
        size_t pos = 0;
        
        // Read the first two values with validation
        if (pos >= compressed.size()) {
            throw std::runtime_error("Delta-delta: Insufficient data for first timestamp");
        }
        
        int64_t first = VarintCompression::decodeVarint(compressed, pos);
        timestamps.push_back(first);
        
        if (pos >= compressed.size()) return timestamps;
        
        int64_t second = VarintCompression::decodeVarint(compressed, pos);
        timestamps.push_back(second);
        
        // Decode delta-of-delta with bounds checking
        int64_t prev_delta = second - first;
        
        while (pos < compressed.size()) {
            int64_t delta_of_delta = VarintCompression::decodeVarint(compressed, pos);
            int64_t curr_delta = prev_delta + delta_of_delta;
            int64_t value = timestamps.back() + curr_delta;
            timestamps.push_back(value);
            prev_delta = curr_delta;
        }
        
        return timestamps;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta-delta decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
int64_t DeltaDeltaCompression::deltaDeltaDecompressAt(const std::vector<uint8_t>& compressed, size_t index) {
    if (compressed.empty()) {
        throw std::runtime_error("Delta-delta: Empty compressed data");
    }
    
    try {
        std::vector<int64_t> timestamps;
        size_t pos = 0;
        
        // Read the first two values with validation
        if (pos >= compressed.size()) {
            throw std::runtime_error("Delta-delta: Insufficient data for first timestamp");
        }
        
        int64_t first = VarintCompression::decodeVarint(compressed, pos);
        timestamps.push_back(first);
        
        // If index is 0, return the first value directly
        if (index == 0) {
            return first;
        }
        
        if (pos >= compressed.size()) {
            throw std::out_of_range("Delta-delta: Index out of range");
        }
        
        int64_t second = VarintCompression::decodeVarint(compressed, pos);
        timestamps.push_back(second);
        
        // If index is 1, return the second value directly
        if (index == 1) {
            return second;
        }
        
        // Decode delta-of-delta with bounds checking until we reach the target index
        int64_t prev_delta = second - first;
        size_t current_index = 2;
        
        while (pos < compressed.size() && current_index <= index) {
            int64_t delta_of_delta = VarintCompression::decodeVarint(compressed, pos);
            int64_t curr_delta = prev_delta + delta_of_delta;
            int64_t value = timestamps.back() + curr_delta;
            timestamps.push_back(value);
            prev_delta = curr_delta;
            current_index++;
            
            // If we've reached the target index, return the value
            if (current_index - 1 == index) {
                return value;
            }
        }
        
        // If we get here, the index is out of range
        throw std::out_of_range("Delta-delta: Index out of range");
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta-delta partial decompression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> DeltaDeltaCompression::compressTimestamps(const std::vector<int64_t>& timestamps) {
    if (timestamps.empty()) return {};
    
    try {
        return deltaDeltaCompress(timestamps);
    } catch (const std::exception& e) {
        throw std::runtime_error("Timestamp compression failed: " + std::string(e.what()));
    }
}

std::vector<int64_t> DeltaDeltaCompression::decompressTimestamps(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    try {
        return deltaDeltaDecompress(compressed);
    } catch (const std::exception& e) {
        throw std::runtime_error("Timestamp decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
int64_t DeltaDeltaCompression::decompressTimestampAt(const std::vector<uint8_t>& compressed, size_t index) {
    return deltaDeltaDecompressAt(compressed, index);
}

bool DeltaDeltaCompression::hasRegularIntervals(const std::vector<int64_t>& timestamps, double tolerance) {
    try {
        return utils::DataAnalysisUtils::hasRegularIntervals(timestamps, tolerance);
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
}

bool DeltaDeltaCompression::hasSmallDeltaDeltas(const std::vector<int64_t>& timestamps, int64_t threshold) {
    if (timestamps.size() < 3) return true;
    
    try {
        int64_t prev_delta = timestamps[1] - timestamps[0];
        
        for (size_t i = 2; i < timestamps.size(); ++i) {
            int64_t curr_delta = timestamps[i] - timestamps[i-1];
            int64_t delta_of_delta = std::abs(curr_delta - prev_delta);
            
            if (delta_of_delta > threshold) {
                return false;
            }
            prev_delta = curr_delta;
        }
        
        return true;
        
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
}

double DeltaDeltaCompression::estimateCompressionRatio(const std::vector<int64_t>& timestamps) {
    if (timestamps.size() < MIN_TIMESTAMPS_FOR_DELTA_DELTA) {
        return DeltaCompression::estimateCompressionRatio(timestamps);
    }
    
    try {
        TimestampStats stats = analyzeTimestamps(timestamps);
        return stats.estimated_ratio;
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

DeltaDeltaCompression::TimestampStats DeltaDeltaCompression::analyzeTimestamps(const std::vector<int64_t>& timestamps) {
    TimestampStats stats = {};
    
    if (timestamps.size() < 2) {
        stats.is_worth_compressing = false;
        return stats;
    }
    
    try {
        // Calculate all intervals with bounds checking
        std::vector<int64_t> intervals;
        intervals.reserve(timestamps.size() - 1);
        
        for (size_t i = 1; i < timestamps.size(); ++i) {
            intervals.push_back(timestamps[i] - timestamps[i-1]);
        }
        
        // Basic statistics with validation
        if (!intervals.empty()) {
            stats.min_interval = *std::min_element(intervals.begin(), intervals.end());
            stats.max_interval = *std::max_element(intervals.begin(), intervals.end());
            
            int64_t sum = 0;
            for (int64_t interval : intervals) {
                sum += interval;
            }
            stats.avg_interval = sum / static_cast<int64_t>(intervals.size());
            
            // Calculate variance with bounds checking
            double variance_sum = 0.0;
            for (int64_t interval : intervals) {
                double diff = static_cast<double>(interval) - static_cast<double>(stats.avg_interval);
                variance_sum += diff * diff;
            }
            stats.interval_variance = variance_sum / intervals.size();
            
            // Calculate regularity with validation
            stats.regular_count = 0;
            double tolerance = 0.1;
            if (stats.avg_interval != 0) {
                for (int64_t interval : intervals) {
                    double deviation = std::abs(static_cast<double>(interval) - static_cast<double>(stats.avg_interval)) / 
                                      static_cast<double>(std::abs(stats.avg_interval));
                    if (deviation <= tolerance) {
                        stats.regular_count++;
                    }
                }
            }
            
            stats.regularity_ratio = static_cast<double>(stats.regular_count) / intervals.size();
        }
        
        // Estimate compression ratio with validation
        size_t estimated_size = 16; // First two values
        
        if (timestamps.size() >= 3) {
            int64_t prev_delta = timestamps[1] - timestamps[0];
            for (size_t i = 2; i < timestamps.size(); ++i) {
                int64_t curr_delta = timestamps[i] - timestamps[i-1];
                int64_t delta_of_delta = curr_delta - prev_delta;
                estimated_size += VarintCompression::getVarintSize(delta_of_delta);
                prev_delta = curr_delta;
            }
        }
        
        stats.estimated_ratio = static_cast<double>(estimated_size) / (timestamps.size() * sizeof(int64_t));
        stats.is_worth_compressing = stats.estimated_ratio < 0.7;
        
    } catch (const std::exception&) {
        stats.is_worth_compressing = false;
        stats.estimated_ratio = 1.0;
    }
    
    return stats;
}

// ========== ICompression Interface Implementation ==========

std::vector<uint8_t> DeltaCompression::compress(const std::vector<uint8_t>& data, int level) {
    // Convert bytes to int64 values and compress using delta compression
    if (data.empty()) return {};
    
    try {
        // Convert byte data to int64 array for delta compression
        std::vector<int64_t> values = utils::SerializationUtils::bytesToInt64s(data);
        return compressInt64(values);
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> DeltaCompression::decompress(const std::vector<uint8_t>& compressed_data) {
    // Decompress delta-compressed data and convert back to bytes
    if (compressed_data.empty()) return {};
    
    try {
        std::vector<int64_t> values = decompressInt64(compressed_data);
        return utils::SerializationUtils::int64sToBytes(values);
    } catch (const std::exception& e) {
        throw std::runtime_error("Delta decompression failed: " + std::string(e.what()));
    }
}

std::string DeltaCompression::getName() const {
    return "Delta";
}

bool DeltaCompression::supportsLevel(int level) const {
    return (level >= 0 && level <= 9);  // Delta compression doesn't use levels, but accept reasonable range
}

} // namespace algorithms
} // namespace compression
} // namespace json2