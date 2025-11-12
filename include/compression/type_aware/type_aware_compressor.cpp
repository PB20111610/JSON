#include "type_aware_compressor.h"
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
#include "../../field_key.h"
#include "../../compress.h"
#include "../../trie.h"
#include "../../field_dictionary_manager.h"
#include "../../louds.h"
#include "../../loudsTotrie.h"
#include <stdexcept>

namespace json2 {
namespace compression {
namespace type_aware {

TypeAwareCompressor::TypeAwareCompressor(const TypeAwareCompressionConfig& config)
    : config_(config) {}

std::vector<uint8_t> TypeAwareCompressor::compress(const std::vector<uint8_t>& data, FieldType type) {
    if (data.empty()) return {};
    
    // 根据配置和字段类型选择压缩策略
    CompressionBackend backend = selectBackendForFieldType(type);
    
    // 统一使用配置驱动的压缩方法
    return compressWithBackend(data, backend, type);
}

// 分层数据压缩接口（与主压缩接口相同）
std::vector<uint8_t> TypeAwareCompressor::compressLayerData(const std::vector<uint8_t>& data, FieldType type) {
    return compress(data, type);
}

// 分层数据解压接口（与主解压接口相同）
std::vector<uint8_t> TypeAwareCompressor::decompressLayerData(const std::vector<uint8_t>& compressed, FieldType type) {
    return decompress(compressed, type);
}

std::vector<uint8_t> TypeAwareCompressor::decompress(const std::vector<uint8_t>& compressed, FieldType type) {
    if (compressed.empty()) return {};
    
    size_t pos = 0;
    
    // 读取压缩方法标识
    uint8_t compression_method = utils::SerializationUtils::readValue<uint8_t>(compressed, pos);
    
    // 根据压缩方法解压数据
    switch (static_cast<CompressionMethod>(compression_method)) {
        case CompressionMethod::NONE:
            return std::vector<uint8_t>(compressed.begin() + pos, compressed.end());
        case CompressionMethod::RLE:
            return algorithms::RLECompression::rleDecompress(
                std::vector<uint8_t>(compressed.begin() + pos, compressed.end()));
        case CompressionMethod::VARINT:
            return decompressVarintField(compressed, pos);
        case CompressionMethod::DELTA:
            return decompressDeltaField(compressed, pos);
        case CompressionMethod::BITPACKING:
            return decompressBitPackingField(compressed, pos);
        case CompressionMethod::DICTIONARY:
            return decompressDictionaryField(compressed, pos);
        case CompressionMethod::BACKEND:
            return decompressBackendField(compressed, pos);
        default:
            throw std::runtime_error("Unknown compression method");
    }
}

// ===================================================================
// 注意：以下硬编码的类型特定压缩方法已被配置驱动的
// compressWithBackend() 方法替代，现在统一使用配置中的策略
// ===================================================================

std::vector<uint8_t> TypeAwareCompressor::decompressVarintField(const std::vector<uint8_t>& compressed, size_t& pos) {
    algorithms::VarintCompression varint;
    std::vector<int64_t> values = varint.decompressInt64Array(
        std::vector<uint8_t>(compressed.begin() + pos, compressed.end()));
    return utils::SerializationUtils::int64sToBytes(values);
}

std::vector<uint8_t> TypeAwareCompressor::decompressDeltaField(const std::vector<uint8_t>& compressed, size_t& pos) {
    std::vector<int64_t> values = algorithms::DeltaCompression::deltaVarintDecompress(
        std::vector<uint8_t>(compressed.begin() + pos, compressed.end()));
    return utils::SerializationUtils::int64sToBytes(values);
}

std::vector<uint8_t> TypeAwareCompressor::decompressBitPackingField(const std::vector<uint8_t>& compressed, size_t& pos) {
    algorithms::BitPackingCompression bitpacking;
    
    // BitPacking compression includes the count in its own header,
    // so we don't need to pass a separate count - use 0 to let it read from header
    std::vector<bool> values = bitpacking.decompressBool(
        std::vector<uint8_t>(compressed.begin() + pos, compressed.end()), 0);
    return utils::SerializationUtils::boolsToBytes(values);
}

std::vector<uint8_t> TypeAwareCompressor::decompressDictionaryField(const std::vector<uint8_t>& compressed, size_t& pos) {
    algorithms::DictionaryCompression dict;
    std::vector<std::string> strings = dict.decompressStrings(
        std::vector<uint8_t>(compressed.begin() + pos, compressed.end()));
    
    std::vector<uint8_t> result;
    utils::SerializationUtils::writeStringVector(result, strings);
    return result;
}

std::vector<uint8_t> TypeAwareCompressor::decompressBackendField(const std::vector<uint8_t>& compressed, size_t& pos) {
    // Read backend type indicator from the data
    if (pos >= compressed.size()) {
        throw std::runtime_error("Backend decompression: Insufficient data for backend type");
    }
    
    // For now, we need to determine the backend used during compression
    // Since the compression method is BACKEND, we try the configured default backend
    // TODO: In the future, we should store the backend type in the compression header
    
    std::vector<uint8_t> data(compressed.begin() + pos, compressed.end());
    
    // Try decompression with different backends based on configuration
    CompressionBackend fallback_backend = config_.fallback_backend;
    
    try {
        switch (fallback_backend) {
            case CompressionBackend::ZSTD: {
                backends::ZstdBackend backend;
                return backend.decompress(data);
            }
            case CompressionBackend::BROTLI: {
                backends::BrotliBackend backend;
                return backend.decompress(data);
            }
            case CompressionBackend::LZMA: {
                backends::LzmaBackend backend;
                return backend.decompress(data);
            }
            case CompressionBackend::LZ4: {
                backends::Lz4Backend backend;
                return backend.decompress(data);
            }
            case CompressionBackend::SNAPPY: {
                backends::SnappyBackend backend;
                return backend.decompress(data);
            }
            default: {
                // Default to ZSTD as fallback
                backends::ZstdBackend backend;
                return backend.decompress(data);
            }
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Backend decompression failed with " + getBackendName(fallback_backend) + ": " + e.what());
    }
}

// ===================================================================
// 性能监控和统计接口实现
// ===================================================================

void TypeAwareCompressor::recordCompressionStats(size_t original_size, size_t compressed_size, 
                                                CompressionBackend backend, double time_ms, bool success) {
    CompressionStats stats;
    stats.original_size = original_size;
    stats.compressed_size = compressed_size;
    stats.compression_ratio = (original_size > 0) ? (double)compressed_size / original_size : 1.0;
    stats.backend_used = backend;
    stats.algorithm_name = getBackendName(backend);
    stats.compression_time_ms = time_ms;
    stats.decompression_time_ms = 0.0; // Will be updated during decompression
    stats.is_successful = success;
    
    last_stats_ = stats;
    all_stats_.push_back(stats);
}

std::string TypeAwareCompressor::getBackendName(CompressionBackend backend) const {
    switch (backend) {
        case CompressionBackend::RLE: return "RLE";
        case CompressionBackend::DELTA_VARINT: return "Delta+Varint";
        case CompressionBackend::DELTA_DELTA: return "Delta-of-Delta";
        case CompressionBackend::BIT_PACKING: return "BitPacking";
        case CompressionBackend::DICTIONARY: return "Dictionary";
        case CompressionBackend::ZSTD: return "ZSTD";
        case CompressionBackend::BROTLI: return "Brotli";
        case CompressionBackend::LZMA: return "LZMA";
        case CompressionBackend::LZ4: return "LZ4";
        case CompressionBackend::SNAPPY: return "Snappy";
        case CompressionBackend::AUTO: return "AUTO";
        case CompressionBackend::NONE: return "None";
        default: return "Unknown";
    }
}

// 根据配置和字段类型选择压缩后端
CompressionBackend TypeAwareCompressor::selectBackendForFieldType(FieldType type) {
    switch (type) {
        case FieldType::INT64:
            return config_.layer_config.int_backend;
        case FieldType::UINT32:
            return config_.layer_config.int_backend;  // 使用相同的整数配置
        case FieldType::DOUBLE:
            return config_.layer_config.double_backend;
        case FieldType::BOOL:
            return config_.layer_config.bool_backend;
        case FieldType::STRING:
            return config_.layer_config.string_backend;
        case FieldType::TIMESTAMP:
            return config_.layer_config.timestamp_backend;
        case FieldType::LOGTYPE:
            return config_.layer_config.logtype_backend;
        case FieldType::ARRAY:
            return config_.layer_config.array_backend;
        case FieldType::OBJECT:
            return config_.fallback_backend;  // 对象类型使用备用策略
        case FieldType::NULL_TYPE:
            return config_.layer_config.null_backend;
        default:
            return config_.fallback_backend;
    }
}

// 使用指定后端进行压缩
std::vector<uint8_t> TypeAwareCompressor::compressWithBackend(const std::vector<uint8_t>& data, 
                                                            CompressionBackend backend, 
                                                            FieldType type) {
    Timer timer; // 记录压缩时间
    std::vector<uint8_t> result;
    bool success = false;
    
    try {
        // 如果是AUTO模式，则使用原有的智能选择逻辑
        if (backend == CompressionBackend::AUTO) {
            result = compressWithIntelligentSelection(data, type);
            success = true;
        } else {
            // 根据用户配置的后端进行压缩
            switch (backend) {
                case CompressionBackend::RLE: {
                    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::RLE));
                    std::vector<uint8_t> compressed = algorithms::RLECompression::rleCompress(data);
                    result.insert(result.end(), compressed.begin(), compressed.end());
                    success = true;
                    break;
                }
                case CompressionBackend::DELTA_VARINT: {
                    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::DELTA));
                    if (type == FieldType::INT64 || type == FieldType::UINT32) {
                        std::vector<int64_t> values = utils::SerializationUtils::bytesToInt64s(data);
                        std::vector<uint8_t> compressed = algorithms::DeltaCompression::deltaVarintCompress(values);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    } else if (type == FieldType::DOUBLE) {
                        std::vector<double> values = utils::SerializationUtils::bytesToDoubles(data);
                        algorithms::DeltaCompression delta;
                        std::vector<uint8_t> compressed = delta.compressDouble(values);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    } else if (type == FieldType::TIMESTAMP) {
                        // For TIMESTAMP, preserve TemplateEncodedTimestamp structure
                        // Convert structured data to a sequence of uint32_t values that can be properly reconstructed
                        std::vector<uint32_t> structured_values = json2::compression::utils::SerializationUtils::bytesToUint32s(data);
                        
                        // Debug output for TIMESTAMP compression
                        std::cout << "DEBUG: TIMESTAMP compression - structured_values size: " << structured_values.size() << std::endl;
                        std::cout << "DEBUG: TIMESTAMP compression - structured_values: [";
                        for (size_t i = 0; i < structured_values.size(); ++i) {
                            if (i > 0) std::cout << ",";
                            std::cout << structured_values[i];
                        }
                        std::cout << "]" << std::endl;
                        
                        std::vector<uint8_t> compressed = algorithms::DeltaCompression::deltaVarintCompress(
                            std::vector<int64_t>(structured_values.begin(), structured_values.end()));
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    } else if (type == FieldType::LOGTYPE) {
                        // For LOGTYPE, preserve EncodedLog structure
                        // Convert structured data to a sequence of uint32_t values that can be properly reconstructed
                        std::vector<uint32_t> structured_values = json2::compression::utils::SerializationUtils::bytesToUint32s(data);
                        
                        // Debug output for LOGTYPE compression
                        std::cout << "DEBUG: LOGTYPE compression - structured_values size: " << structured_values.size() << std::endl;
                        std::cout << "DEBUG: LOGTYPE compression - structured_values: [";
                        for (size_t i = 0; i < structured_values.size(); ++i) {
                            if (i > 0) std::cout << ",";
                            std::cout << structured_values[i];
                        }
                        std::cout << "]" << std::endl;
                        
                        std::vector<uint8_t> compressed = algorithms::DeltaCompression::deltaVarintCompress(
                            std::vector<int64_t>(structured_values.begin(), structured_values.end()));
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    } else {
                        // For other types (String/Array), assume they are encoded value sequences
                        std::vector<uint32_t> codes = utils::SerializationUtils::bytesToUint32s(data);
                        std::vector<int64_t> int64_codes(codes.begin(), codes.end());
                        std::vector<uint8_t> compressed = algorithms::DeltaCompression::deltaVarintCompress(int64_codes);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    }
                    success = true;
                    break;
                }
                case CompressionBackend::DELTA_DELTA: {
                    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::DELTA));
                    if (type == FieldType::INT64 || type == FieldType::TIMESTAMP) {
                        std::vector<int64_t> values = utils::SerializationUtils::bytesToInt64s(data);
                        std::vector<uint8_t> compressed = algorithms::DeltaDeltaCompression::deltaDeltaCompress(values);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    } else {
                        // 对于非时间戳类型，降级为delta+varint
                        std::vector<int64_t> values = utils::SerializationUtils::bytesToInt64s(data);
                        std::vector<uint8_t> compressed = algorithms::DeltaCompression::deltaVarintCompress(values);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    }
                    success = true;
                    break;
                }
                case CompressionBackend::BIT_PACKING: {
                    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::BITPACKING));
                    if (type == FieldType::BOOL) {
                        size_t bool_count = data.size();
                        std::vector<bool> values = utils::SerializationUtils::bytesToBools(data, bool_count);
                        algorithms::BitPackingCompression bitpacking;
                        std::vector<uint8_t> compressed = bitpacking.compressBool(values);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    } else {
                        // 对于非布尔类型，降级为RLE
                        std::vector<uint8_t> compressed = algorithms::RLECompression::rleCompress(data);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    }
                    success = true;
                    break;
                }
                case CompressionBackend::DICTIONARY: {
                    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::DICTIONARY));
                    if (type == FieldType::STRING) {
                        size_t pos = 0;
                        std::vector<std::string> strings = utils::SerializationUtils::readStringVector(data, pos);
                        algorithms::DictionaryCompression dict;
                        std::vector<uint8_t> compressed = dict.compressStrings(strings);
                        result.insert(result.end(), compressed.begin(), compressed.end());
                    } else {
                        // 对于非字符串类型，直接存储
                        utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::NONE));
                        result.insert(result.end(), data.begin(), data.end());
                    }
                    success = true;
                    break;
                }
                case CompressionBackend::ZSTD:
                case CompressionBackend::BROTLI:
                case CompressionBackend::LZMA:
                case CompressionBackend::LZ4:
                case CompressionBackend::SNAPPY: {
                    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::BACKEND));
                    std::vector<uint8_t> compressed = compressWithExternalBackend(data, backend);
                    result.insert(result.end(), compressed.begin(), compressed.end());
                    success = true;
                    break;
                }
                case CompressionBackend::NONE:
                default: {
                    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::NONE));
                    result.insert(result.end(), data.begin(), data.end());
                    success = true;
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        // 在错误情况下，回退到无压缩
        result.clear();
        utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(CompressionMethod::NONE));
        result.insert(result.end(), data.begin(), data.end());
        success = false;
    }
    
    // 记录统计信息
    double elapsed_ms = timer.elapsedMs();
    recordCompressionStats(data.size(), result.size(), backend, elapsed_ms, success);
    
    return result;
}

