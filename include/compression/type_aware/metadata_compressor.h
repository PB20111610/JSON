#pragma once

#include "../core/compression_interface.h"
#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace json2 {
namespace compression {
namespace type_aware {

/**
 * 元数据压缩器
 * 专门处理JSON结构的元数据压缩，如字段名、类型信息等
 */
class MetadataCompressor {
public:
    MetadataCompressor() = default;
    virtual ~MetadataCompressor() = default;
    
    /**
     * JSON元数据结构
     */
    struct JsonMetadata {
        std::vector<std::string> field_names;    // 字段名列表
        std::vector<uint8_t> field_types;        // 字段类型列表
        std::vector<uint32_t> field_counts;      // 每个字段的元素数量
        std::vector<bool> is_nullable;           // 字段是否可为空
        uint32_t total_records;                  // 总记录数
        uint32_t schema_version;                 // 模式版本
    };
    
    /**
     * 压缩JSON元数据
     * @param metadata JSON元数据
     * @return 压缩后的元数据
     */
    std::vector<uint8_t> compressMetadata(const JsonMetadata& metadata);
    
    /**
     * 解压JSON元数据
     * @param compressed 压缩的元数据
     * @return 解压后的元数据
     */
    JsonMetadata decompressMetadata(const std::vector<uint8_t>& compressed);
    
    /**
     * 压缩字段名字典
     * @param field_names 字段名列表
     * @return 压缩后的字典
     */
    std::vector<uint8_t> compressFieldNames(const std::vector<std::string>& field_names);
    
    /**
     * 解压字段名字典
     * @param compressed 压缩的字典
     * @return 解压后的字段名
     */
    std::vector<std::string> decompressFieldNames(const std::vector<uint8_t>& compressed);
    
    /**
     * 压缩类型信息
     * @param types 类型信息列表
     * @return 压缩后的类型信息
     */
    std::vector<uint8_t> compressTypeInfo(const std::vector<uint8_t>& types);
    
    /**
     * 解压类型信息
     * @param compressed 压缩的类型信息
     * @return 解压后的类型信息
     */
    std::vector<uint8_t> decompressTypeInfo(const std::vector<uint8_t>& compressed);
    
    // 模式缓存和优化
    struct SchemaCache {
        std::unordered_map<std::string, uint32_t> schema_hash_to_id;
        std::unordered_map<uint32_t, JsonMetadata> id_to_metadata;
        uint32_t next_schema_id;
    };
    
    /**
     * 使用模式缓存压缩元数据
     * @param metadata JSON元数据
     * @param cache 模式缓存
     * @return 压缩后的元数据（可能只是模式ID）
     */
    std::vector<uint8_t> compressWithSchema(const JsonMetadata& metadata, SchemaCache& cache);
    
    /**
     * 使用模式缓存解压元数据
     * @param compressed 压缩的元数据
     * @param cache 模式缓存
     * @return 解压后的元数据
     */
    JsonMetadata decompressWithSchema(const std::vector<uint8_t>& compressed, const SchemaCache& cache);
    
    // 分析工具
    struct MetadataStats {
        size_t field_name_total_size;
        size_t unique_field_count;
        double field_name_redundancy;
        size_t type_info_size;
        double metadata_compression_ratio;
    };
    
    /**
     * 分析元数据特征
     */
    MetadataStats analyzeMetadata(const JsonMetadata& metadata);
    
    /**
     * 计算模式哈希值
     */
    std::string calculateSchemaHash(const JsonMetadata& metadata);
    
private:
    enum class MetadataCompressionType : uint8_t {
        FULL_METADATA = 0,    // 完整元数据
        SCHEMA_REFERENCE = 1, // 模式引用
        INCREMENTAL = 2       // 增量元数据
    };
    
    // 内部压缩方法
    std::vector<uint8_t> compressFullMetadata(const JsonMetadata& metadata);
    std::vector<uint8_t> compressSchemaReference(uint32_t schema_id);
    std::vector<uint8_t> compressIncrementalMetadata(const JsonMetadata& metadata, const JsonMetadata& base_metadata);
    
    JsonMetadata decompressFullMetadata(const std::vector<uint8_t>& compressed, size_t& pos);
    uint32_t decompressSchemaReference(const std::vector<uint8_t>& compressed, size_t& pos);
    JsonMetadata decompressIncrementalMetadata(const std::vector<uint8_t>& compressed, const JsonMetadata& base_metadata, size_t& pos);
    
    // 字段名优化
    std::vector<uint8_t> optimizeFieldNames(const std::vector<std::string>& field_names);
    std::vector<std::string> restoreFieldNames(const std::vector<uint8_t>& optimized);
    
    // 常用字段名字典
    static const std::vector<std::string> COMMON_FIELD_NAMES;
    std::unordered_map<std::string, uint8_t> common_field_map_;
    std::vector<std::string> common_field_reverse_map_;
    
    void initializeCommonFieldMaps();
};

} // namespace type_aware
} // namespace compression
} // namespace json2