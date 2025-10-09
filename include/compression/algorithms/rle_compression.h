#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_utils.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace algorithms {

/**
 * RLE (Run-Length Encoding) 压缩算法
 * 适用于: LOUDS位图、重复序列数据
 */
class RLECompression : public ICompression {
public:
    RLECompression() = default;
    virtual ~RLECompression() = default;
    
    // ICompression 接口实现
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override { return "RLE"; }
    bool supportsLevel(int level) const override { return level == 6; } // RLE不支持压缩级别
    
    // 支持部分解压
    bool supportsPartialDecompression() const override { return true; }
    
    // RLE专用接口
    static std::vector<uint8_t> rleCompress(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> rleDecompress(const std::vector<uint8_t>& compressed);
    
    // 部分解压指定索引的值
    static uint8_t rleDecompressAt(const std::vector<uint8_t>& compressed, size_t index);
    
    // 数据分析：判断是否适合RLE压缩
    static bool isHighlyRepetitive(const std::vector<uint8_t>& data, double threshold = 0.5);
    static double estimateCompressionRatio(const std::vector<uint8_t>& data);
    
    // 性能分析
    struct RLEStats {
        size_t run_count;           // 游程数量
        double repetition_ratio;    // 重复率
        double estimated_ratio;     // 预估压缩比
        bool is_worth_compressing;  // 是否值得压缩
    };
    
    static RLEStats analyzeData(const std::vector<uint8_t>& data);

private:
    // 辅助函数：查找指定索引所在的游程
    static std::pair<uint8_t, size_t> findRunAtIndex(const std::vector<uint8_t>& compressed, size_t index);
};

} // namespace algorithms
} // namespace compression
} // namespace json2