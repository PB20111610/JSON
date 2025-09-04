#include "metadata_compressor.h"
#include "../core/compression_utils.h"
#include "../algorithms/dictionary_compression.h"
#include "../algorithms/rle_compression.h"
#include "../algorithms/varint_compression.h"
#include <algorithm>
#include <sstream>

namespace json2 {
namespace compression {
namespace type_aware {

// 常用字段名定义
const std::vector<std::string> MetadataCompressor::COMMON_FIELD_NAMES = {
    "id", "name", "type", "value", "data", "status", "time", "timestamp",
    "created", "updated", "user", "email", "phone", "address", "city",
    "country", "price", "amount", "count", "size", "length", "width",
    "height", "description", "title", "content", "message", "error",
    "success", "result", "response", "request", "method", "url", "path"
};

std::vector<uint8_t> MetadataCompressor::compressMetadata(const JsonMetadata& metadata) {
    std::vector<uint8_t> result;
    
    // 写入压缩类型
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(MetadataCompressionType::FULL_METADATA));
    
    // 压缩完整元数据
    std::vector<uint8_t> compressed = compressFullMetadata(metadata);
    result.insert(result.end(), compressed.begin(), compressed.end());
    
    return result;
}

MetadataCompressor::JsonMetadata MetadataCompressor::decompressMetadata(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    size_t pos = 0;
    MetadataCompressionType type = static_cast<MetadataCompressionType>(
        utils::SerializationUtils::readValue<uint8_t>(compressed, pos));
    
    switch (type) {
        case MetadataCompressionType::FULL_METADATA:
            return decompressFullMetadata(compressed, pos);
        case MetadataCompressionType::SCHEMA_REFERENCE:
            // 需要外部cache支持
            throw std::runtime_error("Schema reference decompression requires cache");
        case MetadataCompressionType::INCREMENTAL:
            // 需要基础元数据
            throw std::runtime_error("Incremental decompression requires base metadata");
        default:
            throw std::runtime_error("Unknown metadata compression type");
    }
}

std::vector<uint8_t> MetadataCompressor::compressFieldNames(const std::vector<std::string>& field_names) {
    if (field_names.empty()) return {};
    
    // 初始化常用字段映射
    if (common_field_map_.empty()) {
        initializeCommonFieldMaps();
    }
    
    std::vector<uint8_t> result;
    
    // 分离常用字段和自定义字段
    std::vector<uint8_t> common_indices;
    std::vector<std::string> custom_fields;
    std::vector<uint8_t> field_types; // 0=常用，1=自定义
    
    for (const auto& field : field_names) {
        auto it = common_field_map_.find(field);
        if (it != common_field_map_.end()) {
            field_types.push_back(0);
            common_indices.push_back(it->second);
        } else {
            field_types.push_back(1);
            custom_fields.push_back(field);
        }
    }
    
    // 压缩字段类型标记（RLE压缩）
    std::vector<uint8_t> compressed_types = algorithms::RLECompression::rleCompress(field_types);
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_types.size()));
    result.insert(result.end(), compressed_types.begin(), compressed_types.end());
    
    // 压缩常用字段索引（Varint压缩）
    if (!common_indices.empty()) {
        algorithms::VarintCompression varint;
        std::vector<int64_t> indices_int64(common_indices.begin(), common_indices.end());
        std::vector<uint8_t> compressed_indices = varint.compressInt64Array(indices_int64);
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_indices.size()));
        result.insert(result.end(), compressed_indices.begin(), compressed_indices.end());
    } else {
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(0));
    }
    
    // 压缩自定义字段（字典压缩）
    if (!custom_fields.empty()) {
        algorithms::DictionaryCompression dict;
        std::vector<uint8_t> compressed_custom = dict.compressStrings(custom_fields);
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_custom.size()));
        result.insert(result.end(), compressed_custom.begin(), compressed_custom.end());
    } else {
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(0));
    }
    
    return result;
}

