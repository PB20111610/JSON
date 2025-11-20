#pragma once

#include <memory>
#include <vector>
#include <cstdint>
#include <string>

#include "../core/compression_backend.h"
#include "../core/compression_interface.h"

namespace json2 {
namespace compression {
namespace factory {

class CompressionFactory {
public:
    // 数据特征分析结构体
    struct NumericDataCharacteristics {
        size_t data_size = 0;
        bool is_signed = false;
        bool is_sorted = false;
        bool has_small_deltas = false;
        bool is_timestamp_like = false;
        double delta_variance = 0.0;
    };
    
    struct StringDataCharacteristics {
        size_t total_size = 0;
        double unique_ratio = 0.0;
        double avg_string_length = 0.0;
        bool has_common_prefixes = false;
        bool is_highly_repetitive = false;
    };
    
    // 创建通用压缩器
    static std::unique_ptr<ICompression> createCompressor(CompressionBackend backend);
    
    // 创建数值压缩器
    static std::unique_ptr<INumericCompression> createNumericCompressor(
        const NumericDataCharacteristics& characteristics);
    
    // 创建布尔值压缩器
    static std::unique_ptr<IBooleanCompression> createBooleanCompressor(double sparsity = 0.5);
    
    // 创建字符串压缩器
    static std::unique_ptr<IStringCompression> createStringCompressor(
        const StringDataCharacteristics& characteristics);
    
    // 选择最优压缩后端
    static CompressionBackend selectOptimalBackend(const std::vector<uint8_t>& data, FieldType type);
    
    // 压缩性能基准测试
    struct CompressionBenchmark {
        CompressionBackend backend;
        double compression_ratio;
        long long compression_time_us;
        long long decompression_time_us;
        bool is_available;
    };
    
    static std::vector<CompressionBenchmark> benchmarkCompression(const std::vector<uint8_t>& data);

private:
    // 数据分析方法
    static NumericDataCharacteristics analyzeNumericData(const std::vector<int64_t>& values);
    static NumericDataCharacteristics analyzeNumericData(const std::vector<uint32_t>& values);
    static NumericDataCharacteristics analyzeNumericData(const std::vector<double>& values);
    static StringDataCharacteristics analyzeStringData(const std::vector<std::string>& strings);
    
    // 辅助方法
    static std::vector<CompressionBackend> getAvailableBackends();
    static bool isBackendAvailable(CompressionBackend backend);
    static bool detectSortedData(const std::vector<int64_t>& values);
    static bool detectTimestamp(const std::vector<int64_t>& values);
    static double calculateDeltaVariance(const std::vector<int64_t>& values);
    static bool hasCommonPrefixes(const std::vector<std::string>& strings);
    
    // 压缩后端选择策略
    static CompressionBackend selectForNumericData(const NumericDataCharacteristics& characteristics);
    static CompressionBackend selectForBooleanData(double sparsity, size_t data_size);
    static CompressionBackend selectForStringData(const StringDataCharacteristics& characteristics);
    static CompressionBackend selectForGenericData(const std::vector<uint8_t>& data);
    static CompressionBackend selectForTimestampData(const std::vector<uint8_t>& data);
    static CompressionBackend selectForLogtypeData(const std::vector<uint8_t>& data);
};

} // namespace factory
} // namespace compression
} // namespace json2