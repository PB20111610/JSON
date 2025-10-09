#include "dictionary_compression.h"
#include "../core/compression_utils.h"
#include "../backends/zstd_backend.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <climits>
#include <stdexcept>
#include <iostream> // for debug output

namespace json2 {
namespace compression {
namespace algorithms {

// ========== Helper Functions ==========

void DictionaryCompression::writeUint32(std::vector<uint8_t>& data, uint32_t value) {
    utils::SerializationUtils::writeValue(data, value);
}

uint32_t DictionaryCompression::readUint32(const std::vector<uint8_t>& data, size_t& pos) {
    // Bounds checking to prevent segmentation faults
    if (pos + sizeof(uint32_t) > data.size()) {
        throw std::runtime_error("DictionaryCompression::readUint32: Attempt to read beyond data bounds, pos: " + 
                               std::to_string(pos) + ", size: " + std::to_string(data.size()));
    }
    return utils::SerializationUtils::readValue<uint32_t>(data, pos);
}

void DictionaryCompression::writeString(std::vector<uint8_t>& data, const std::string& str) {
    utils::SerializationUtils::writeString(data, str);
}

std::string DictionaryCompression::readString(const std::vector<uint8_t>& data, size_t& pos) {
    // Bounds checking is handled by SerializationUtils::readString
    return utils::SerializationUtils::readString(data, pos);
}

std::vector<uint8_t> DictionaryCompression::compressIndices(const std::vector<uint32_t>& indices, Backend backend) {
    std::vector<uint8_t> result;
    
    switch (backend) {
        case Backend::NONE:
            // No additional compression, just serialize
            for (uint32_t index : indices) {
                writeUint32(result, index);
            }
            break;
            
        case Backend::RLE: {
            // RLE compression of indices
            if (!indices.empty()) {
                uint32_t current_value = indices[0];
                uint8_t run_length = 1;
                
                for (size_t i = 1; i < indices.size(); ++i) {
                    if (indices[i] == current_value && run_length < 255) {
                        ++run_length;
                    } else {
                        // Write current run
                        writeUint32(result, current_value);
                        result.push_back(run_length);
                        current_value = indices[i];
                        run_length = 1;
                    }
                }
                
                // Write last run
                writeUint32(result, current_value);
                result.push_back(run_length);
            }
            break;
        }
        
        case Backend::VARINT: {
            // Varint compression of indices
            for (uint32_t index : indices) {
                // Simple varint encoding for 32-bit values
                while (index >= 0x80) {
                    result.push_back(static_cast<uint8_t>(index & 0x7F) | 0x80);
                    index >>= 7;
                }
                result.push_back(static_cast<uint8_t>(index & 0x7F));
            }
            break;
        }
        
        default:
            // For other backends, just serialize
            for (uint32_t index : indices) {
                writeUint32(result, index);
            }
            break;
    }
    
    return result;
}

std::vector<uint32_t> DictionaryCompression::decompressIndices(const std::vector<uint8_t>& compressed, 
                                                              size_t expected_count, Backend backend) {
    std::vector<uint32_t> result;
    result.reserve(expected_count > 0 ? expected_count : compressed.size() / 4);
    
    size_t pos = 0;
    
    switch (backend) {
        case Backend::NONE: {
            // No additional compression, just deserialize
            while (pos + sizeof(uint32_t) <= compressed.size()) {
                uint32_t value = readUint32(compressed, pos);
                result.push_back(value);
            }
            break;
        }
        
        case Backend::RLE: {
            // RLE decompression of indices
            while (pos + sizeof(uint32_t) + 1 <= compressed.size()) {
                uint32_t value = readUint32(compressed, pos);
                uint8_t count = compressed[pos++];
                
                for (uint8_t i = 0; i < count; ++i) {
                    result.push_back(value);
                }
            }
            break;
        }
        
        case Backend::VARINT: {
            // Varint decompression of indices
            while (pos < compressed.size()) {
                uint32_t value = 0;
                uint8_t shift = 0;
                uint8_t byte;
                
                do {
                    if (pos >= compressed.size()) {
                        throw std::runtime_error("Dictionary: Insufficient data for varint indices");
                    }
                    byte = compressed[pos++];
                    value |= static_cast<uint32_t>(byte & 0x7F) << shift;
                    shift += 7;
                    
                    // Prevent infinite loop and overflow
                    if (shift > 32) {
                        throw std::runtime_error("Dictionary: Varint overflow in decompressIndices");
                    }
                } while (byte & 0x80);
                
                result.push_back(value);
            }
            break;
        }
        
        default: {
            // For other backends, just deserialize
            while (pos + sizeof(uint32_t) <= compressed.size()) {
                uint32_t value = readUint32(compressed, pos);
                result.push_back(value);
            }
            break;
        }
    }
    
    return result;
}

// Static method implementation
std::vector<uint8_t> DictionaryCompression::dictionaryCompress(const std::vector<std::string>& strings, 
                                                              Backend backend) {
    DictionaryCompression compressor;
    return compressor.compressStrings(strings);
}

std::vector<std::string> DictionaryCompression::dictionaryDecompress(const std::vector<uint8_t>& compressed, 
                                                                    Backend backend) {
    DictionaryCompression compressor;
    return compressor.decompressStrings(compressed);
}

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
    std::cout << "DEBUG: compressWithDictionary called with " << strings.size() << " strings" << std::endl;
    
