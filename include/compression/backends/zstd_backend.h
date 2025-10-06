#pragma once

#include "../core/compression_interface.h"
#include <vector>
#include <cstdint>

namespace json2 {
namespace compression {
namespace backends {

/**
 * ZSTD 压缩库包装器
 * 高效的通用压缩算法，适用于大多数数据类型
 */
class ZstdBackend : public ICompression {
public:
    ZstdBackend() = default;
    virtual ~ZstdBackend() = default;
    
    // ICompression 接口实现
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override { return "ZSTD"; }
    bool supportsLevel(int level) const override { return level >= 1 && level <= 22; }
    
    // ZSTD特定功能
    static bool isAvailable();
    static std::string getVersionInfo();
    static std::vector<uint8_t> compressWithLevel(const std::vector<uint8_t>& data, int level);
    static std::vector<uint8_t> decompressZstd(const std::vector<uint8_t>& compressed_data);
    
    // 字典支持
    static std::vector<uint8_t> compressWithDictionary(const std::vector<uint8_t>& data, 
                                                      const std::vector<uint8_t>& dictionary, 
                                                      int level = 6);
    static std::vector<uint8_t> decompressWithDictionary(const std::vector<uint8_t>& compressed_data,
                                                        const std::vector<uint8_t>& dictionary);
    
    // 性能分析
    static double estimateCompressionRatio(const std::vector<uint8_t>& data, int level = 6);
    static bool isWorthCompressing(const std::vector<uint8_t>& data, size_t min_size = 100);
    
    // 训练字典 (适用于大量相似数据)
    static std::vector<uint8_t> trainDictionary(const std::vector<std::vector<uint8_t>>& samples, 
                                               size_t dict_size = 112640);
    
private:
    static constexpr int DEFAULT_LEVEL = 6;
    static constexpr int MIN_LEVEL = 1;
    static constexpr int MAX_LEVEL = 22;
    static constexpr size_t MIN_SIZE_FOR_COMPRESSION = 50;
};

} // namespace backends
} // namespace compression
} // namespace json2