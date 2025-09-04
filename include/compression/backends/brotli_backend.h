#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_backend.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace backends {

/**
 * Brotli压缩后端封装
 * 提供高压缩率的通用压缩算法
 */
class BrotliBackend : public ICompression {
public:
    BrotliBackend() = default;
    virtual ~BrotliBackend() = default;
    
    // ICompression 接口实现
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override { return "Brotli"; }
    bool supportsLevel(int level) const override { return level >= 0 && level <= 11; }
    
    // Brotli专用接口
    static std::vector<uint8_t> brotliCompress(const std::vector<uint8_t>& data, int quality = 6);
    static std::vector<uint8_t> brotliDecompress(const std::vector<uint8_t>& compressed);
    
    // 可用性检查
    static bool isAvailable();
    
    // 性能评估
    static double estimateCompressionRatio(const std::vector<uint8_t>& data, int level = 6);
    static size_t estimateCompressionTime(size_t input_size, int level = 6);
    
private:
    // Brotli库不可用时的回退策略
    std::vector<uint8_t> fallbackCompress(const std::vector<uint8_t>& data);
    std::vector<uint8_t> fallbackDecompress(const std::vector<uint8_t>& compressed);
};

} // namespace backends
} // namespace compression
} // namespace json2