// 使用智能选择进行压缩（使用配置的默认策略）
std::vector<uint8_t> TypeAwareCompressor::compressWithIntelligentSelection(const std::vector<uint8_t>& data, FieldType type) {
    // 获取配置中该类型的默认后端策略
    CompressionBackend defaultBackend = selectBackendForFieldType(type);
    
    // 使用配置的默认策略进行压缩
    return compressWithBackend(data, defaultBackend, type);
}

// 使用外部压缩后端
std::vector<uint8_t> TypeAwareCompressor::compressWithExternalBackend(const std::vector<uint8_t>& data, CompressionBackend backend) {
    switch (backend) {
        case CompressionBackend::ZSTD: {
            backends::ZstdBackend zstd;
            return zstd.compress(data, config_.compression_level);
        }
        case CompressionBackend::BROTLI: {
            backends::BrotliBackend brotli;
            return brotli.compress(data, config_.compression_level);
        }
        case CompressionBackend::LZMA: {
            backends::LzmaBackend lzma;
            return lzma.compress(data, config_.compression_level);
        }
        case CompressionBackend::LZ4: {
            backends::Lz4Backend lz4;
            return lz4.compress(data, config_.compression_level);
        }
        case CompressionBackend::SNAPPY: {
            backends::SnappyBackend snappy;
            return snappy.compress(data, config_.compression_level);
        }
        default:
            // 备用策略：使用ZSTD
            backends::ZstdBackend zstd;
            return zstd.compress(data, config_.compression_level);
    }
}

