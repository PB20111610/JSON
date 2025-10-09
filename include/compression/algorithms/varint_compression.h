#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_utils.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace algorithms {

/**
 * Varint编码算法 - 可变长度整数编码
 * 适用于: 整数序列的高效编码
 */
class VarintCompression : public INumericCompression {
public:
    VarintCompression() = default;
    virtual ~VarintCompression() = default;
    
    // ICompression 接口实现 (from base class)
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override;
    bool supportsLevel(int level) const override;
    
    // 支持部分解压
    bool supportsPartialDecompression() const override { return true; }
    
    // INumericCompression 接口实现
    std::vector<uint8_t> compressInt64(const std::vector<int64_t>& values) override;
    std::vector<int64_t> decompressInt64(const std::vector<uint8_t>& compressed) override;
    
    // 部分解压指定索引的值
    int64_t decompressInt64At(const std::vector<uint8_t>& compressed, size_t index) override;
    
    std::vector<uint8_t> compressUint32(const std::vector<uint32_t>& values) override;
    std::vector<uint32_t> decompressUint32(const std::vector<uint8_t>& compressed) override;
    
    // 部分解压指定索引的值
    uint32_t decompressUint32At(const std::vector<uint8_t>& compressed, size_t index) override;
    
    std::vector<uint8_t> compressDouble(const std::vector<double>& values) override;
    std::vector<double> decompressDouble(const std::vector<uint8_t>& compressed) override;
    
    // 部分解压指定索引的值
    double decompressDoubleAt(const std::vector<uint8_t>& compressed, size_t index) override;
    
    // Varint编码接口
    static std::vector<uint8_t> compressInt64Array(const std::vector<int64_t>& values);
    static std::vector<int64_t> decompressInt64Array(const std::vector<uint8_t>& compressed);
    
    // 部分解压指定索引的值
    static int64_t decompressInt64ArrayAt(const std::vector<uint8_t>& compressed, size_t index);
    
    static std::vector<uint8_t> compressUint32Array(const std::vector<uint32_t>& values);
    static std::vector<uint32_t> decompressUint32Array(const std::vector<uint8_t>& compressed);
    
    // 部分解压指定索引的值
    static uint32_t decompressUint32ArrayAt(const std::vector<uint8_t>& compressed, size_t index);
    
    // 单个值编码
    static void encodeVarint(int64_t value, std::vector<uint8_t>& output);
    static int64_t decodeVarint(const std::vector<uint8_t>& data, size_t& pos);
    
    // 部分解压指定索引的值
    static int64_t decodeVarintAt(const std::vector<uint8_t>& data, size_t index);
    
    // ZigZag编码 (有符号整数转无符号)
    static uint64_t zigzagEncode(int64_t value);
    static int64_t zigzagDecode(uint64_t value);
    
    // 性能分析
    static size_t estimateCompressedSize(const std::vector<int64_t>& values);
    static double estimateCompressionRatio(const std::vector<int64_t>& values);
    
    // 辅助工具
    static size_t getVarintSize(int64_t value);
    static bool isVarintEfficient(const std::vector<int64_t>& values, double threshold = 0.8);
};

} // namespace algorithms
} // namespace compression
} // namespace json2