std::vector<std::string> MetadataCompressor::decompressFieldNames(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    if (common_field_reverse_map_.empty()) {
        initializeCommonFieldMaps();
    }
    
    size_t pos = 0;
    
    // 解压字段类型标记
    uint32_t types_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<uint8_t> compressed_types(compressed.begin() + pos, compressed.begin() + pos + types_size);
    pos += types_size;
    std::vector<uint8_t> field_types = algorithms::RLECompression::rleDecompress(compressed_types);
    
    // 解压常用字段索引
    uint32_t indices_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<uint8_t> common_indices;
    if (indices_size > 0) {
        std::vector<uint8_t> compressed_indices(compressed.begin() + pos, compressed.begin() + pos + indices_size);
        pos += indices_size;
        algorithms::VarintCompression varint;
        std::vector<int64_t> indices_int64 = varint.decompressInt64Array(compressed_indices);
        common_indices.assign(indices_int64.begin(), indices_int64.end());
    }
    
    // 解压自定义字段
    uint32_t custom_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<std::string> custom_fields;
    if (custom_size > 0) {
        std::vector<uint8_t> compressed_custom(compressed.begin() + pos, compressed.begin() + pos + custom_size);
        algorithms::DictionaryCompression dict;
        custom_fields = dict.decompressStrings(compressed_custom);
    }
    
    // 重建字段名列表
    std::vector<std::string> result;
    size_t common_idx = 0, custom_idx = 0;
    
    for (uint8_t type : field_types) {
        if (type == 0) {
            // 常用字段
            if (common_idx < common_indices.size()) {
                uint8_t idx = common_indices[common_idx++];
                if (idx < common_field_reverse_map_.size()) {
                    result.push_back(common_field_reverse_map_[idx]);
                }
            }
        } else {
            // 自定义字段
            if (custom_idx < custom_fields.size()) {
                result.push_back(custom_fields[custom_idx++]);
            }
        }
    }
    
    return result;
}

std::vector<uint8_t> MetadataCompressor::compressTypeInfo(const std::vector<uint8_t>& types) {
    if (types.empty()) return {};
    
    // 类型信息通常具有重复性，使用RLE压缩
    return algorithms::RLECompression::rleCompress(types);
}

std::vector<uint8_t> MetadataCompressor::decompressTypeInfo(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    return algorithms::RLECompression::rleDecompress(compressed);
}

std::vector<uint8_t> MetadataCompressor::compressWithSchema(const JsonMetadata& metadata, SchemaCache& cache) {
    std::string schema_hash = calculateSchemaHash(metadata);
    
    auto it = cache.schema_hash_to_id.find(schema_hash);
    if (it != cache.schema_hash_to_id.end()) {
        // 使用已存在的模式
        return compressSchemaReference(it->second);
    } else {
        // 新模式，添加到缓存
        uint32_t schema_id = cache.next_schema_id++;
        cache.schema_hash_to_id[schema_hash] = schema_id;
        cache.id_to_metadata[schema_id] = metadata;
        
        // 返回完整元数据（首次使用）
        return compressMetadata(metadata);
    }
}

MetadataCompressor::JsonMetadata MetadataCompressor::decompressWithSchema(const std::vector<uint8_t>& compressed, const SchemaCache& cache) {
    if (compressed.empty()) return {};
    
    size_t pos = 0;
    MetadataCompressionType type = static_cast<MetadataCompressionType>(
        utils::SerializationUtils::readValue<uint8_t>(compressed, pos));
    
    switch (type) {
        case MetadataCompressionType::FULL_METADATA:
            return decompressFullMetadata(compressed, pos);
        case MetadataCompressionType::SCHEMA_REFERENCE:
            {
                uint32_t schema_id = decompressSchemaReference(compressed, pos);
                auto it = cache.id_to_metadata.find(schema_id);
                if (it != cache.id_to_metadata.end()) {
                    return it->second;
                } else {
                    throw std::runtime_error("Schema ID not found in cache");
                }
            }
        default:
            throw std::runtime_error("Unsupported metadata compression type");
    }
}

MetadataCompressor::MetadataStats MetadataCompressor::analyzeMetadata(const JsonMetadata& metadata) {
    MetadataStats stats = {};
    
    // 计算字段名总大小
    for (const auto& name : metadata.field_names) {
        stats.field_name_total_size += name.size();
    }
    
    // 计算唯一字段数
    std::unordered_set<std::string> unique_fields(metadata.field_names.begin(), metadata.field_names.end());
    stats.unique_field_count = unique_fields.size();
    
    // 计算字段名冗余度
    if (!metadata.field_names.empty()) {
        stats.field_name_redundancy = 1.0 - (static_cast<double>(stats.unique_field_count) / metadata.field_names.size());
    }
    
    // 类型信息大小
    stats.type_info_size = metadata.field_types.size();
    
    // 估算压缩比
    size_t original_size = stats.field_name_total_size + metadata.field_types.size() + 
                          metadata.field_counts.size() * sizeof(uint32_t) + 
                          metadata.is_nullable.size() + sizeof(uint32_t) * 2;
                          
    std::vector<uint8_t> compressed = compressFieldNames(metadata.field_names);
    compressed = compressTypeInfo(metadata.field_types);
    
    stats.metadata_compression_ratio = static_cast<double>(compressed.size()) / original_size;
    
    return stats;
}

