#include "dictionary_compression.h"
#include "../core/compression_utils.h"
#include "../backends/zstd_backend.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <climits>

namespace json2 {
namespace compression {
namespace algorithms {

std::vector<uint8_t> DictionaryCompression::compressStrings(const std::vector<std::string>& strings) {
    if (strings.empty()) return {};
    
    // 分析数据特征，选择最优压缩策略
    DictionaryStats stats = analyzeStrings(strings);
    
    if (!stats.is_worth_compressing) {
        // 直接序列化，不压缩
        return serializeStrings(strings);
    }
    
    // 根据数据特征选择压缩方式
    if (stats.unique_ratio > 0.9) {
        // 唯一字符串过多，使用外部压缩库
        Backend backend = selectOptimalBackend(strings);
        CompressionBackend compression_backend;
        switch (backend) {
            case Backend::ZSTD:
                compression_backend = CompressionBackend::ZSTD;
                break;
            case Backend::BROTLI:
                compression_backend = CompressionBackend::BROTLI;
                break;
            default:
                compression_backend = CompressionBackend::ZSTD;
                break;
        }
        return compressWithBackend(strings, compression_backend);
    } else {
        // 使用字典压缩
        return compressWithDictionary(strings);
    }
}

std::vector<std::string> DictionaryCompression::decompressStrings(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    size_t pos = 0;
    
    // 读取压缩类型标志
    uint8_t compression_type = utils::SerializationUtils::readValue<uint8_t>(compressed, pos);
    
    switch (compression_type) {
        case COMPRESSION_NONE:
            return deserializeStrings(compressed, pos);
        case COMPRESSION_DICTIONARY:
            return decompressFromDictionary(compressed, pos);
        case COMPRESSION_BACKEND:
            return decompressFromBackend(compressed, pos);
        default:
            throw std::runtime_error("Unknown compression type in dictionary decompression");
    }
}

std::vector<uint8_t> DictionaryCompression::compressWithDictionary(const std::vector<std::string>& strings) {
    std::vector<uint8_t> result;
    
    // 压缩类型标志
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(COMPRESSION_DICTIONARY));
    
    // 构建字典
    std::unordered_map<std::string, uint32_t> dictionary;
    std::vector<std::string> unique_strings;
    uint32_t dict_index = 0;
    
    for (const auto& str : strings) {
        if (dictionary.find(str) == dictionary.end()) {
            dictionary[str] = dict_index++;
            unique_strings.push_back(str);
        }
    }
    
    // 序列化字典
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(unique_strings.size()));
    for (const auto& str : unique_strings) {
        utils::SerializationUtils::writeString(result, str);
    }
    
    // 序列化索引序列
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(strings.size()));
    for (const auto& str : strings) {
        utils::SerializationUtils::writeValue(result, dictionary[str]);
    }
    
    return result;
}

std::vector<std::string> DictionaryCompression::decompressFromDictionary(const std::vector<uint8_t>& compressed, size_t& pos) {
    // 读取字典
    uint32_t dict_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<std::string> dictionary(dict_size);
    
    for (uint32_t i = 0; i < dict_size; ++i) {
        dictionary[i] = utils::SerializationUtils::readString(compressed, pos);
    }
    
    // 读取索引序列并重建字符串
    uint32_t string_count = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<std::string> result(string_count);
    
    for (uint32_t i = 0; i < string_count; ++i) {
        uint32_t index = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
        result[i] = dictionary[index];
    }
    
    return result;
}

std::vector<uint8_t> DictionaryCompression::compressWithBackend(const std::vector<std::string>& strings, CompressionBackend backend) {
    std::vector<uint8_t> result;
    
    // 压缩类型标志
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(COMPRESSION_BACKEND));
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(backend));
    
    // 序列化字符串
    std::vector<uint8_t> serialized = serializeStrings(strings);
    
    // 使用后端压缩
    switch (backend) {
        case CompressionBackend::ZSTD: {
            backends::ZstdBackend zstd;
            std::vector<uint8_t> compressed = zstd.compress(serialized);
            utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed.size()));
            result.insert(result.end(), compressed.begin(), compressed.end());
            break;
        }
        default:
            // 其他后端暂未实现，回退到直接序列化
            utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(serialized.size()));
            result.insert(result.end(), serialized.begin(), serialized.end());
    }
    
    return result;
}

std::vector<std::string> DictionaryCompression::decompressFromBackend(const std::vector<uint8_t>& compressed, size_t& pos) {
    // 读取后端类型
    CompressionBackend backend = static_cast<CompressionBackend>(
        utils::SerializationUtils::readValue<uint8_t>(compressed, pos));
    
    // 读取压缩数据长度
    uint32_t compressed_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    
    // 提取压缩数据
    std::vector<uint8_t> compressed_data(compressed.begin() + pos, 
                                       compressed.begin() + pos + compressed_size);
    pos += compressed_size;
    
    // 解压
    std::vector<uint8_t> decompressed;
    switch (backend) {
        case CompressionBackend::ZSTD: {
            backends::ZstdBackend zstd;
            decompressed = zstd.decompress(compressed_data);
            break;
        }
        default:
            decompressed = compressed_data; // 回退情况
    }
    
    // 反序列化字符串
    size_t deserialize_pos = 0;
    return deserializeStrings(decompressed, deserialize_pos);
}

std::vector<uint8_t> DictionaryCompression::serializeStrings(const std::vector<std::string>& strings) {
    std::vector<uint8_t> result;
    
    // 压缩类型标志
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(COMPRESSION_NONE));
    
    // 序列化字符串向量
    utils::SerializationUtils::writeStringVector(result, strings);
    
    return result;
}

