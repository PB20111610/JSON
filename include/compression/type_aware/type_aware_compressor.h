#pragma once

#include "../factory/compression_factory.h"
#include "../core/compression_backend.h"
#include <memory>
#include <chrono>
#include <vector>
#include <string>

// 前向声明以避免循环依赖
namespace json2 {
    class Trie;
    class FieldDictionaryManager;
    class LOUDSTrie;
    class FieldKey;
    struct CompressedData;
}

namespace json2 {
namespace compression {
namespace type_aware {

/**
 * 新的模块化类型感知压缩器
 * 使用工厂模式和分离的算法模块
 */
class TypeAwareCompressor {
public:
    // 构造函数
    explicit TypeAwareCompressor(const TypeAwareCompressionConfig& config = TypeAwareCompressionConfig{});
    
    // 主要压缩/解压接口 - 核心数据类型压缩
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, FieldType type);
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed, FieldType type);

    // 高级压缩接口 - 处理完整的数据结构
    CompressedData compress(const Trie& trie, const FieldDictionaryManager& manager);
    std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompress(const CompressedData& compressed_data);
    
    // LOUDS压缩接口
    CompressedData compressLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, 
                                const std::vector<FieldKey>& field_order);
    std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressLouds(const CompressedData& compressed_data);
    
    // 分层数据压缩 - 核心重构接口
    std::vector<uint8_t> compressLayerData(const std::vector<uint8_t>& data, FieldType type);
    std::vector<uint8_t> decompressLayerData(const std::vector<uint8_t>& compressed, FieldType type);
    
    // 配置管理
    void setConfig(const TypeAwareCompressionConfig& config) { config_ = config; }
    const TypeAwareCompressionConfig& getConfig() const { return config_; }
    
    // 性能分析
    struct CompressionStats {
        size_t original_size;
        size_t compressed_size;
        double compression_ratio;
        CompressionBackend backend_used;
        std::string algorithm_name;
        double compression_time_ms;
        double decompression_time_ms;
        bool is_successful;
    };
    
    CompressionStats getLastStats() const { return last_stats_; }
    std::vector<CompressionStats> getAllStats() const { return all_stats_; }
    void clearStats() { all_stats_.clear(); }
    
    // 批量处理
    struct BatchCompressionResult {
        std::vector<std::vector<uint8_t>> compressed_layers;
        std::vector<CompressionStats> layer_stats;
        CompressionStats overall_stats;
    };
    
    BatchCompressionResult compressBatch(const std::vector<std::pair<FieldType, std::vector<uint8_t>>>& layers);
    
private:
    TypeAwareCompressionConfig config_;
    mutable CompressionStats last_stats_;
    mutable std::vector<CompressionStats> all_stats_;
    
    // 内部压缩方法
    std::vector<uint8_t> compressWithSelectedBackend(const std::vector<uint8_t>& data, 
                                                    FieldType type, 
                                                    CompressionBackend backend);
    std::vector<uint8_t> decompressWithSelectedBackend(const std::vector<uint8_t>& compressed, 
                                                      CompressionBackend backend);
    
    // 特定数据类型压缩
    std::vector<uint8_t> compressLoudsBitmap(const std::vector<uint8_t>& bitmap);
    std::vector<uint8_t> decompressLoudsBitmap(const std::vector<uint8_t>& compressed);
    
    std::vector<uint8_t> compressFieldKeys(const std::vector<FieldKey>& field_keys);
    std::vector<FieldKey> decompressFieldKeys(const std::vector<uint8_t>& compressed);
    
    std::vector<uint8_t> compressDictionaryData(const FieldDictionaryManager& manager);
    std::unique_ptr<FieldDictionaryManager> decompressDictionaryData(const std::vector<uint8_t>& compressed,
                                                                    const std::vector<FieldKey>& field_keys);
    
    // 性能监控
    void recordCompressionStats(size_t original_size, size_t compressed_size, 
                               CompressionBackend backend, double time_ms, bool success = true);
    
    // 辅助方法
    std::string getBackendName(CompressionBackend backend) const;
    
    // 后端选择器
    CompressionBackend selectBackendForType(FieldType type, const std::vector<uint8_t>& data);
    CompressionBackend selectBackendForFieldType(FieldType type);
    
    // 新的配置驱动压缩方法
    std::vector<uint8_t> compressWithBackend(const std::vector<uint8_t>& data, 
                                            CompressionBackend backend, 
                                            FieldType type);
    std::vector<uint8_t> compressWithIntelligentSelection(const std::vector<uint8_t>& data, FieldType type);
    std::vector<uint8_t> compressWithExternalBackend(const std::vector<uint8_t>& data, CompressionBackend backend);
    