std::string MetadataCompressor::calculateSchemaHash(const JsonMetadata& metadata) {
    std::ostringstream oss;
    
    // 包含字段名、类型、可空性信息
    for (size_t i = 0; i < metadata.field_names.size(); ++i) {
        oss << metadata.field_names[i] << "|";
        if (i < metadata.field_types.size()) {
            oss << static_cast<int>(metadata.field_types[i]) << "|";
        }
        if (i < metadata.is_nullable.size()) {
            oss << (metadata.is_nullable[i] ? "1" : "0") << "|";
        }
    }
    
    return oss.str(); // 简化的哈希，实际应该使用真正的哈希算法
}

std::vector<uint8_t> MetadataCompressor::compressFullMetadata(const JsonMetadata& metadata) {
    std::vector<uint8_t> result;
    
    // 压缩字段名
    std::vector<uint8_t> compressed_names = compressFieldNames(metadata.field_names);
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_names.size()));
    result.insert(result.end(), compressed_names.begin(), compressed_names.end());
    
    // 压缩字段类型
    std::vector<uint8_t> compressed_types = compressTypeInfo(metadata.field_types);
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_types.size()));
    result.insert(result.end(), compressed_types.begin(), compressed_types.end());
    
    // 压缩字段计数（Varint）
    algorithms::VarintCompression varint;
    std::vector<int64_t> counts_int64(metadata.field_counts.begin(), metadata.field_counts.end());
    std::vector<uint8_t> compressed_counts = varint.compressInt64Array(counts_int64);
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_counts.size()));
    result.insert(result.end(), compressed_counts.begin(), compressed_counts.end());
    
    // 压缩可空性信息（BitPacking）
    std::vector<uint8_t> nullable_bytes = utils::SerializationUtils::boolsToBytes(metadata.is_nullable);
    std::vector<uint8_t> compressed_nullable = algorithms::RLECompression::rleCompress(nullable_bytes);
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_nullable.size()));
    result.insert(result.end(), compressed_nullable.begin(), compressed_nullable.end());
    
    // 其他字段
    utils::SerializationUtils::writeValue(result, metadata.total_records);
    utils::SerializationUtils::writeValue(result, metadata.schema_version);
    
    return result;
}

std::vector<uint8_t> MetadataCompressor::compressSchemaReference(uint32_t schema_id) {
    std::vector<uint8_t> result;
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(MetadataCompressionType::SCHEMA_REFERENCE));
    utils::SerializationUtils::writeValue(result, schema_id);
    return result;
}

MetadataCompressor::JsonMetadata MetadataCompressor::decompressFullMetadata(const std::vector<uint8_t>& compressed, size_t& pos) {
    JsonMetadata metadata;
    
    // 解压字段名
    uint32_t names_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<uint8_t> compressed_names(compressed.begin() + pos, compressed.begin() + pos + names_size);
    pos += names_size;
    metadata.field_names = decompressFieldNames(compressed_names);
    
    // 解压字段类型
    uint32_t types_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<uint8_t> compressed_types(compressed.begin() + pos, compressed.begin() + pos + types_size);
    pos += types_size;
    metadata.field_types = decompressTypeInfo(compressed_types);
    
    // 解压字段计数
    uint32_t counts_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<uint8_t> compressed_counts(compressed.begin() + pos, compressed.begin() + pos + counts_size);
    pos += counts_size;
    algorithms::VarintCompression varint;
    std::vector<int64_t> counts_int64 = varint.decompressInt64Array(compressed_counts);
    metadata.field_counts.assign(counts_int64.begin(), counts_int64.end());
    
    // 解压可空性信息
    uint32_t nullable_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<uint8_t> compressed_nullable(compressed.begin() + pos, compressed.begin() + pos + nullable_size);
    pos += nullable_size;
    std::vector<uint8_t> nullable_bytes = algorithms::RLECompression::rleDecompress(compressed_nullable);
    metadata.is_nullable = utils::SerializationUtils::bytesToBools(nullable_bytes, nullable_bytes.size());
    
    // 其他字段
    metadata.total_records = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    metadata.schema_version = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    
    return metadata;
}

uint32_t MetadataCompressor::decompressSchemaReference(const std::vector<uint8_t>& compressed, size_t& pos) {
    return utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
}

void MetadataCompressor::initializeCommonFieldMaps() {
    common_field_map_.clear();
    common_field_reverse_map_ = COMMON_FIELD_NAMES;
    
    for (size_t i = 0; i < COMMON_FIELD_NAMES.size(); ++i) {
        common_field_map_[COMMON_FIELD_NAMES[i]] = static_cast<uint8_t>(i);
    }
}

} // namespace type_aware
} // namespace compression
} // namespace json2