// ===================================================================
// 高级压缩接口实现 - 处理完整的数据结构
// ===================================================================

CompressedData TypeAwareCompressor::compress(const Trie& trie, const FieldDictionaryManager& manager) {
    CompressedData result;
    
    try {
        // 序列化数据
        std::vector<uint8_t> trie_data = serializeTrie(trie);
        std::vector<uint8_t> dict_data = serializeDictionary(manager);
        
        // 使用配置压缩
        result.trie_data = compress(trie_data, FieldType::BOOL);  // LOUDS使用布尔类型
        result.dictionary_data = compress(dict_data, FieldType::STRING);  // 字典使用字符串类型
        
        // 计算大小
        result.original_size = trie_data.size() + dict_data.size();
        result.compressed_size = result.trie_data.size() + result.dictionary_data.size();
        
        // 创建元数据
        std::vector<uint8_t> metadata;
        utils::SerializationUtils::writeValue(metadata, static_cast<uint32_t>(trie_data.size()));
        utils::SerializationUtils::writeValue(metadata, static_cast<uint32_t>(dict_data.size()));
        result.metadata_data = compress(metadata, FieldType::STRING);
        result.compressed_size += result.metadata_data.size();
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Compression failed: " + std::string(e.what()));
    }
    
    return result;
}

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>>
TypeAwareCompressor::decompress(const CompressedData& compressed_data) {
    try {
        // 解压元数据
        std::vector<uint8_t> metadata = decompress(compressed_data.metadata_data, FieldType::STRING);
        size_t pos = 0;
        uint32_t trie_size = utils::SerializationUtils::readValue<uint32_t>(metadata, pos);
        uint32_t dict_size = utils::SerializationUtils::readValue<uint32_t>(metadata, pos);
        
        // 解压数据
        std::vector<uint8_t> trie_data = decompress(compressed_data.trie_data, FieldType::BOOL);
        std::vector<uint8_t> dict_data = decompress(compressed_data.dictionary_data, FieldType::STRING);
        
        // 反序列化
        auto trie = deserializeTrie(trie_data);
        auto manager = deserializeDictionary(dict_data);
        
        return std::make_pair(std::move(trie), std::move(manager));
    } catch (const std::exception& e) {
        throw std::runtime_error("Decompression failed: " + std::string(e.what()));
    }
}