    std::vector<uint8_t> result;
    
    // 压缩类型标志
    std::cout << "DEBUG: Writing compression type and backend" << std::endl;
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(COMPRESSION_DICTIONARY));
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(Backend::NONE));
    
    // 构建字典
    std::cout << "DEBUG: Building dictionary..." << std::endl;
    std::unordered_map<std::string, uint32_t> dictionary;
    std::vector<std::string> unique_strings;
    uint32_t dict_index = 0;
    
    for (const auto& str : strings) {
        if (dictionary.find(str) == dictionary.end()) {
            dictionary[str] = dict_index++;
            unique_strings.push_back(str);
        }
    }
    
    std::cout << "DEBUG: Dictionary built with " << unique_strings.size() << " unique strings" << std::endl;
    
    // 序列化字典
    std::cout << "DEBUG: Serializing dictionary..." << std::endl;
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(unique_strings.size()));
    for (const auto& str : unique_strings) {
        utils::SerializationUtils::writeString(result, str);
    }
    
    // 序列化索引序列
    std::cout << "DEBUG: Serializing indices..." << std::endl;
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(strings.size()));
    for (const auto& str : strings) {
        // Add bounds checking
        auto it = dictionary.find(str);
        if (it == dictionary.end()) {
            throw std::runtime_error("Dictionary: String not found in dictionary during compression");
        }
        utils::SerializationUtils::writeValue(result, it->second);
    }
    
    std::cout << "DEBUG: compressWithDictionary complete, result size: " << result.size() << std::endl;
    
    return result;
}

