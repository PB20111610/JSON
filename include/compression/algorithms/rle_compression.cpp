#include "rle_compression.h"
#include "../core/compression_utils.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace json2 {
namespace compression {
namespace algorithms {

// ========== Helper Functions (模仿compress.cpp的风格) ==========
static void writeRun(std::vector<uint8_t>& output, uint8_t value, uint8_t count) {
    output.push_back(value);
    output.push_back(count);
}

static std::pair<uint8_t, uint8_t> readRun(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos + 1 >= data.size()) {
        throw std::runtime_error("RLE: Insufficient data for run");
    }
    uint8_t value = data[pos++];
    uint8_t count = data[pos++];
    return {value, count};
}

// ========== Core RLE Implementation ==========
std::vector<uint8_t> RLECompression::compress(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return data;
    
    // 分析数据，判断是否适合RLE压缩
    RLEStats stats = analyzeData(data);
    if (!stats.is_worth_compressing) {
        // 如果不值得压缩，返回原数据（加上标志位）
        std::vector<uint8_t> result;
        result.push_back(0); // 标志：未压缩
        result.insert(result.end(), data.begin(), data.end());
        return result;
    }
    
    std::vector<uint8_t> result;
    result.push_back(1); // 标志：已压缩
    
    auto compressed = rleCompress(data);
    result.insert(result.end(), compressed.begin(), compressed.end());
    
    return result;
}

std::vector<uint8_t> RLECompression::decompress(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return compressed_data;
    
    // 检查压缩标志
    if (compressed_data[0] == 0) {
        // 未压缩数据
        return std::vector<uint8_t>(compressed_data.begin() + 1, compressed_data.end());
    } else if (compressed_data[0] == 1) {
        // 压缩数据
        std::vector<uint8_t> actual_data(compressed_data.begin() + 1, compressed_data.end());
        return rleDecompress(actual_data);
    } else {
        throw std::runtime_error("RLE: Invalid compression flag");
    }
}

std::vector<uint8_t> RLECompression::rleCompress(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> compressed;
    if (data.empty()) return compressed;
    
    uint8_t current_value = data[0];
    uint8_t run_length = 1;
    
    for (size_t i = 1; i < data.size(); ++i) {
        if (data[i] == current_value && run_length < 255) {
            ++run_length;
        } else {
            // 写入当前游程
            writeRun(compressed, current_value, run_length);
            current_value = data[i];
            run_length = 1;
        }
    }
    
    // 写入最后一个游程
    writeRun(compressed, current_value, run_length);
    
    return compressed;
}

std::vector<uint8_t> RLECompression::rleDecompress(const std::vector<uint8_t>& compressed) {
    std::vector<uint8_t> decompressed;
    
    if (compressed.size() % 2 != 0) {
        throw std::runtime_error("RLE: Invalid compressed data length");
    }
    
    size_t pos = 0;
    while (pos < compressed.size()) {
        auto [value, count] = readRun(compressed, pos);
        
        if (count == 0) {
            throw std::runtime_error("RLE: Invalid run length");
        }
        
        decompressed.insert(decompressed.end(), count, value);
    }
    
    return decompressed;
}

bool RLECompression::isHighlyRepetitive(const std::vector<uint8_t>& data, double threshold) {
    return utils::DataAnalysisUtils::isHighlyRepetitive(data, threshold);
}

double RLECompression::estimateCompressionRatio(const std::vector<uint8_t>& data) {
    if (data.empty()) return 1.0;
    
    RLEStats stats = analyzeData(data);
    return stats.estimated_ratio;
}

RLECompression::RLEStats RLECompression::analyzeData(const std::vector<uint8_t>& data) {
    RLEStats stats = {};
    
    if (data.empty()) {
        stats.is_worth_compressing = false;
        return stats;
    }
    
    // 计算游程数量
    stats.run_count = 1;
    uint8_t current = data[0];
    
    for (size_t i = 1; i < data.size(); ++i) {
        if (data[i] != current) {
            stats.run_count++;
            current = data[i];
        }
    }
    
    // 计算重复率
    stats.repetition_ratio = utils::DataAnalysisUtils::calculateRepetitionRatio(data);
    
    // 估算压缩比: 每个run需要2字节(值+计数)
    size_t compressed_size = stats.run_count * 2;
    stats.estimated_ratio = static_cast<double>(compressed_size) / data.size();
    
    // 判断是否值得压缩
    stats.is_worth_compressing = (stats.estimated_ratio < 0.9) && (data.size() > 10);
    
    return stats;
}

} // namespace algorithms
} // namespace compression
} // namespace json2