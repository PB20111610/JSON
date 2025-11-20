#include "compression_factory.h"
#include "../algorithms/rle_compression.h"
#include "../algorithms/varint_compression.h"
#include "../algorithms/delta_compression.h"
#include "../algorithms/bitpacking_compression.h"
#include "../algorithms/dictionary_compression.h"
#include "../backends/zstd_backend.h"
#include "../backends/brotli_backend.h"
#include "../backends/lzma_backend.h"
#include "../backends/lz4_backend.h"
#include "../backends/snappy_backend.h"
#include "../core/compression_utils.h"
#include <algorithm>
#include <chrono>

namespace json2 {
namespace compression {
namespace factory {

std::unique_ptr<ICompression> CompressionFactory::createCompressor(CompressionBackend backend) {
    switch (backend) {
        case CompressionBackend::ZSTD:
            return std::make_unique<backends::ZstdBackend>();
        case CompressionBackend::BROTLI:
            return std::make_unique<backends::BrotliBackend>();
        case CompressionBackend::LZMA:
            return std::make_unique<backends::LzmaBackend>();
        case CompressionBackend::LZ4:
            return std::make_unique<backends::Lz4Backend>();
        case CompressionBackend::SNAPPY:
            return std::make_unique<backends::SnappyBackend>();
        case CompressionBackend::RLE:
            return std::make_unique<algorithms::RLECompression>();
        default:
            // 默认使用ZSTD
            return std::make_unique<backends::ZstdBackend>();
    }
}

std::unique_ptr<INumericCompression> CompressionFactory::createNumericCompressor(const NumericDataCharacteristics& characteristics) {
    if (characteristics.is_sorted && characteristics.has_small_deltas) {
        // 有序且差值较小，使用Delta压缩
        return std::make_unique<algorithms::DeltaCompression>();
    } else if (characteristics.is_timestamp_like) {
        // 时间戳数据，使用Delta压缩
        return std::make_unique<algorithms::DeltaCompression>();
    } else {
        // 通用数值数据，使用Varint压缩
        return std::make_unique<algorithms::VarintCompression>();
    }
}

std::unique_ptr<IBooleanCompression> CompressionFactory::createBooleanCompressor(double sparsity) {
    // 布尔值数据使用BitPacking压缩
    return std::make_unique<algorithms::BitPackingCompression>();
}

std::unique_ptr<IStringCompression> CompressionFactory::createStringCompressor(const StringDataCharacteristics& characteristics) {
    // 字符串数据使用Dictionary压缩
    return std::make_unique<algorithms::DictionaryCompression>();
}

// Removed createTypeAwareCompressor method since it's no longer needed

CompressionBackend CompressionFactory::selectOptimalBackend(const std::vector<uint8_t>& data, FieldType type) {
    switch (type) {
        case FieldType::INT64:
        case FieldType::UINT32:
        case FieldType::DOUBLE:
            {
                // 数值类型数据分析
                if (type == FieldType::INT64) {
                    std::vector<int64_t> values = utils::SerializationUtils::bytesToInt64s(data);
                    NumericDataCharacteristics chars = analyzeNumericData(values);
                    return selectForNumericData(chars);
                } else if (type == FieldType::UINT32) {
                    std::vector<uint32_t> values = utils::SerializationUtils::bytesToUint32s(data);
                    NumericDataCharacteristics chars = analyzeNumericData(values);
                    return selectForNumericData(chars);
                } else {
                    std::vector<double> values = utils::SerializationUtils::bytesToDoubles(data);
                    NumericDataCharacteristics chars = analyzeNumericData(values);
                    return selectForNumericData(chars);
                }
            }
        case FieldType::BOOL:
            {
                std::vector<bool> values = utils::SerializationUtils::bytesToBools(data, data.size());
                double sparsity = utils::DataAnalysisUtils::calculateTrueFalseRatio(values);
                return selectForBooleanData(sparsity, data.size());
            }
        case FieldType::TIMESTAMP:
            {
                // 时间戳数据特殊处理 - 使用专门的方法
                return selectForTimestampData(data);
            }
        case FieldType::LOGTYPE:
            {
                // 日志类型数据特殊处理 - 使用专门的方法
                return selectForLogtypeData(data);
            }
        case FieldType::STRING:
            {
                // 字符串数据需要特殊处理，这里简化处理
                StringDataCharacteristics chars;
                chars.total_size = data.size();
                chars.unique_ratio = 0.5; // 默认估值
                chars.avg_string_length = 20; // 默认估值
                chars.has_common_prefixes = false;
                chars.is_highly_repetitive = false;
                return selectForStringData(chars);
            }
        default:
            return selectForGenericData(data);
    }
}

CompressionFactory::NumericDataCharacteristics CompressionFactory::analyzeNumericData(const std::vector<int64_t>& values) {
    NumericDataCharacteristics chars = {};
    
    if (values.empty()) return chars;
    
    chars.data_size = values.size() * sizeof(int64_t);
    chars.is_signed = true;
    chars.is_sorted = detectSortedData(values);
    chars.has_small_deltas = utils::DataAnalysisUtils::hasSmallDeltas(values);
    chars.is_timestamp_like = detectTimestamp(values);
    chars.delta_variance = calculateDeltaVariance(values);
    
    return chars;
}

CompressionFactory::NumericDataCharacteristics CompressionFactory::analyzeNumericData(const std::vector<uint32_t>& values) {
    std::vector<int64_t> int64_values(values.begin(), values.end());
    NumericDataCharacteristics chars = analyzeNumericData(int64_values);
    chars.is_signed = false;
    chars.data_size = values.size() * sizeof(uint32_t);
    return chars;
}

CompressionFactory::NumericDataCharacteristics CompressionFactory::analyzeNumericData(const std::vector<double>& values) {
    NumericDataCharacteristics chars = {};
    
    if (values.empty()) return chars;
    
    chars.data_size = values.size() * sizeof(double);
    chars.is_signed = true;
    
    // 对于浮点数，分析相对简单
    chars.is_sorted = std::is_sorted(values.begin(), values.end());
    chars.has_small_deltas = false; // 浮点数差值分析复杂
    chars.is_timestamp_like = false;
    chars.delta_variance = 1.0; // 默认值
    
    return chars;
}

CompressionFactory::StringDataCharacteristics CompressionFactory::analyzeStringData(const std::vector<std::string>& strings) {
    StringDataCharacteristics chars = {};
    
    if (strings.empty()) return chars;
    
    // 计算总大小和平均长度
    size_t total_length = 0;
    for (const auto& str : strings) {
        total_length += str.length();
    }
    
    chars.total_size = total_length;
    chars.avg_string_length = static_cast<double>(total_length) / strings.size();
    
    // 计算唯一性比率
    chars.unique_ratio = utils::DataAnalysisUtils::calculateUniqueRatio(strings);
    
    // 检查重复性
    chars.is_highly_repetitive = utils::DataAnalysisUtils::isHighlyRedundant(strings);
    
    // 检查公共前缀
    chars.has_common_prefixes = hasCommonPrefixes(strings);
    
    return chars;
}

std::vector<CompressionFactory::CompressionBenchmark> CompressionFactory::benchmarkCompression(const std::vector<uint8_t>& data) {
    std::vector<CompressionBenchmark> results;
    std::vector<CompressionBackend> backends = getAvailableBackends();
    
    for (CompressionBackend backend : backends) {
        CompressionBenchmark benchmark = {};
        benchmark.backend = backend;
        benchmark.is_available = isBackendAvailable(backend);
        
        if (!benchmark.is_available) {
            benchmark.compression_ratio = 1.0;
            benchmark.compression_time_us = 0;
            benchmark.decompression_time_us = 0;
            results.push_back(benchmark);
            continue;
        }
        
        try {
            auto compressor = createCompressor(backend);
            
            // 测试压缩时间
            auto start = std::chrono::high_resolution_clock::now();
            std::vector<uint8_t> compressed = compressor->compress(data);
            auto end = std::chrono::high_resolution_clock::now();
            
            auto compression_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            benchmark.compression_time_us = compression_duration.count();
            
            // 计算压缩比
            benchmark.compression_ratio = static_cast<double>(compressed.size()) / data.size();
            
            // 测试解压时间
            start = std::chrono::high_resolution_clock::now();
            std::vector<uint8_t> decompressed = compressor->decompress(compressed);
            end = std::chrono::high_resolution_clock::now();
            
            auto decompression_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            benchmark.decompression_time_us = decompression_duration.count();
            
        } catch (...) {
            // 如果测试失败，标记为不可用
            benchmark.is_available = false;
            benchmark.compression_ratio = 1.0;
            benchmark.compression_time_us = 0;
            benchmark.decompression_time_us = 0;
        }
        
        results.push_back(benchmark);
    }
    
    return results;
}

std::vector<CompressionBackend> CompressionFactory::getAvailableBackends() {
    return {
        CompressionBackend::ZSTD,
        CompressionBackend::BROTLI,
        CompressionBackend::LZMA,
        CompressionBackend::LZ4,
        CompressionBackend::SNAPPY,
        CompressionBackend::RLE,
        CompressionBackend::DELTA_VARINT,
        CompressionBackend::DELTA_DELTA,
        CompressionBackend::BIT_PACKING,
        CompressionBackend::DICTIONARY
    };
}

bool CompressionFactory::isBackendAvailable(CompressionBackend backend) {
    // 简化实现 - 所有后端都视为可用
    return true;
}

bool CompressionFactory::detectSortedData(const std::vector<int64_t>& values) {
    return std::is_sorted(values.begin(), values.end());
}

bool CompressionFactory::detectTimestamp(const std::vector<int64_t>& values) {
    if (values.size() < 2) return false;
    
    // 检查是否类似时间戳（递增且差值相对稳定）
    bool is_increasing = true;
    std::vector<int64_t> deltas;
    deltas.reserve(values.size() - 1);
    
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] < values[i-1]) {
            is_increasing = false;
            break;
        }
        deltas.push_back(values[i] - values[i-1]);
    }
    