CompressedData TypeAwareCompressor::compressLouds(const LOUDSTrie& louds, 
                                                 const FieldDictionaryManager& manager,
                                                 const std::vector<FieldKey>& field_order) {
    CompressedData result;
    
    try {
        // 序列化数据
        std::vector<uint8_t> louds_data = serializeLouds(louds);
        std::vector<uint8_t> dict_data = serializeDictionary(manager);
        std::vector<uint8_t> field_data = serializeFieldOrder(field_order);
        
        // 使用配置压缩
        result.trie_data = compress(louds_data, FieldType::BOOL);
        result.dictionary_data = compress(dict_data, FieldType::STRING);
        result.layer_data = compress(field_data, FieldType::STRING);
        
        // 计算大小
        result.original_size = louds_data.size() + dict_data.size() + field_data.size();
        result.compressed_size = result.trie_data.size() + result.dictionary_data.size() + result.layer_data.size();
        
        // 创建元数据
        std::vector<uint8_t> metadata;
        utils::SerializationUtils::writeValue(metadata, static_cast<uint32_t>(louds_data.size()));
        utils::SerializationUtils::writeValue(metadata, static_cast<uint32_t>(dict_data.size()));
        utils::SerializationUtils::writeValue(metadata, static_cast<uint32_t>(field_data.size()));
        result.metadata_data = compress(metadata, FieldType::STRING);
        result.compressed_size += result.metadata_data.size();
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LOUDS compression failed: " + std::string(e.what()));
    }
    
    return result;
}