std::vector<std::string> DictionaryCompression::decompressFromDictionary(const std::vector<uint8_t>& compressed, size_t& pos) {
    // 读取字典
    uint32_t dict_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<std::string> dictionary(dict_size);
    
    for (uint32_t i = 0; i < dict_size; ++i) {
        // Add bounds checking
        if (pos >= compressed.size()) {
            throw std::runtime_error("Dictionary: Insufficient data for dictionary entry " + std::to_string(i));
        }
        dictionary[i] = utils::SerializationUtils::readString(compressed, pos);
    }
    
    // 读取索引序列并重建字符串
    uint32_t string_count = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<std::string> result(string_count);
    
    for (uint32_t i = 0; i < string_count; ++i) {
        // Add bounds checking
        if (pos >= compressed.size()) {
            throw std::runtime_error("Dictionary: Insufficient data for index " + std::to_string(i));
        }
        uint32_t index = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
        if (index >= dictionary.size()) {
            throw std::runtime_error("Dictionary: Invalid index " + std::to_string(index) + " for dictionary size " + std::to_string(dictionary.size()));
        }
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
    
    // Calculate stats directly without calling analyzeStrings to avoid recursion
    std::unordered_set<std::string> unique_strings(strings.begin(), strings.end());
    size_t unique_count = unique_strings.size();
    size_t total_count = strings.size();
    double unique_ratio = static_cast<double>(unique_count) / total_count;
    
    if (unique_ratio > 0.9) {
        // 唯一字符串太多，字典压缩效果不佳
        return 0.8; // 外部压缩库的估计压缩率
    }
    
    // Calculate total size and average length
    size_t total_size = 0;
    for (const auto& str : strings) {
        total_size += str.size();
    }
    
    double avg_string_length = static_cast<double>(total_size) / total_count;
    
    // 字典压缩的估计：字典大小 + 索引序列大小
    size_t dict_size = unique_count * avg_string_length;
    size_t index_size = strings.size() * sizeof(uint32_t);
    size_t total_estimated = dict_size + index_size;
    
    // Avoid division by zero
    if (total_size == 0) return 1.0;
    
    return static_cast<double>(total_estimated) / total_size;
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

// 部分解压指定索引的值
std::string DictionaryCompression::decompressStringAt(const std::vector<uint8_t>& compressed, size_t index) {
    if (compressed.empty()) {
        throw std::runtime_error("Dictionary: Empty compressed data");
    }
    
    std::cout << "DEBUG: decompressStringAt called with index " << index << ", compressed size: " << compressed.size() << std::endl;
    
    try {
        size_t pos = 0;
        // Read compression type correctly as a single byte
        if (pos >= compressed.size()) {
            throw std::runtime_error("Dictionary: Insufficient data for compression type");
        }
        uint8_t compression_type = compressed[pos++];
        
        std::cout << "DEBUG: compression_type = " << (int)compression_type << std::endl;
        
        if (compression_type == COMPRESSION_NONE) {
            std::cout << "DEBUG: Processing COMPRESSION_NONE" << std::endl;
            // 未压缩数据
            auto strings = deserializeStrings(compressed, pos);
            if (index >= strings.size()) {
                throw std::out_of_range("Dictionary: Index out of range");
            }
            return strings[index];
        } else if (compression_type == COMPRESSION_DICTIONARY) {
            std::cout << "DEBUG: Processing COMPRESSION_DICTIONARY" << std::endl;
            // 字典压缩数据
            if (pos >= compressed.size()) {
                throw std::runtime_error("Dictionary: Insufficient data for backend type");
            }
            Backend backend = static_cast<Backend>(compressed[pos++]);
            std::cout << "DEBUG: backend = " << (int)backend << std::endl;
            return dictionaryDecompressAt(compressed, index, backend);
        } else if (compression_type == COMPRESSION_BACKEND) {
            std::cout << "DEBUG: Processing COMPRESSION_BACKEND" << std::endl;
            // Backend compressed data
            throw std::runtime_error("Dictionary: Backend compression not supported for partial decompression");
        } else {
            throw std::runtime_error("Dictionary: Invalid compression type: " + std::to_string(compression_type));
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Dictionary partial decompression failed: " + std::string(e.what()));
    }
}

// 部分解压指定索引的值
std::string DictionaryCompression::dictionaryDecompressAt(const std::vector<uint8_t>& compressed, size_t index,
                                                        Backend backend) {
    std::cout << "DEBUG: dictionaryDecompressAt called with index " << index << ", backend " << (int)backend << ", compressed size: " << compressed.size() << std::endl;
                                                        
    if (compressed.size() < 10) { // Minimum header size (1 byte type + 1 byte backend + 4 bytes dict size + 4 bytes count)
        throw std::runtime_error("Dictionary: Invalid compressed data - insufficient header, size: " + std::to_string(compressed.size()));
    }
    
    try {
        size_t pos = 0;
        
        // Skip compression type and backend type (already read by caller)
        pos += 2;
        
        std::cout << "DEBUG: pos after skipping headers = " << pos << std::endl;
        
        // Read dictionary size
        if (pos + sizeof(uint32_t) > compressed.size()) {
            throw std::runtime_error("Dictionary: Insufficient data for dictionary size, pos: " + std::to_string(pos) + ", size: " + std::to_string(compressed.size()));
        }
        uint32_t dict_size = readUint32(compressed, pos);
        
        std::cout << "DEBUG: dict_size = " << dict_size << ", pos after reading dict_size = " << pos << std::endl;
        
        // Validate dictionary size
        if (dict_size == 0) {
            throw std::runtime_error("Dictionary: Invalid dictionary size: " + std::to_string(dict_size));
        }
        
        // Read dictionary
        std::vector<std::string> dictionary;
        dictionary.reserve(dict_size);
        
        std::cout << "DEBUG: Reading dictionary entries..." << std::endl;
        
        for (uint32_t i = 0; i < dict_size; ++i) {
            if (pos >= compressed.size()) {
                throw std::runtime_error("Dictionary: Insufficient data for dictionary entry " + std::to_string(i) + ", pos: " + std::to_string(pos) + ", size: " + std::to_string(compressed.size()));
            }
            std::string entry = readString(compressed, pos);
            dictionary.push_back(entry);
            std::cout << "DEBUG: Dictionary entry " << i << " = \"" << entry << "\", pos = " << pos << std::endl;
        }
        
        // Read total count
        if (pos + sizeof(uint32_t) > compressed.size()) {
            throw std::runtime_error("Dictionary: Insufficient data for total count, pos: " + std::to_string(pos) + ", size: " + std::to_string(compressed.size()));
        }
        uint32_t total_count = readUint32(compressed, pos);
        
        std::cout << "DEBUG: total_count = " << total_count << ", pos after reading total_count = " << pos << std::endl;
        
        // Validate index
        if (index >= total_count) {
            throw std::out_of_range("Dictionary: Index out of range, index: " + std::to_string(index) + ", count: " + std::to_string(total_count));
        }
        
        std::cout << "DEBUG: Calling partialDecompressHelper with index " << index << std::endl;
        
        return partialDecompressHelper(compressed, index, pos, dictionary, backend);
    } catch (const std::exception& e) {
        throw std::runtime_error("Dictionary partial decompression failed: " + std::string(e.what()));
    }
}

// 部分解压辅助函数
std::string DictionaryCompression::partialDecompressHelper(const std::vector<uint8_t>& compressed, size_t index,
                                                          size_t& pos, const std::vector<std::string>& dictionary,
                                                          Backend backend) {
    std::cout << "DEBUG: partialDecompressHelper called with index " << index << ", pos " << pos << ", dictionary size " << dictionary.size() << ", backend " << (int)backend << std::endl;
                                                          
    // Make sure we have enough data
    if (pos >= compressed.size()) {
        throw std::runtime_error("Dictionary: Insufficient data for indices, pos: " + std::to_string(pos) + ", size: " + std::to_string(compressed.size()));
    }
    
    // Extract indices data - this should be from pos to end of compressed data
    std::vector<uint8_t> indices_data(compressed.begin() + pos, compressed.end());
    
    std::cout << "DEBUG: indices_data size = " << indices_data.size() << std::endl;
    
    if (backend == Backend::NONE) {
        std::cout << "DEBUG: Processing Backend::NONE" << std::endl;
        // No additional compression on indices
        size_t indices_pos = 0;
        for (size_t i = 0; i <= index; ++i) {
            std::cout << "DEBUG: Processing index " << i << ", indices_pos = " << indices_pos << std::endl;
            if (indices_pos + sizeof(uint32_t) > indices_data.size()) {
                throw std::runtime_error("Dictionary: Insufficient data for indices at position " + std::to_string(i) + 
                                       ", indices_pos: " + std::to_string(indices_pos) + 
                                       ", indices_size: " + std::to_string(indices_data.size()));
            }
            uint32_t value = readUint32(indices_data, indices_pos);
            std::cout << "DEBUG: Read value " << value << " at index " << i << std::endl;
            if (i == index) {
                if (value >= dictionary.size()) {
                    throw std::runtime_error("Dictionary: Invalid dictionary index " + std::to_string(value) + 
                                           " (dictionary size: " + std::to_string(dictionary.size()) + ")");
                }
                std::cout << "DEBUG: Returning dictionary[" << value << "] = \"" << dictionary[value] << "\"" << std::endl;
                return dictionary[value];
            }
        }
    } else if (backend == Backend::RLE) {
        std::cout << "DEBUG: Processing Backend::RLE" << std::endl;
        // RLE compressed indices
        size_t current_index = 0;
        size_t indices_pos = 0;
        
        while (indices_pos + sizeof(uint32_t) + 1 <= indices_data.size() && current_index <= index) {
            uint32_t value = readUint32(indices_data, indices_pos);
            if (indices_pos >= indices_data.size()) {
                throw std::runtime_error("Dictionary: Insufficient data for RLE count");
            }
            uint8_t count = indices_data[indices_pos++];
            
            if (value >= dictionary.size()) {
                throw std::runtime_error("Dictionary: Invalid dictionary index in RLE: " + std::to_string(value) + 
                                       " (dictionary size: " + std::to_string(dictionary.size()) + ")");
            }
            
            std::cout << "DEBUG: RLE value " << value << ", count " << (int)count << ", current_index " << current_index << std::endl;
            
            // Check if target index is in this run
            if (index >= current_index && index < current_index + count) {
                std::cout << "DEBUG: Found target in RLE run, returning dictionary[" << value << "] = \"" << dictionary[value] << "\"" << std::endl;
                return dictionary[value];
            }
            
            current_index += count;
        }
    } else if (backend == Backend::VARINT) {
        std::cout << "DEBUG: Processing Backend::VARINT" << std::endl;
        // Varint compressed indices
        size_t current_index = 0;
        size_t indices_pos = 0;
        
        while (indices_pos < indices_data.size() && current_index <= index) {
            // Check if we have enough data for at least one byte
            if (indices_pos >= indices_data.size()) {
                throw std::runtime_error("Dictionary: Insufficient data for varint indices at position " + std::to_string(current_index));
            }
            
            // Decode varint
            uint32_t value = 0;
            uint8_t shift = 0;
            uint8_t byte;
            
            do {
                if (indices_pos >= indices_data.size()) {
                    throw std::runtime_error("Dictionary: Insufficient data for varint indices");
                }
                byte = indices_data[indices_pos++];
                value |= static_cast<uint32_t>(byte & 0x7F) << shift;
                shift += 7;
                
                // Prevent infinite loop and overflow
                if (shift > 32) {
                    throw std::runtime_error("Dictionary: Varint overflow");
                }
            } while (byte & 0x80);
            
            if (value >= dictionary.size()) {
                throw std::runtime_error("Dictionary: Invalid dictionary index in varint: " + std::to_string(value) + 
                                       " (dictionary size: " + std::to_string(dictionary.size()) + ")");
            }
            
            std::cout << "DEBUG: Varint value " << value << " at current_index " << current_index << std::endl;
            
            if (current_index == index) {
                std::cout << "DEBUG: Found target in varint, returning dictionary[" << value << "] = \"" << dictionary[value] << "\"" << std::endl;
                return dictionary[value];
            }
            
            current_index++;
        }
    } else {
        std::cout << "DEBUG: Processing other backend" << std::endl;
        // For other backends (ZSTD, BROTLI), we need to decompress the entire indices array
        // since they don't support random access
        auto all_indices = decompressIndices(indices_data, 0, backend);
        std::cout << "DEBUG: Decompressed all indices, size = " << all_indices.size() << std::endl;
        if (index >= all_indices.size()) {
            throw std::out_of_range("Dictionary: Index out of range");
        }
        uint32_t dict_index = all_indices[index];
        if (dict_index >= dictionary.size()) {
            throw std::runtime_error("Dictionary: Invalid dictionary index");
        }
        std::cout << "DEBUG: Returning dictionary[" << dict_index << "] = \"" << dictionary[dict_index] << "\"" << std::endl;
        return dictionary[dict_index];
    }
    
    throw std::out_of_range("Dictionary: Index out of range");
}

} // namespace algorithms
} // namespace compression
} // namespace json2