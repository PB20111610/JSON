#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_backend.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace backends {

/**
 * LZ4压缩后端封装
 * 提供快速压缩的通用算法
 */
class Lz4Backend : public ICompression {
public:
    Lz4Backend() = default;
    virtual ~Lz4Backend() = default;
    
    // ICompression 接口实现
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override { return "LZ4"; }
    bool supportsLevel(int level) const override { return level >= 1 && level <= 12; }
    
    // LZ4专用接口
    static std::vector<uint8_t> lz4Compress(const std::vector<uint8_t>& data, int level = 6);
    static std::vector<uint8_t> lz4Decompress(const std::vector<uint8_t>& compressed);
    
    // 高压缩模式
    static std::vector<uint8_t> lz4HcCompress(const std::vector<uint8_t>& data, int level = 9);
    
    // 可用性检查
    static bool isAvailable();
    
    // 性能评估
    static double estimateCompressionRatio(const std::vector<uint8_t>& data, int level = 6);
    static size_t estimateCompressionTime(size_t input_size, int level = 6);
    
private:
    // LZ4库不可用时的回退策略
    std::vector<uint8_t> fallbackCompress(const std::vector<uint8_t>& data);
    std::vector<uint8_t> fallbackDecompress(const std::vector<uint8_t>& compressed);
};

} // namespace backends
} // namespace compression
} // namespace json2