    if (!is_increasing) return false;
    
    // 检查差值的方差是否相对较小（表示时间间隔相对稳定）
    double variance = calculateDeltaVariance(values);
    return variance < 1000000; // 阈值需要根据实际情况调整
}

double CompressionFactory::calculateDeltaVariance(const std::vector<int64_t>& values) {
    if (values.size() < 2) return 0.0;
    
    std::vector<int64_t> deltas;
    deltas.reserve(values.size() - 1);
    
    for (size_t i = 1; i < values.size(); ++i) {
        deltas.push_back(values[i] - values[i-1]);
    }
    
    // 计算平均值
    double mean = 0.0;
    for (int64_t delta : deltas) {
        mean += delta;
    }
    mean /= deltas.size();
    
    // 计算方差
    double variance = 0.0;
    for (int64_t delta : deltas) {
        variance += (delta - mean) * (delta - mean);
    }
    variance /= deltas.size();
    
    return variance;
}

CompressionBackend CompressionFactory::selectForNumericData(const NumericDataCharacteristics& characteristics) {
    if (characteristics.is_sorted && characteristics.has_small_deltas) {
        return CompressionBackend::DELTA_VARINT;
    } else if (characteristics.is_timestamp_like) {
        return CompressionBackend::DELTA_VARINT;
    } else {
        return CompressionBackend::ZSTD;
    }
}