    // 时间测量工具
    class Timer {
    public:
        Timer() : start_(std::chrono::high_resolution_clock::now()) {}
        double elapsedMs() const {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
            return duration.count() / 1000.0;
        }
    private:
        std::chrono::high_resolution_clock::time_point start_;
    };
    
    // 序列化和反序列化工具方法
    std::vector<uint8_t> serializeTrie(const Trie& trie);
    std::unique_ptr<Trie> deserializeTrie(const std::vector<uint8_t>& data);
    
    std::vector<uint8_t> serializeDictionary(const FieldDictionaryManager& manager);
    std::unique_ptr<FieldDictionaryManager> deserializeDictionary(const std::vector<uint8_t>& data);
    
    std::vector<uint8_t> serializeLouds(const LOUDSTrie& louds);
    std::unique_ptr<LOUDSTrie> deserializeLouds(const std::vector<uint8_t>& data, const std::vector<FieldKey>& field_order);
    
    std::vector<uint8_t> serializeFieldOrder(const std::vector<FieldKey>& field_order);
    std::vector<FieldKey> deserializeFieldOrder(const std::vector<uint8_t>& data);
    
    // 特定数据类型解压方法
    std::vector<uint8_t> decompressVarintField(const std::vector<uint8_t>& compressed, size_t& pos);
    std::vector<uint8_t> decompressDeltaField(const std::vector<uint8_t>& compressed, size_t& pos);
    std::vector<uint8_t> decompressBitPackingField(const std::vector<uint8_t>& compressed, size_t& pos);
    std::vector<uint8_t> decompressDictionaryField(const std::vector<uint8_t>& compressed, size_t& pos);
    std::vector<uint8_t> decompressBackendField(const std::vector<uint8_t>& compressed, size_t& pos);
};

/**
 * 向后兼容的静态接口包装器
 * 保持原有的TypeAwareCompressor接口
 */
class LegacyInterface {
public:
    // 原有的静态接口
    static CompressedData compress(const Trie& trie, const FieldDictionaryManager& manager, 
                                  const TypeAwareCompressionConfig& config = TypeAwareCompressionConfig{});
    
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompress(const CompressedData& compressed_data, 
              const TypeAwareCompressionConfig& config = TypeAwareCompressionConfig{});
    
    static CompressedData compressLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, 
                                       const std::vector<FieldKey>& field_order,
                                       const TypeAwareCompressionConfig& config = TypeAwareCompressionConfig{});
    
    static std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressLouds(const CompressedData& compressed_data, 
                   const TypeAwareCompressionConfig& config = TypeAwareCompressionConfig{});
    
    // 各种算法的静态接口
    static std::vector<uint8_t> rleCompress(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> rleDecompress(const std::vector<uint8_t>& compressed);
    
    static std::vector<uint8_t> deltaVarintCompress(const std::vector<int64_t>& values);
    static std::vector<int64_t> deltaVarintDecompress(const std::vector<uint8_t>& compressed);
    
    static std::vector<uint8_t> bitPackingCompress(const std::vector<bool>& values);
    static std::vector<bool> bitPackingDecompress(const std::vector<uint8_t>& compressed, size_t count);
    
    static std::vector<uint8_t> dictionaryCompress(const std::vector<std::string>& strings, 
                                                  CompressionBackend backend = CompressionBackend::AUTO);
    static std::vector<std::string> dictionaryDecompress(const std::vector<uint8_t>& compressed, 
                                                        CompressionBackend backend = CompressionBackend::AUTO);
    
    static std::vector<uint8_t> deltaDeltaCompress(const std::vector<int64_t>& timestamps);
    static std::vector<int64_t> deltaDeltaDecompress(const std::vector<uint8_t>& compressed);
    
    static void encodeVarint(int64_t value, std::vector<uint8_t>& output);
    static int64_t decodeVarint(const std::vector<uint8_t>& data, size_t& pos);
    
    // 工具函数
    static std::vector<int64_t> bytesToInt64s(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> int64sToBytes(const std::vector<int64_t>& values);
    static std::vector<bool> bytesToBools(const std::vector<uint8_t>& data, size_t count);
    static std::vector<uint8_t> boolsToBytes(const std::vector<bool>& values);
    static std::vector<uint32_t> bytesToUint32s(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> uint32sToBytes(const std::vector<uint32_t>& values);

private:
    static TypeAwareCompressor& getDefaultInstance();
};

} // namespace type_aware
} // namespace compression
} // namespace json2