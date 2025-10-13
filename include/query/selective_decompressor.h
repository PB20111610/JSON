#pragma once

#include "chunk_selector.h"
#include "field_dictionary_manager.h"
#include "trie.h"
#include "louds.h"
#include <memory>
#include <unordered_set>
#include <unordered_map>

namespace json2 {
namespace query {

/**
 * 解压选项
 */
struct DecompressionOptions {
    bool decompress_trie = true;                         // 是否解压Trie
    bool decompress_string_dict = true;                  // 是否解压字符串字典
    bool decompress_timestamp_dict = true;               // 是否解压时间戳字典
    bool decompress_logtype_dict = true;                 // 是否解压日志类型字典
    std::unordered_set<std::string> required_fields;    // 需要的字段
    std::unordered_set<FieldType> required_types;       // 需要的类型
    bool partial_decompression = true;                   // 是否使用部分解压
    bool decompress_layer_sizes = false;                 // 是否解压层大小信息（新增）
};

/**
 * 解压结果
 */
struct DecompressionResult {
    std::unique_ptr<Trie> trie;                          // 解压的Trie
    std::unique_ptr<FieldDictionaryManager> dict_manager; // 解压的字典管理器
    std::unique_ptr<LOUDSTrie> louds;                    // 解压的LOUDS结构（如果可用）
    std::vector<FieldKey> field_order;                   // 字段顺序
    std::vector<uint32_t> layer_sizes;                   // 层大小信息（新增）
    size_t original_size;                                // 原始大小
    size_t decompressed_size;                            // 解压后大小
    double decompression_ratio;                          // 解压比例
    std::unordered_set<std::string> available_fields;    // 可用字段
    bool is_partial;                                     // 是否部分解压
};

/**
 * 选择性解压器
 * 只解压查询所需的字典和Trie部分
 */
class SelectiveDecompressor {
public:
    SelectiveDecompressor() = default;
    
    /**
     * 解压细粒度压缩数据（主要接口）
     * @param granular_data 细粒度压缩数据
     * @param options 解压选项
     * @return 解压结果
     */
    DecompressionResult decompress(const GranularCompressedData& granular_data,
                                  const DecompressionOptions& options);

    // ========== 新增：适配新查询思路的方法 ==========
    
    /**
     * 检查字段在chunk中是否存在
     * @param granular_data 细粒度压缩数据
     * @param field_name 字段名
     * @return 是否存在
     */
    bool fieldExistsInChunk(const GranularCompressedData& granular_data,
                           const std::string& field_name);
    
    /**
     * 解压特定字段的字典（用于查询）
     * @param granular_data 细粒度压缩数据
     * @param field_name 字段名
     * @param field_type 字段类型
     * @param manager 字典管理器
     * @return 是否成功
     */
    bool decompressFieldDictionaryForQuery(const GranularCompressedData& granular_data,
                                          const std::string& field_name,
                                          FieldType field_type,
                                          FieldDictionaryManager& manager);
    
    /**
     * 解压特定层的内容（用于查询）
     * @param granular_data 细粒度压缩数据
     * @param layer_index 层索引
     * @param louds LOUDS结构
     * @return 是否成功
     */
    bool decompressLayerForQuery(const GranularCompressedData& granular_data,
                                size_t layer_index,
                                LOUDSTrie& louds);
    
    /**
     * 解压LOUDS位图和层大小信息（用于查询）
     * @param granular_data 细粒度压缩数据
     * @param louds LOUDS结构
     * @param layer_sizes 层大小信息（输出）
     * @return 是否成功
     */
    bool decompressLoudsAndLayerSizesForQuery(const GranularCompressedData& granular_data,
                                             LOUDSTrie& louds,
                                             std::vector<uint32_t>& layer_sizes);
    
    /**
     * 按需解压特定层
     * @param louds LOUDS结构
     * @param granular_data 细粒度压缩数据
     * @param required_layers 需要的层索引
     * @return 是否成功
     */
    bool decompressSpecificLayers(LOUDSTrie& louds,
                                 const GranularCompressedData& granular_data,
                                 const std::vector<size_t>& required_layers);
    
    /**
     * 将字段名映射到层索引
     * @param required_fields 需要的字段
     * @param field_order 字段顺序
     * @return 层索引列表
     */
    std::vector<size_t> mapFieldsToLayers(const std::unordered_set<std::string>& required_fields,
                                         const std::vector<FieldKey>& field_order);
    
    /**
     * 基于字段分析创建解压选项
     * @param analysis 字段分析结果
     * @return 解压选项
     */
    DecompressionOptions createOptions(const FieldAnalysis& analysis);
    
    /**
     * 检查是否需要解压特定字段
     * @param field_name 字段名
     * @param options 解压选项
     * @return 是否需要解压
     */
    bool needsFieldDecompression(const std::string& field_name, 
                                const DecompressionOptions& options) const;
    
    /**
     * 检查是否需要解压特定类型
     * @param field_type 字段类型
     * @param options 解压选项
     * @return 是否需要解压
     */
    bool needsTypeDecompression(FieldType field_type, 
                               const DecompressionOptions& options) const;

private:
    // 解压Trie结构
    std::unique_ptr<Trie> decompressTrie(const std::vector<uint8_t>& trie_data,
                                        const std::vector<FieldKey>& field_order);
    
    // 解压字典管理器
    std::unique_ptr<FieldDictionaryManager> decompressDictionary(
        const std::vector<uint8_t>& dict_data,
        const DecompressionOptions& options);
    
    // 解压LOUDS结构
    std::unique_ptr<LOUDSTrie> decompressLouds(const std::vector<uint8_t>& louds_data,
                                               const std::vector<FieldKey>& field_order);
    
    // 部分解压字典
    std::unique_ptr<FieldDictionaryManager> partialDecompressDictionary(
        const std::vector<uint8_t>& dict_data,
        const DecompressionOptions& options);
    
    // 解压特定字段的字典
    void decompressFieldDictionary(FieldDictionaryManager& manager,
                                  const std::vector<uint8_t>& dict_data,
                                  const std::string& field_name,
                                  FieldType field_type);
    
    // 解压特定类型的字典
    void decompressTypeDictionary(FieldDictionaryManager& manager,
                                 const std::vector<uint8_t>& dict_data,
                                 FieldType field_type);
    
    // 计算解压统计
    void calculateDecompressionStats(DecompressionResult& result,
                                   const DecompressionOptions& options);
    
    // 验证解压结果
    bool validateDecompressionResult(const DecompressionResult& result,
                                   const DecompressionOptions& options);
    
    // ========== 新增：分层解压缩辅助方法 ==========
    
    // 解压LOUDS结构（仅位图）
    std::unique_ptr<LOUDSTrie> decompressLoudsStructure(const std::vector<uint8_t>& trie_bitmap);
    
    // 按需加载特定层
    bool loadLayer(LOUDSTrie& louds, size_t layer_idx, const std::vector<uint8_t>& layer_data);
    
    // 按需加载特定字典
    void loadStringDict(FieldDictionaryManager& manager, const std::vector<uint8_t>& string_dict);
    void loadTimestampDict(FieldDictionaryManager& manager, const std::vector<uint8_t>& timestamp_dict);
    void loadLogTypeDict(FieldDictionaryManager& manager, const std::vector<uint8_t>& logtype_dict);
    
    // ========== 新增：适配新查询思路的辅助方法 ==========
    
    // 解压层大小信息
    bool decompressLayerSizes(const std::vector<uint8_t>& layer_sizes_data,
                             std::vector<uint32_t>& layer_sizes);
};

} // namespace query
} // namespace json2