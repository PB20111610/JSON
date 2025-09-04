#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_backend.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace backends {

/**
 * Snappy压缩后端封装
 * 提供超快速压缩的通用算法
 */
class SnappyBackend : public ICompression {
public:
    SnappyBackend() = default;
    virtual ~SnappyBackend() = default;
    
    // ICompression 接口实现
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override { return "Snappy"; }
    bool supportsLevel(int level) const override { return level == 6; } // Snappy不支持压缩级别
    
    // Snappy专用接口
    static std::vector<uint8_t> snappyCompress(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> snappyDecompress(const std::vector<uint8_t>& compressed);
    
    // 可用性检查
    static bool isAvailable();
    
    // 性能评估
    static double estimateCompressionRatio(const std::vector<uint8_t>& data);
    static size_t estimateCompressionTime(size_t input_size);
    
    // 数据验证
    static bool isValidCompressedData(const std::vector<uint8_t>& compressed);
    static size_t getUncompressedLength(const std::vector<uint8_t>& compressed);
    
private:
    // Snappy库不可用时的回退策略
    std::vector<uint8_t> fallbackCompress(const std::vector<uint8_t>& data);
    std::vector<uint8_t> fallbackDecompress(const std::vector<uint8_t>& compressed);
};

} // namespace backends
} // namespace compression
} // namespace json2