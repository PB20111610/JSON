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

// 压缩后的数据结构
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

    static CompressedData compressLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, const std::vector<FieldKey>& field_order);
    static std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> decompressLouds(const CompressedData& compressed_data);

    // 序列化字典数据
    static std::vector<uint8_t> serializeDictionary(const FieldDictionaryManager& manager);
    
    // 反序列化字典数据
    static std::unique_ptr<FieldDictionaryManager> deserializeDictionary(const std::vector<uint8_t>& data);
    
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