std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>>
TypeAwareCompressor::decompressLouds(const CompressedData& compressed_data) {
    try {
        // 解压元数据
        std::vector<uint8_t> metadata = decompress(compressed_data.metadata_data, FieldType::STRING);
        size_t pos = 0;
        uint32_t louds_size = utils::SerializationUtils::readValue<uint32_t>(metadata, pos);
        uint32_t dict_size = utils::SerializationUtils::readValue<uint32_t>(metadata, pos);
        uint32_t field_size = utils::SerializationUtils::readValue<uint32_t>(metadata, pos);
        
        // 解压数据
        std::vector<uint8_t> louds_data = decompress(compressed_data.trie_data, FieldType::BOOL);
        std::vector<uint8_t> dict_data = decompress(compressed_data.dictionary_data, FieldType::STRING);
        std::vector<uint8_t> field_data = decompress(compressed_data.layer_data, FieldType::STRING);
        
        // 反序列化
        auto field_order = deserializeFieldOrder(field_data);
        auto louds = deserializeLouds(louds_data, field_order);
        auto manager = deserializeDictionary(dict_data);
        
        return std::make_pair(std::move(louds), std::move(manager));
    } catch (const std::exception& e) {
        throw std::runtime_error("LOUDS decompression failed: " + std::string(e.what()));
    }
}

