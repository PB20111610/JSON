#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "field_key.h"
#include "field_dictionary_manager.h"
#include "louds.h"
#include "loudsTotrie.h"

namespace json2 {

// 细粒度压缩后的数据结构
struct GranularCompressedData {
    // Trie结构压缩
    std::vector<uint8_t> trie_bitmap;        // Trie位图数据（单独压缩）
    
    // 字典分类压缩
    std::vector<uint8_t> string_dict;        // String字典（单独压缩）
    std::vector<uint8_t> timestamp_dict;     // Timestamp字典（单独压缩）
    std::vector<uint8_t> logtype_dict;       // LogType字典（单独压缩）
    
    // 分层内容压缩（可选择逐层或整体压缩）
    std::vector<std::vector<uint8_t>> layer_data_by_level;  // 按层分别压缩
    std::vector<uint8_t> layer_data_combined;               // 或整体压缩（二选一）
    bool use_layer_separation;                              // 是否使用分层压缩
    
    // 元数据
    std::vector<uint8_t> metadata;           // 元数据（字段列表、类型等）
    
    // 统计信息
    size_t original_size;                    // 原始数据大小
    size_t compressed_size;                  // 压缩后总大小
    
    // 各组件原始大小（用于压缩率分析）
    size_t trie_original_size;
    size_t string_dict_original_size;
    size_t timestamp_dict_original_size;
    size_t logtype_dict_original_size;
    size_t layer_original_size;
    size_t metadata_original_size;
};

// 兼容性：压缩后的数据结构
struct CompressedData {
    std::vector<uint8_t> trie_data;           // 压缩后的Trie树位图数据
    std::vector<uint8_t> layer_data;         // 压缩后的分层内容数据
    std::vector<uint8_t> dictionary_data;    // 压缩后的字典数据（仅包含String、Timestamp、LogType字典）
    std::vector<uint8_t> metadata_data;      // 压缩后的元数据（字段列表、类型等）
    size_t original_size;                    // 原始数据大小
    size_t compressed_size;                  // 压缩后总大小
};

// 压缩功能类
class Compressor {
public:
    // ========== 原有接口（兼容性） ==========
    // 压缩Trie树和相关数据
    static CompressedData compress(const Trie& trie, const FieldDictionaryManager& manager);
    
    // 解压缩并重建Trie树和字典
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompress(const CompressedData& compressed_data);
    
    // 计算压缩率
    static double getCompressionRatio(const CompressedData& compressed_data);
    
    // 保存压缩数据到文件
    static bool saveToFile(const CompressedData& compressed_data, const std::string& filename);
    
    // 从文件加载压缩数据
    static CompressedData loadFromFile(const std::string& filename);

    // ========== 新的细粒度压缩接口 ==========
    // 细粒度压缩Trie树和相关数据
    static GranularCompressedData compressGranular(const Trie& trie, const FieldDictionaryManager& manager, bool use_layer_separation = false);
    static GranularCompressedData compressGranularLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, const std::vector<FieldKey>& field_order, bool use_layer_separation = false);
    
    // 细粒度解压缩
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressGranular(const GranularCompressedData& compressed_data);
    static std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressGranularLouds(const GranularCompressedData& compressed_data);
    
    // 部分解压缩（按需加载特定组件）
    struct PartialDecompressionOptions {
        bool load_trie = true;
        bool load_string_dict = true;
        bool load_timestamp_dict = true;
        bool load_logtype_dict = true;
        bool load_layers = true;
        bool load_metadata = true;
        std::vector<size_t> specific_layers; // 如果使用分层压缩，指定加载哪些层
    };
    
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressGranularPartial(const GranularCompressedData& compressed_data, const PartialDecompressionOptions& options);
    
    // 细粒度压缩数据的文件操作
    static bool saveGranularToFile(const GranularCompressedData& compressed_data, const std::string& filename);
    static GranularCompressedData loadGranularFromFile(const std::string& filename);
    
    // 计算细粒度压缩率
    static double getGranularCompressionRatio(const GranularCompressedData& compressed_data);

    static CompressedData compressLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, const std::vector<FieldKey>& field_order);
    static std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> decompressLouds(const CompressedData& compressed_data);

    // ========== 原有字典序列化接口（兼容性） ==========
    // 序列化字典数据
    static std::vector<uint8_t> serializeDictionary(const FieldDictionaryManager& manager);
    
    // 反序列化字典数据
    static std::unique_ptr<FieldDictionaryManager> deserializeDictionary(const std::vector<uint8_t>& data);

    // ========== 新的分离字典序列化接口 ==========
    // 分别序列化不同类型的字典
    static std::vector<uint8_t> serializeStringDictionary(const FieldDictionaryManager& manager);
    static std::vector<uint8_t> serializeTimestampDictionary(const FieldDictionaryManager& manager);
    static std::vector<uint8_t> serializeLogTypeDictionary(const FieldDictionaryManager& manager);
    
    // 分别反序列化不同类型的字典
    static void deserializeStringDictionary(const std::vector<uint8_t>& data, FieldDictionaryManager& manager);
    static void deserializeTimestampDictionary(const std::vector<uint8_t>& data, FieldDictionaryManager& manager);
    static void deserializeLogTypeDictionary(const std::vector<uint8_t>& data, FieldDictionaryManager& manager);
    
    // 序列化元数据（字段列表、类型等）
    static std::vector<uint8_t> serializeMetadata(const Trie& trie, const FieldDictionaryManager& manager);
    static std::vector<uint8_t> serializeMetadata(const std::vector<FieldKey>& fields, const FieldDictionaryManager& manager);
    
    // 反序列化元数据
    static std::vector<FieldKey> deserializeMetadata(const std::vector<uint8_t>& data);
    
    // 使用Zstandard压缩数据
    static std::vector<uint8_t> compressWithZstd(const std::vector<uint8_t>& data);
    
    // 使用Zstandard解压缩数据
    static std::vector<uint8_t> decompressWithZstd(const std::vector<uint8_t>& compressed_data);

    static std::vector<uint8_t> serializeLoudsTrie(const LOUDSTrie& louds);
    static std::unique_ptr<LOUDSTrie> deserializeLoudsTrie(const std::vector<uint8_t>& data, const FieldDictionaryManager& dict_mgr, const std::vector<FieldKey>& field_order);

    // 内存序列化/反序列化接口
    static std::vector<uint8_t> saveToMemory(const CompressedData& compressed_data);
    static CompressedData loadFromMemory(const std::vector<uint8_t>& buffer);
};

} // namespace json2 