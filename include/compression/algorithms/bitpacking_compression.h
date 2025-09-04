#pragma once

#include "../core/compression_interface.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace algorithms {

/**
 * Bit Packing 压缩算法
 * 适用于: 布尔值数据、Null掩码、LOUDS位图
 */
class BitPackingCompression : public IBooleanCompression {
public:
    BitPackingCompression() = default;
    virtual ~BitPackingCompression() = default;
    
    // ICompression 接口实现 (from base class)
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override;
    bool supportsLevel(int level) const override;
    
    // IBooleanCompression 接口实现
    std::vector<uint8_t> compressBool(const std::vector<bool>& values) override;
    std::vector<bool> decompressBool(const std::vector<uint8_t>& compressed, size_t count) override;
    
    // BitPacking专用接口
    static std::vector<uint8_t> bitPackingCompress(const std::vector<bool>& values);
    static std::vector<bool> bitPackingDecompress(const std::vector<uint8_t>& compressed, size_t count);
    
    // 支持三态压缩 (false/true/null)
    static std::vector<uint8_t> compressBoolStates(const std::vector<uint8_t>& states);
    static std::vector<uint8_t> decompressBoolStates(const std::vector<uint8_t>& compressed, size_t count);
    
    // 稀疏位图压缩 (优化稀疏数据)
    static std::vector<uint8_t> compressSparseBitmap(const std::vector<bool>& values);
    static std::vector<bool> decompressSparseBitmap(const std::vector<uint8_t>& compressed, size_t count);
    
    // 数据分析
    static double estimateCompressionRatio(const std::vector<bool>& values);
    static bool isSparse(const std::vector<bool>& values, double threshold = 0.1);
    
    // 性能分析
    struct BitPackingStats {
        size_t true_count;
        size_t false_count;
        double sparsity_ratio;
        double estimated_ratio;
        bool is_worth_compressing;
        bool recommend_sparse_encoding;
    };
    
    static BitPackingStats analyzeData(const std::vector<bool>& values);
    
private:
    // 内部辅助函数
    static size_t calculateCompressedSize(size_t bool_count);
    static bool shouldUseSparseEncoding(const std::vector<bool>& values, double threshold = 0.1);
};

} // namespace algorithms
} // namespace compression
} // namespace json2