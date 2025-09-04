#pragma once

#include "../core/compression_interface.h"
#include "varint_compression.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace algorithms {

/**
 * Delta压缩算法 - 差分编码
 * 适用于: 有序数值序列、时间序列数据
 */
class DeltaCompression : public INumericCompression {
public:
    DeltaCompression() = default;
    virtual ~DeltaCompression() = default;
    
    // ICompression 接口实现 (from base class)
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override;
    bool supportsLevel(int level) const override;
    
    // INumericCompression 接口实现
    std::vector<uint8_t> compressInt64(const std::vector<int64_t>& values) override;
    std::vector<int64_t> decompressInt64(const std::vector<uint8_t>& compressed) override;
    
    std::vector<uint8_t> compressUint32(const std::vector<uint32_t>& values) override;
    std::vector<uint32_t> decompressUint32(const std::vector<uint8_t>& compressed) override;
    
    std::vector<uint8_t> compressDouble(const std::vector<double>& values) override;
    std::vector<double> decompressDouble(const std::vector<uint8_t>& compressed) override;
    
    // Delta专用接口
    static std::vector<uint8_t> deltaCompress(const std::vector<int64_t>& values);
    static std::vector<int64_t> deltaDecompress(const std::vector<uint8_t>& compressed);
    
    // Delta + Varint组合压缩
    static std::vector<uint8_t> deltaVarintCompress(const std::vector<int64_t>& values);
    static std::vector<int64_t> deltaVarintDecompress(const std::vector<uint8_t>& compressed);
    
    // 数据分析：判断是否适合Delta压缩
    static bool isSorted(const std::vector<int64_t>& values);
    static bool hasSmallDeltas(const std::vector<int64_t>& values, int64_t threshold = 1000);
    static double estimateCompressionRatio(const std::vector<int64_t>& values);
    
    // 性能分析
    struct DeltaStats {
        bool is_sorted;
        int64_t min_delta;
        int64_t max_delta;
        double avg_delta;
        double delta_variance;
        double estimated_ratio;
        bool is_worth_compressing;
    };
    
    static DeltaStats analyzeData(const std::vector<int64_t>& values);
    
private:
    // 辅助函数：数据类型转换
    static std::vector<int64_t> uint32sToInt64s(const std::vector<uint32_t>& values);
    static std::vector<uint32_t> int64sToUint32s(const std::vector<int64_t>& values);
    static std::vector<int64_t> doublesToInt64s(const std::vector<double>& values);
    static std::vector<double> int64sToDoubles(const std::vector<int64_t>& values);
};

/**
 * Delta-of-Delta压缩算法 - 二阶差分编码
 * 专门适用于: 时间戳数据、等间隔序列数据
 */
class DeltaDeltaCompression {
public:
    DeltaDeltaCompression() = default;
    virtual ~DeltaDeltaCompression() = default;
    
    // Delta-of-Delta专用接口
    static std::vector<uint8_t> deltaDeltaCompress(const std::vector<int64_t>& timestamps);
    static std::vector<int64_t> deltaDeltaDecompress(const std::vector<uint8_t>& compressed);
    
    // 时间戳专用接口
    static std::vector<uint8_t> compressTimestamps(const std::vector<int64_t>& timestamps);
    static std::vector<int64_t> decompressTimestamps(const std::vector<uint8_t>& compressed);
    
    // 数据分析：判断是否适合Delta-of-Delta压缩
    static bool hasRegularIntervals(const std::vector<int64_t>& timestamps, double tolerance = 0.1);
    static bool hasSmallDeltaDeltas(const std::vector<int64_t>& timestamps, int64_t threshold = 100);
    static double estimateCompressionRatio(const std::vector<int64_t>& timestamps);
    
    // 时间序列分析
    struct TimestampStats {
        int64_t min_interval;
        int64_t max_interval;
        int64_t avg_interval;
        double interval_variance;
        size_t regular_count;      // 规律间隔的数量
        double regularity_ratio;   // 规律性比率
        double estimated_ratio;
        bool is_worth_compressing;
    };
    
    static TimestampStats analyzeTimestamps(const std::vector<int64_t>& timestamps);
    
private:
    // 降级处理：当Delta-of-Delta效果不佳时使用普通Delta压缩
    static constexpr size_t MIN_TIMESTAMPS_FOR_DELTA_DELTA = 3;
};

} // namespace algorithms
} // namespace compression
} // namespace json2