// ===================================================================
// 序列化和反序列化工具方法
// ===================================================================

std::vector<uint8_t> TypeAwareCompressor::serializeTrie(const Trie& trie) {
    // 传递给 Compressor 的序列化方法
    return json2::Compressor::serializeLoudsTrie(LOUDSTrie(trie.getOrderedFields()));
}

std::unique_ptr<Trie> TypeAwareCompressor::deserializeTrie(const std::vector<uint8_t>& data) {
    // 这里需要实现完整的反序列化，暂时返回空对象
    return std::make_unique<Trie>(std::vector<FieldKey>{});
}

std::vector<uint8_t> TypeAwareCompressor::serializeDictionary(const FieldDictionaryManager& manager) {
    return json2::Compressor::serializeDictionary(manager);
}

std::unique_ptr<FieldDictionaryManager> TypeAwareCompressor::deserializeDictionary(const std::vector<uint8_t>& data) {
    return json2::Compressor::deserializeDictionary(data);
}

std::vector<uint8_t> TypeAwareCompressor::serializeLouds(const LOUDSTrie& louds) {
    return json2::Compressor::serializeLoudsTrie(louds);
}

std::unique_ptr<LOUDSTrie> TypeAwareCompressor::deserializeLouds(const std::vector<uint8_t>& data, 
                                                               const std::vector<FieldKey>& field_order) {
    // 创建空的 FieldDictionaryManager 作为参数
    FieldDictionaryManager dummy_manager;
    return json2::Compressor::deserializeLoudsTrie(data, dummy_manager, field_order);
}

std::vector<uint8_t> TypeAwareCompressor::serializeFieldOrder(const std::vector<FieldKey>& field_order) {
    std::vector<uint8_t> data;
    utils::SerializationUtils::writeValue(data, static_cast<uint32_t>(field_order.size()));
    for (const auto& field : field_order) {
        // 这里需要实现 FieldKey 的序列化，暂时使用简单实现
        utils::SerializationUtils::writeString(data, field.name);
        utils::SerializationUtils::writeValue(data, static_cast<uint8_t>(field.type));
    }
    return data;
}

std::vector<FieldKey> TypeAwareCompressor::deserializeFieldOrder(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    uint32_t count = utils::SerializationUtils::readValue<uint32_t>(data, pos);
    std::vector<FieldKey> field_order;
    field_order.reserve(count);
    
    for (uint32_t i = 0; i < count; ++i) {
        FieldKey field;
        field.name = utils::SerializationUtils::readString(data, pos);
        field.type = static_cast<json2::FieldType>(utils::SerializationUtils::readValue<uint8_t>(data, pos));
        field_order.push_back(std::move(field));
    }
    
    return field_order;
}

} // namespace type_aware
} // namespace compression
} // namespace json2