std::vector<std::string> DictionaryCompression::deserializeStrings(const std::vector<uint8_t>& data, size_t& pos) {
    return utils::SerializationUtils::readStringVector(data, pos);
}

DictionaryCompression::Backend DictionaryCompression::selectOptimalBackend(const std::vector<std::string>& strings) {
    // 简单的后端选择策略，后续可以根据实际性能测试调整
    size_t total_size = 0;
    for (const auto& str : strings) {
        total_size += str.size();
    }
    
    if (total_size > 10000) {
        return Backend::ZSTD; // 大数据集使用ZSTD
    } else {
        return Backend::ZSTD; // 默认使用ZSTD
    }
}

double DictionaryCompression::calculateUniqueRatio(const std::vector<std::string>& strings) {
    if (strings.empty()) return 1.0;
    
    std::unordered_set<std::string> unique_strings(strings.begin(), strings.end());
    return static_cast<double>(unique_strings.size()) / strings.size();
}

double DictionaryCompression::estimateCompressionRatio(const std::vector<std::string>& strings) {
    if (strings.empty()) return 1.0;
    
    DictionaryStats stats = analyzeStrings(strings);
    
    if (stats.unique_ratio > 0.9) {
        // 唯一字符串太多，字典压缩效果不佳
        return 0.8; // 外部压缩库的估计压缩率
    }
    
    // 字典压缩的估计：字典大小 + 索引序列大小
    size_t dict_size = stats.unique_count * stats.avg_string_length;
    size_t index_size = strings.size() * sizeof(uint32_t);
    size_t total_estimated = dict_size + index_size;
    
    return static_cast<double>(total_estimated) / stats.total_size;
}

DictionaryCompression::DictionaryStats DictionaryCompression::analyzeStrings(const std::vector<std::string>& strings) {
    DictionaryStats stats = {};
    
    if (strings.empty()) {
        stats.is_worth_compressing = false;
        return stats;
    }
    
    // 基本统计
    std::unordered_set<std::string> unique_strings(strings.begin(), strings.end());
    stats.unique_count = unique_strings.size();
    stats.total_count = strings.size();
    stats.unique_ratio = static_cast<double>(stats.unique_count) / stats.total_count;
    
    // 计算总大小和平均长度
    stats.total_size = 0;
    stats.min_length = SIZE_MAX;
    stats.max_length = 0;
    
    for (const auto& str : strings) {
        size_t len = str.size();
        stats.total_size += len;
        stats.min_length = std::min(stats.min_length, len);
        stats.max_length = std::max(stats.max_length, len);
    }
    
    stats.avg_string_length = static_cast<double>(stats.total_size) / stats.total_count;
    
    // 计算估计压缩比
    stats.estimated_ratio = estimateCompressionRatio(strings);
    
    // 判断是否值得压缩
    stats.is_worth_compressing = (stats.estimated_ratio < 0.9) && 
                               (stats.total_size > 100) && 
                               (stats.unique_ratio < 0.95);
    
    return stats;
}

// ========== ICompression Interface Implementation ==========

std::vector<uint8_t> DictionaryCompression::compress(const std::vector<uint8_t>& data, int level) {
    // Convert bytes to string format and compress using dictionary compression
    if (data.empty()) return {};
    
    try {
        // Convert byte data to strings for dictionary compression
        // For simplicity, treat each sequence of printable bytes as a "string"
        std::vector<std::string> strings;
        
        // Split data into chunks that can be treated as strings
        std::string current_string;
        for (uint8_t byte : data) {
            if (byte >= 32 && byte <= 126) { // Printable ASCII
                current_string += static_cast<char>(byte);
            } else {
                if (!current_string.empty()) {
                    strings.push_back(current_string);
                    current_string.clear();
                }
                // Handle non-printable bytes as hex strings
                strings.push_back("\\x" + std::to_string(byte));
            }
        }
        
        if (!current_string.empty()) {
            strings.push_back(current_string);
        }
        
        return compressStrings(strings);
    } catch (const std::exception& e) {
        throw std::runtime_error("Dictionary compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> DictionaryCompression::decompress(const std::vector<uint8_t>& compressed_data) {
    // Decompress dictionary-compressed data and convert back to bytes
    if (compressed_data.empty()) return {};
    
    try {
        std::vector<std::string> strings = decompressStrings(compressed_data);
        
        // Convert strings back to byte data
        std::vector<uint8_t> result;
        for (const std::string& str : strings) {
            if (str.length() >= 3 && str.substr(0, 2) == "\\x") {
                // Handle hex-encoded bytes
                try {
                    int byte_val = std::stoi(str.substr(2));
                    if (byte_val >= 0 && byte_val <= 255) {
                        result.push_back(static_cast<uint8_t>(byte_val));
                    }
                } catch (const std::exception&) {
                    // If hex parsing fails, treat as regular string
                    for (char c : str) {
                        result.push_back(static_cast<uint8_t>(c));
                    }
                }
            } else {
                // Regular string to bytes
                for (char c : str) {
                    result.push_back(static_cast<uint8_t>(c));
                }
            }
        }
        
        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error("Dictionary decompression failed: " + std::string(e.what()));
    }
}

std::string DictionaryCompression::getName() const {
    return "Dictionary";
}

bool DictionaryCompression::supportsLevel(int level) const {
    return (level >= 0 && level <= 9);  // Dictionary compression doesn't use levels, but accept reasonable range
}

} // namespace algorithms
} // namespace compression
} // namespace json2