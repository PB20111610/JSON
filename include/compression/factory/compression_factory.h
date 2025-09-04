#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_backend.h"
#include <memory>
#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

// Forward declarations to avoid circular dependency
namespace json2 {
    namespace compression {
        namespace type_aware {
            class TypeAwareCompressor;
        }
    }
}

namespace json2 {
namespace compression {
namespace factory {

/**
 * 压缩算法工厂类
 * 负责根据数据特征和配置选择最优的压缩算法
 */
class CompressionFactory {
public:
    CompressionFactory() = default;
    virtual ~CompressionFactory() = default;
    
    // 数据特征分析结构 - 必须在使用前定义
    struct NumericDataCharacteristics {
        bool is_sorted;              // 是否有序
        bool has_small_deltas;       // 是否有小的差值
        bool is_timestamp_like;      // 是否类似时间戳
        double delta_variance;       // 差值方差
        size_t data_size;           // 数据大小
        bool is_signed;             // 是否有符号
    };
    
    struct StringDataCharacteristics {
        double unique_ratio;         // 唯一性比率
        double avg_string_length;    // 平均字符串长度
        size_t total_size;          // 总大小
        bool has_common_prefixes;    // 是否有公共前缀
        bool is_highly_repetitive;   // 是否高度重复
    };
    
    /**
     * 创建通用压缩器
     * @param backend 压缩后端类型
     * @return 压缩器实例
     */
    static std::unique_ptr<ICompression> createCompressor(CompressionBackend backend);
    
    /**
     * 创建数值压缩器
     * @param data_characteristics 数据特征
     * @return 数值压缩器实例
     */
    static std::unique_ptr<INumericCompression> createNumericCompressor(const NumericDataCharacteristics& data_characteristics);
    
    /**
     * 创建布尔值压缩器
     * @param sparsity 稀疏度
     * @return 布尔值压缩器实例
     */
    static std::unique_ptr<IBooleanCompression> createBooleanCompressor(double sparsity = 0.5);
    
    /**
     * 创建字符串压缩器
     * @param string_characteristics 字符串特征
     * @return 字符串压缩器实例
     */
    static std::unique_ptr<IStringCompression> createStringCompressor(const StringDataCharacteristics& string_characteristics);
    
    /**
     * 创建类型感知压缩器
     * @param config 压缩配置
     * @return 类型感知压缩器实例
     */
    static std::unique_ptr<type_aware::TypeAwareCompressor> createTypeAwareCompressor(const TypeAwareCompressionConfig& config = {});
    
    /**
     * 自动选择最优压缩算法
     * @param data 原始数据
     * @param type 数据类型
     * @return 推荐的压缩后端
     */
    static CompressionBackend selectOptimalBackend(const std::vector<uint8_t>& data, FieldType type);
    
    /**
     * 分析数值数据特征
     */
    static NumericDataCharacteristics analyzeNumericData(const std::vector<int64_t>& values);
    static NumericDataCharacteristics analyzeNumericData(const std::vector<uint32_t>& values);
    static NumericDataCharacteristics analyzeNumericData(const std::vector<double>& values);
    
    /**
     * 分析字符串数据特征
     */
    static StringDataCharacteristics analyzeStringData(const std::vector<std::string>& strings);
    
    /**
     * 性能基准测试
     */
    struct CompressionBenchmark {
        CompressionBackend backend;
        double compression_ratio;
        size_t compression_time_us;  // 微秒
        size_t decompression_time_us;
        bool is_available;
    };
    
    /**
     * 对数据进行压缩性能基准测试
     */
    static std::vector<CompressionBenchmark> benchmarkCompression(const std::vector<uint8_t>& data);
    
    /**
     * 获取所有可用的压缩后端
     */
    static std::vector<CompressionBackend> getAvailableBackends();
    
    /**
     * 检查压缩后端是否可用
     */
    static bool isBackendAvailable(CompressionBackend backend);
    
private:
    // 算法选择策略
    static CompressionBackend selectForNumericData(const NumericDataCharacteristics& characteristics);
    static CompressionBackend selectForStringData(const StringDataCharacteristics& characteristics);
    static CompressionBackend selectForBooleanData(double sparsity, size_t data_size);
    static CompressionBackend selectForGenericData(const std::vector<uint8_t>& data);
    
    // 数据分析工具
    static bool detectTimestamp(const std::vector<int64_t>& values);
    static bool detectSortedData(const std::vector<int64_t>& values);
    static double calculateDeltaVariance(const std::vector<int64_t>& values);
    static bool hasCommonPrefixes(const std::vector<std::string>& strings);
    
    // 性能估算
    static double estimateCompressionRatio(CompressionBackend backend, const std::vector<uint8_t>& data);
    static size_t estimateCompressionTime(CompressionBackend backend, size_t data_size);
};

} // namespace factory
} // namespace compression
} // namespace json2