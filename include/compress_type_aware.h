#pragma once

#include "compression/type_aware/type_aware_compressor.h"
#include "compression/factory/compression_factory.h"
#include "compress.h"  // Use existing Compressor for all other operations
#include <memory>

namespace json2 {

// Forward declarations
class Trie;
class FieldDictionaryManager;
class LOUDSTrie;
struct FieldKey;
struct CompressedData;

class TypeAwareCompressor {
public:
    // Core compression methods with configurable algorithms - the ONLY unique functionality
    static CompressedData compress(const Trie& trie, const FieldDictionaryManager& manager, 
                                  const compression::TypeAwareCompressionConfig& config = {});
    
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompress(const CompressedData& compressed_data, 
               const compression::TypeAwareCompressionConfig& config = {});
    
    static CompressedData compressLouds(const LOUDSTrie& louds, 
                                       const FieldDictionaryManager& manager,
                                       const std::vector<FieldKey>& field_order,
                                       const compression::TypeAwareCompressionConfig& config = {});
    
    static std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressLouds(const CompressedData& compressed_data, 
                    const compression::TypeAwareCompressionConfig& config = {});
    
    // ========== 细粒度类型敏感压缩接口 ==========
    // 细粒度类型敏感压缩
    static GranularCompressedData compressGranular(const Trie& trie, const FieldDictionaryManager& manager, 
                                                  const compression::TypeAwareCompressionConfig& config = {}, 
                                                  bool use_layer_separation = false);
    static GranularCompressedData compressGranularLouds(const LOUDSTrie& louds, 
                                                       const FieldDictionaryManager& manager,
                                                       const std::vector<FieldKey>& field_order,
                                                       const compression::TypeAwareCompressionConfig& config = {},
                                                       bool use_layer_separation = false);
    
    // 细粒度类型敏感解压缩
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressGranular(const GranularCompressedData& compressed_data, 
                      const compression::TypeAwareCompressionConfig& config = {});
    static std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressGranularLouds(const GranularCompressedData& compressed_data, 
                           const compression::TypeAwareCompressionConfig& config = {});
    
    // 部分解压缩（类型敏感版本）
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressGranularPartial(const GranularCompressedData& compressed_data, 
                             const Compressor::PartialDecompressionOptions& options,
                             const compression::TypeAwareCompressionConfig& config = {});

    // ========== 类型敏感的部分解压缩接口 ==========
    // 部分解压String字典中的特定值（使用配置的字典后端）
    static std::string getStringValueAt(const std::vector<uint8_t>& data, 
                                       const FieldKey& target_fk, 
                                       uint32_t code,
                                       const compression::TypeAwareCompressionConfig& config = {});
    
    // 部分解压Timestamp字典中的模板（使用配置的字典后端）
    static std::string getTimestampTemplateAt(const std::vector<uint8_t>& data, 
                                             uint32_t template_id,
                                             const compression::TypeAwareCompressionConfig& config = {});
    
    // 部分解压Timestamp字典中的变量（使用配置的字典后端）
    static std::string getTimestampVariableAt(const std::vector<uint8_t>& data, 
                                             uint32_t var_code,
                                             const compression::TypeAwareCompressionConfig& config = {});
    
    // 部分解压LogType字典中的模板（使用配置的字典后端）
    static std::string getLogTypeTemplateAt(const std::vector<uint8_t>& data, 
                                           uint32_t template_id,
                                           const compression::TypeAwareCompressionConfig& config = {});
    
    // 部分解压LogType字典中的变量（使用配置的字典后端）
    static std::string getLogTypeVariableAt(const std::vector<uint8_t>& data, 
                                           uint32_t var_code,
                                           const compression::TypeAwareCompressionConfig& config = {});

    // Statistics for type-aware compression
    static void printCompressionStats(const compression::TypeAwareCompressionConfig& config = {});
    static void clearCompressionStats(const compression::TypeAwareCompressionConfig& config = {});
    static double getCompressionRatio(const CompressedData& compressed_data);
    static double getGranularCompressionRatio(const GranularCompressedData& compressed_data);

    // All other operations delegate to the existing Compressor class:
    // - File I/O: Compressor::saveToFile(), Compressor::loadFromFile()
    // - Memory I/O: Compressor::saveToMemory(), Compressor::loadFromMemory()
    // - Compression ratio: Compressor::getCompressionRatio()
    // - Serialization: Compressor::serializeDictionary(), Compressor::deserializeDictionary()
    // - Metadata: Compressor::serializeMetadata(), Compressor::deserializeMetadata()
};

} // namespace json2