CompressionBackend CompressionFactory::selectForBooleanData(double sparsity, size_t data_size) {
    // 布尔值数据通常使用BitPacking压缩
    return CompressionBackend::BIT_PACKING;
}

CompressionBackend CompressionFactory::selectForStringData(const StringDataCharacteristics& characteristics) {
    if (characteristics.is_highly_repetitive || characteristics.unique_ratio < 0.5) {
        return CompressionBackend::DICTIONARY;
    } else {
        return CompressionBackend::ZSTD;
    }
}

CompressionBackend CompressionFactory::selectForGenericData(const std::vector<uint8_t>& data) {
    // 通用数据使用ZSTD
    return CompressionBackend::ZSTD;
}

CompressionBackend CompressionFactory::selectForTimestampData(const std::vector<uint8_t>& data) {
    // For timestamp data, we use DELTA_VARINT compression to enable efficient random access
    return CompressionBackend::DELTA_VARINT;
}

CompressionBackend CompressionFactory::selectForLogtypeData(const std::vector<uint8_t>& data) {
    // For logtype data, we use DELTA_VARINT compression to enable efficient random access
    return CompressionBackend::DELTA_VARINT;
}

bool CompressionFactory::hasCommonPrefixes(const std::vector<std::string>& strings) {
    if (strings.size() < 2) return false;
    
    // 简化实现 - 检查前几个字符串是否有公共前缀
    size_t min_len = std::min(strings[0].length(), strings[1].length());
    for (size_t i = 0; i < min_len; ++i) {
        if (strings[0][i] != strings[1][i]) {
            return i > 3; // 如果前缀长度大于3，则认为有公共前缀
        }
    }
    return min_len > 3;
}

} // namespace factory
} // namespace compression
} // namespace json2