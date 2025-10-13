#include "../include/query/selective_decompressor.h"
#include "../include/compress.h"
#include "../include/compress_type_aware.h"
#include <algorithm>

namespace json2 {
namespace query {

DecompressionResult SelectiveDecompressor::decompress(const GranularCompressedData& granular_data,
                                                     const DecompressionOptions& options) {
    DecompressionResult result;
    
    try {
        // 1. 只解压LOUDS位图（结构信息）
        result.louds = decompressLoudsStructure(granular_data.trie_bitmap);
        if (!result.louds) {
            throw std::runtime_error("Failed to decompress LOUDS structure");
        }
        
        // 2. 解压元数据以获取字段顺序
        std::vector<uint8_t> metadata_raw = Compressor::decompressWithZstd(granular_data.metadata);
        result.field_order = Compressor::deserializeMetadata(metadata_raw);
        result.louds->setFieldOrder(result.field_order);
        
        // 3. 按需解压特定层的节点值
        if (!options.required_fields.empty()) {
            std::vector<size_t> required_layers = mapFieldsToLayers(
                options.required_fields, result.field_order);
            
            if (!decompressSpecificLayers(*result.louds, granular_data, required_layers)) {
                throw std::runtime_error("Failed to decompress specific layers");
            }
        } else {
            // 解压所有层
            std::vector<size_t> all_layers;
            for (size_t i = 0; i < result.field_order.size(); ++i) {
                all_layers.push_back(i);
            }
            decompressSpecificLayers(*result.louds, granular_data, all_layers);
        }
        
        // 4. 按需解压字典
        result.dict_manager = std::make_unique<FieldDictionaryManager>();
        if (options.decompress_string_dict) {
            loadStringDict(*result.dict_manager, granular_data.string_dict);
        }
        if (options.decompress_timestamp_dict) {
            loadTimestampDict(*result.dict_manager, granular_data.timestamp_dict);
        }
        if (options.decompress_logtype_dict) {
            loadLogTypeDict(*result.dict_manager, granular_data.logtype_dict);
        }
        
        // 5. 设置统计信息
        result.original_size = granular_data.original_size;
        result.decompressed_size = granular_data.compressed_size;
        result.decompression_ratio = static_cast<double>(result.decompressed_size) / 
                                   (result.original_size == 0 ? 1 : result.original_size);
        result.is_partial = !options.required_fields.empty();
        
        // 6. 设置可用字段
        for (const auto& field : result.field_order) {
            result.available_fields.insert(field.name);
        }
        
        // 7. 验证结果
        if (!validateDecompressionResult(result, options)) {
            throw std::runtime_error("Decompression validation failed");
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("decompression failed: " + std::string(e.what()));
    }
    
    return result;
}

bool SelectiveDecompressor::decompressSpecificLayers(LOUDSTrie& louds,
                                                    const GranularCompressedData& granular_data,
                                                    const std::vector<size_t>& required_layers) {
    try {
        for (size_t layer_idx : required_layers) {
            if (layer_idx >= granular_data.layer_data_by_level.size()) {
                continue; // 跳过不存在的层
            }
            
            if (!loadLayer(louds, layer_idx, granular_data.layer_data_by_level[layer_idx])) {
                return false;
            }
        }
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

std::vector<size_t> SelectiveDecompressor::mapFieldsToLayers(const std::unordered_set<std::string>& required_fields,
                                                            const std::vector<FieldKey>& field_order) {
    std::vector<size_t> layer_indices;
    
    for (size_t i = 0; i < field_order.size(); ++i) {
        if (required_fields.find(field_order[i].name) != required_fields.end()) {
            layer_indices.push_back(i);
        }
    }
    
    // 去重并排序
    std::sort(layer_indices.begin(), layer_indices.end());
    layer_indices.erase(std::unique(layer_indices.begin(), layer_indices.end()), layer_indices.end());
    
    return layer_indices;
}

DecompressionOptions SelectiveDecompressor::createOptions(const FieldAnalysis& analysis) {
    DecompressionOptions options;
    
    // 基于分析结果设置解压选项
    options.required_fields = analysis.required_fields;
    
    // 确定需要解压的类型
    for (const auto& field_type : analysis.field_types) {
        switch (field_type.second) {
            case FieldType::String:
                options.decompress_string_dict = true;
                break;
            case FieldType::Timestamp:
                options.decompress_timestamp_dict = true;
                break;
            case FieldType::LogType:
                options.decompress_logtype_dict = true;
                break;
            default:
                options.decompress_string_dict = true;
                break;
        }
    }
    
    // 如果没有特定字段要求，解压所有字典
    if (analysis.required_fields.empty()) {
        options.decompress_string_dict = true;
        options.decompress_timestamp_dict = true;
        options.decompress_logtype_dict = true;
    }
    
    // 设置部分解压
    options.partial_decompression = !analysis.required_fields.empty();
    
    return options;
}

bool SelectiveDecompressor::needsFieldDecompression(const std::string& field_name, 
                                                   const DecompressionOptions& options) const {
    if (options.required_fields.empty()) {
        return true;
    }
    
    return options.required_fields.find(field_name) != options.required_fields.end();
}

bool SelectiveDecompressor::needsTypeDecompression(FieldType field_type, 
                                                  const DecompressionOptions& options) const {
    switch (field_type) {
        case FieldType::String:
            return options.decompress_string_dict;
        case FieldType::Timestamp:
            return options.decompress_timestamp_dict;
        case FieldType::LogType:
            return options.decompress_logtype_dict;
        default:
            return options.decompress_string_dict;
    }
}

bool SelectiveDecompressor::validateDecompressionResult(const DecompressionResult& result,
                                                       const DecompressionOptions& options) {
    // 验证Trie
    if (options.decompress_trie && !result.louds) {
        return false;
    }
    
    // 验证字典管理器
    if ((options.decompress_string_dict || options.decompress_timestamp_dict || options.decompress_logtype_dict) 
        && !result.dict_manager) {
        return false;
    }
    
    // 验证字段顺序
    if (result.field_order.empty()) {
        return false;
    }
    
    // 验证可用字段
    if (options.partial_decompression && !options.required_fields.empty()) {
        for (const auto& field : options.required_fields) {
            if (result.available_fields.find(field) == result.available_fields.end()) {
                return false;
            }
        }
    }
    
    return true;
}

// ========== 新增：适配新查询思路的实现 ==========

bool SelectiveDecompressor::fieldExistsInChunk(const GranularCompressedData& granular_data,
                                              const std::string& field_name) {
    try {
        // 解压元数据以获取字段顺序
        std::vector<uint8_t> metadata_raw = Compressor::decompressWithZstd(granular_data.metadata);
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        
        // 检查字段是否存在
        for (const auto& field_key : field_order) {
            if (field_key.name == field_name) {
                return true;
            }
        }
        
        return false;
    } catch (const std::exception& e) {
        return false;
    }
}

bool SelectiveDecompressor::decompressFieldDictionaryForQuery(const GranularCompressedData& granular_data,
                                                             const std::string& field_name,
                                                             FieldType field_type,
                                                             FieldDictionaryManager& manager) {
    try {
        // INT, FLOAT, BOOL 不用解压任何字典，因为它们存的是原始值
        switch (field_type) {
            case FieldType::Int:
            case FieldType::Double:
            case FieldType::Bool:
                // 这些类型存储原始值，不需要字典
                return true;
            case FieldType::String:
            case FieldType::UnstructuredArray: {
                if (!granular_data.string_dict.empty()) {
                    std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(granular_data.string_dict);
                    Compressor::deserializeStringDictionary(dict_raw, manager);
                    return true;
                }
                break;
            }
            case FieldType::Timestamp: {
                if (!granular_data.timestamp_dict.empty()) {
                    std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(granular_data.timestamp_dict);
                    Compressor::deserializeTimestampDictionary(dict_raw, manager);
                    return true;
                }
                break;
            }
            case FieldType::LogType: {
                if (!granular_data.logtype_dict.empty()) {
                    std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(granular_data.logtype_dict);
                    Compressor::deserializeLogTypeDictionary(dict_raw, manager);
                    return true;
                }
                break;
            }
            default: {
                // 默认解压字符串字典
                if (!granular_data.string_dict.empty()) {
                    std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(granular_data.string_dict);
                    Compressor::deserializeStringDictionary(dict_raw, manager);
                    return true;
                }
                break;
            }
        }
        
        return false;
    } catch (const std::exception& e) {
        return false;
    }
}

bool SelectiveDecompressor::decompressLayerForQuery(const GranularCompressedData& granular_data,
                                                   size_t layer_index,
                                                   LOUDSTrie& louds) {
    try {
        // 检查层索引是否有效
        if (layer_index >= granular_data.layer_data_by_level.size()) {
            return false;
        }
        
        // 解压层数据
        std::vector<uint8_t> layer_raw = Compressor::decompressWithZstd(granular_data.layer_data_by_level[layer_index]);
        
        // 反序列化层数据
        std::istringstream iss(std::string(layer_raw.begin(), layer_raw.end()));
        louds.getLayeredStorage().deserializeLayer(layer_index, iss);
        
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

bool SelectiveDecompressor::decompressLoudsAndLayerSizesForQuery(const GranularCompressedData& granular_data,
                                                                LOUDSTrie& louds,
                                                                std::vector<uint32_t>& layer_sizes) {
    try {
        // 1. 解压LOUDS位图（结构信息）
        std::vector<uint8_t> bitmap_raw = Compressor::decompressWithZstd(granular_data.trie_bitmap);
        std::istringstream iss(std::string(bitmap_raw.begin(), bitmap_raw.end()));
        louds.deserializeBitmap(iss);
        
        // 2. 解压层大小信息（如果存在）
        if (!granular_data.layer_sizes.empty()) {
            std::vector<uint8_t> layer_sizes_raw = Compressor::decompressWithZstd(granular_data.layer_sizes);
            if (layer_sizes_raw.size() >= sizeof(uint32_t)) {
                size_t layer_count = layer_sizes_raw.size() / sizeof(uint32_t);
                layer_sizes.resize(layer_count);
                std::memcpy(layer_sizes.data(), layer_sizes_raw.data(), layer_count * sizeof(uint32_t));
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

// ========== 私有辅助方法实现 ==========

std::unique_ptr<LOUDSTrie> SelectiveDecompressor::decompressLoudsStructure(const std::vector<uint8_t>& trie_bitmap) {
    try {
        // 解压位图数据
        std::vector<uint8_t> bitmap_raw = Compressor::decompressWithZstd(trie_bitmap);
        
        // 创建LOUDS结构
        auto louds = std::make_unique<LOUDSTrie>();
        
        // 从解压的数据中重建LOUDS位图
        std::istringstream iss(std::string(bitmap_raw.begin(), bitmap_raw.end()));
        louds->deserializeBitmap(iss);
        
        return louds;
    } catch (const std::exception& e) {
        return nullptr;
    }
}

bool SelectiveDecompressor::loadLayer(LOUDSTrie& louds, size_t layer_idx, const std::vector<uint8_t>& layer_data) {
    try {
        // 解压层数据
        std::vector<uint8_t> layer_raw = Compressor::decompressWithZstd(layer_data);
        
        // 反序列化层数据
        std::istringstream iss(std::string(layer_raw.begin(), layer_raw.end()));
        louds.getLayeredStorage().deserializeLayer(layer_idx, iss);
        
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

void SelectiveDecompressor::loadStringDict(FieldDictionaryManager& manager, const std::vector<uint8_t>& string_dict) {
    try {
        // 解压字符串字典
        if (!string_dict.empty()) {
            std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(string_dict);
            
            // 反序列化字符串字典
            Compressor::deserializeStringDictionary(dict_raw, manager);
        }
    } catch (const std::exception& e) {
        // 忽略错误，继续处理
    }
}

void SelectiveDecompressor::loadTimestampDict(FieldDictionaryManager& manager, const std::vector<uint8_t>& timestamp_dict) {
    try {
        // 解压时间戳字典
        if (!timestamp_dict.empty()) {
            std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(timestamp_dict);
            
            // 反序列化时间戳字典
            Compressor::deserializeTimestampDictionary(dict_raw, manager);
        }
    } catch (const std::exception& e) {
        // 忽略错误，继续处理
    }
}

void SelectiveDecompressor::loadLogTypeDict(FieldDictionaryManager& manager, const std::vector<uint8_t>& logtype_dict) {
    try {
        // 解压日志类型字典
        if (!logtype_dict.empty()) {
            std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(logtype_dict);
            
            // 反序列化日志类型字典
            Compressor::deserializeLogTypeDictionary(dict_raw, manager);
        }
    } catch (const std::exception& e) {
        // 忽略错误，继续处理
    }
}

bool SelectiveDecompressor::decompressLayerSizes(const std::vector<uint8_t>& layer_sizes_data,
                                                std::vector<uint32_t>& layer_sizes) {
    try {
        if (layer_sizes_data.empty()) {
            return false;
        }
        
        // 解压层大小信息
        std::vector<uint8_t> layer_sizes_raw = Compressor::decompressWithZstd(layer_sizes_data);
        
        // 反序列化层大小信息
        if (layer_sizes_raw.size() >= sizeof(uint32_t)) {
            size_t layer_count = layer_sizes_raw.size() / sizeof(uint32_t);
            layer_sizes.resize(layer_count);
            std::memcpy(layer_sizes.data(), layer_sizes_raw.data(), layer_count * sizeof(uint32_t));
            return true;
        }
        
        return false;
    } catch (const std::exception& e) {
        return false;
    }
}

} // namespace query
} // namespace json2