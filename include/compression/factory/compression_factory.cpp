#include "compression_factory.h"
#include "../type_aware/type_aware_compressor.h"
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

std::unique_ptr<type_aware::TypeAwareCompressor> CompressionFactory::createTypeAwareCompressor(const TypeAwareCompressionConfig& config) {
    return std::make_unique<type_aware::TypeAwareCompressor>(config);
}

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
            benchmark.compression_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
            // 计算压缩比
            benchmark.compression_ratio = static_cast<double>(compressed.size()) / data.size();
            
            // 测试解压时间
            start = std::chrono::high_resolution_clock::now();
            std::vector<uint8_t> decompressed = compressor->decompress(compressed);
            end = std::chrono::high_resolution_clock::now();
            benchmark.decompression_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
        } catch (const std::exception&) {
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
        CompressionBackend::RLE
    };
}

bool CompressionFactory::isBackendAvailable(CompressionBackend backend) {
    switch (backend) {
        case CompressionBackend::ZSTD:
            return backends::ZstdBackend::isAvailable();
        case CompressionBackend::BROTLI:
            return backends::BrotliBackend::isAvailable();
        case CompressionBackend::LZMA:
            return backends::LzmaBackend::isAvailable();
        case CompressionBackend::LZ4:
            return backends::Lz4Backend::isAvailable();
        case CompressionBackend::SNAPPY:
            return backends::SnappyBackend::isAvailable();
        case CompressionBackend::RLE:
            return true; // RLE总是可用
        default:
            return false;
    }
}

CompressionBackend CompressionFactory::selectForNumericData(const NumericDataCharacteristics& characteristics) {
    if (characteristics.is_sorted && characteristics.has_small_deltas) {
        // 有序小差值数据，不需要外部压缩
        return CompressionBackend::RLE; // 作为标识，实际会使用Delta+Varint
    } else if (characteristics.data_size > 10000) {
        // 大数据集使用高压缩率算法
        return CompressionBackend::ZSTD;
    } else {
        // 中小数据集使用快速算法
        return CompressionBackend::LZ4;
    }
}

CompressionBackend CompressionFactory::selectForStringData(const StringDataCharacteristics& characteristics) {
    if (characteristics.unique_ratio < 0.5) {
        // 重复度高，字典压缩效果好
        return CompressionBackend::ZSTD; // 后端压缩
    } else if (characteristics.total_size > 10000) {
        // 大字符串集合
        return CompressionBackend::BROTLI;
    } else {
        // 小字符串集合
        return CompressionBackend::LZ4;
    }
}

CompressionBackend CompressionFactory::selectForBooleanData(double sparsity, size_t data_size) {
    // 布尔值数据通常使用BitPacking，不需要外部压缩后端
    return CompressionBackend::RLE; // 作为标识
}

CompressionBackend CompressionFactory::selectForGenericData(const std::vector<uint8_t>& data) {
    if (data.size() > 50000) {
        return CompressionBackend::ZSTD; // 大数据
    } else if (data.size() > 10000) {
        return CompressionBackend::BROTLI; // 中等数据
    } else {
        return CompressionBackend::LZ4; // 小数据
    }
}

bool CompressionFactory::detectTimestamp(const std::vector<int64_t>& values) {
    if (values.size() < 2) return false;
    
    // 简单的时间戳检测：值都在合理的时间戳范围内，且单调递增
    const int64_t MIN_TIMESTAMP = 1000000000; // 2001年左右
    const int64_t MAX_TIMESTAMP = 4000000000; // 2096年左右
    
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] < MIN_TIMESTAMP || values[i] > MAX_TIMESTAMP) {
            return false;
        }
        if (i > 0 && values[i] < values[i-1]) {
            return false; // 不是单调递增
        }
    }
    
    return true;
}

bool CompressionFactory::detectSortedData(const std::vector<int64_t>& values) {
    return std::is_sorted(values.begin(), values.end());
}

double CompressionFactory::calculateDeltaVariance(const std::vector<int64_t>& values) {
    if (values.size() < 2) return 0.0;
    
    std::vector<int64_t> deltas;
    for (size_t i = 1; i < values.size(); ++i) {
        deltas.push_back(values[i] - values[i-1]);
    }
    
    // 计算方差
    int64_t sum = 0;
    for (int64_t delta : deltas) {
        sum += delta;
    }
    double mean = static_cast<double>(sum) / deltas.size();
    
    double variance = 0.0;
    for (int64_t delta : deltas) {
        double diff = delta - mean;
        variance += diff * diff;
    }
    
    return variance / deltas.size();
}

bool CompressionFactory::hasCommonPrefixes(const std::vector<std::string>& strings) {
    if (strings.size() < 2) return false;
    
    // 简单检测：看是否有多个字符串共享前缀
    std::unordered_map<std::string, int> prefix_count;
    
    for (const auto& str : strings) {
        if (str.length() >= 3) {
            std::string prefix = str.substr(0, 3);
            prefix_count[prefix]++;
        }
    }
    
    // 如果有前缀出现次数超过总数的30%，认为有公共前缀
    size_t threshold = strings.size() * 0.3;
    for (const auto& pair : prefix_count) {
        if (pair.second >= threshold) {
            return true;
        }
    }
    
    return false;
}

double CompressionFactory::estimateCompressionRatio(CompressionBackend backend, const std::vector<uint8_t>& data) {
    // 基于经验的压缩率估算
    switch (backend) {
        case CompressionBackend::ZSTD:
            return 0.6;
        case CompressionBackend::BROTLI:
            return 0.55;
        case CompressionBackend::LZMA:
            return 0.5;
        case CompressionBackend::LZ4:
            return 0.7;
        case CompressionBackend::SNAPPY:
            return 0.75;
        case CompressionBackend::RLE:
            return algorithms::RLECompression::estimateCompressionRatio(data);
        default:
            return 1.0;
    }
}

size_t CompressionFactory::estimateCompressionTime(CompressionBackend backend, size_t data_size) {
    // 基于经验的压缩时间估算（微秒）
    switch (backend) {
        case CompressionBackend::ZSTD:
            return data_size / 1000; // 1MB/s
        case CompressionBackend::BROTLI:
            return data_size / 500;  // 500KB/s
        case CompressionBackend::LZMA:
            return data_size / 100;  // 100KB/s
        case CompressionBackend::LZ4:
            return data_size / 10000; // 10MB/s
        case CompressionBackend::SNAPPY:
            return data_size / 50000; // 50MB/s
        case CompressionBackend::RLE:
            return data_size / 5000;  // 5MB/s
        default:
            return data_size / 1000;
    }
}

} // namespace factory
} // namespace compression
} // namespace json2