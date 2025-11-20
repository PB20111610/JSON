#include "../include/query/selective_decompressor.h"
#include "../include/compress.h"
#include "../include/compress_type_aware.h"
#include "../include/compression/algorithms/delta_compression.h"
#include "../include/compression/algorithms/bitpacking_compression.h"
#include "../include/compression/algorithms/varint_compression.h"
#include "../include/compression/algorithms/rle_compression.h"
#include "../include/compression/factory/compression_factory.h"
#include "../include/compression/core/compression_utils.h"
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
            case FieldType::STRING:
                options.decompress_string_dict = true;
                break;
            case FieldType::TIMESTAMP:
                options.decompress_timestamp_dict = true;
                break;
            case FieldType::LOGTYPE:
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
        case FieldType::STRING:
            return options.decompress_string_dict;
        case FieldType::TIMESTAMP:
            return options.decompress_timestamp_dict;
        case FieldType::LOGTYPE:
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

// ========== 适配新查询思路的实现 ==========

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
            case FieldType::INT64:
            case FieldType::DOUBLE:
            case FieldType::BOOL:
                // 这些类型存储原始值，不需要字典
                return true;
            case FieldType::STRING:
            case FieldType::ARRAY: {
                if (!granular_data.string_dict.empty()) {
                    std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(granular_data.string_dict);
                    Compressor::deserializeStringDictionary(dict_raw, manager);
                    return true;
                }
                break;
            }
            case FieldType::TIMESTAMP: {
                if (!granular_data.timestamp_dict.empty()) {
                    std::vector<uint8_t> dict_raw = Compressor::decompressWithZstd(granular_data.timestamp_dict);
                    Compressor::deserializeTimestampDictionary(dict_raw, manager);
                    return true;
                }
                break;
            }
            case FieldType::LOGTYPE: {
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

// ========== 新增：部分解压层值的实现 ==========

NodeValue SelectiveDecompressor::getLayerValueAt(const GranularCompressedData& granular_data,
                                                size_t layer_index,
                                                size_t node_index_in_layer,
                                                const std::vector<FieldKey>& field_order,
                                                const compression::TypeAwareCompressionConfig& config,
                                                const std::vector<uint32_t>* layer_sizes) {
    try {
        // 检查层索引是否有效
        if (layer_index >= granular_data.layer_data_by_level.size()) {
            throw std::out_of_range("Layer index out of range");
        }
        
        // 获取字段类型
        compression::FieldType field_type = compression::FieldType::STRING; // Default
        if (layer_index < field_order.size()) {
            field_type = mapJsonFieldTypeToCompressionType(field_order[layer_index].type);
        }
        
        // 获取压缩的层数据
        const std::vector<uint8_t>& compressed_layer = granular_data.layer_data_by_level[layer_index];
        
        // 获取层大小信息（如果可用）
        size_t layer_size = 0;
        if (layer_sizes && layer_index < layer_sizes->size()) {
            layer_size = (*layer_sizes)[layer_index];
        } else if (!granular_data.layer_sizes.empty()) {
            // 解压层大小信息
            std::vector<uint8_t> layer_sizes_raw = Compressor::decompressWithZstd(granular_data.layer_sizes);
            if (layer_sizes_raw.size() >= sizeof(uint32_t) * (layer_index + 1)) {
                // 从解压的数据中读取指定层的大小
                const uint32_t* layer_sizes_data = reinterpret_cast<const uint32_t*>(layer_sizes_raw.data());
                layer_size = layer_sizes_data[layer_index];
            }
        }
        
        // 使用配置驱动的部分解压方法
        return decompressLayerValueAt(compressed_layer, node_index_in_layer, field_type, config, layer_size);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to get layer value at index: " + std::string(e.what()));
    }
}

NodeValue SelectiveDecompressor::decompressLayerValueAt(const std::vector<uint8_t>& compressed_layer,
                                                       size_t node_index_in_layer,
                                                       compression::FieldType field_type,
                                                       const compression::TypeAwareCompressionConfig& config,
                                                       size_t layer_size) {
    try {
        // 首先解压配置头以确定使用的压缩算法
        if (compressed_layer.size() < 2) {
            throw std::runtime_error("Compressed layer data too small");
        }
        
        uint8_t compression_flag = compressed_layer[0];
        
        if (compression_flag == 0) {
            // 数据未压缩，直接处理原始数据
            std::vector<uint8_t> raw_data(compressed_layer.begin() + 1, compressed_layer.end());
            return extractNodeValueFromRawData(raw_data, node_index_in_layer);
        } else if (compression_flag == 1) {
            // 数据已压缩，先解压该层数据，然后从解压后的数据中提取节点值
            if (compressed_layer.size() < 3) {
                throw std::runtime_error("Compressed layer data too small for header");
            }
            
            compression::CompressionBackend backend = static_cast<compression::CompressionBackend>(compressed_layer[1]);
            std::vector<uint8_t> compressed_payload(compressed_layer.begin() + 2, compressed_layer.end());
            
            // 创建相应的解压器并解压整个数据
            auto decompressor = compression::factory::CompressionFactory::createCompressor(backend);
            if (!decompressor) {
                throw std::runtime_error("Failed to create decompressor for backend: " + std::to_string(static_cast<int>(backend)));
            }
            
            std::vector<uint8_t> decompressed_data = decompressor->decompress(compressed_payload);
            return extractNodeValueFromRawData(decompressed_data, node_index_in_layer);
            // 根据后端类型和字段类型使用相应的*At方法进行部分解压
            // return extractNodeValueUsingCompressionAt(compressed_payload, node_index_in_layer, backend, field_type, layer_size);
        } else {
            throw std::runtime_error("Invalid compression flag: " + std::to_string(compression_flag));
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to decompress layer value at index: " + std::string(e.what()));
    }
}

NodeValue SelectiveDecompressor::extractNodeValueUsingCompressionAt(
    const std::vector<uint8_t>& compressed_data,
    size_t node_index_in_layer,
    compression::CompressionBackend backend,
    compression::FieldType field_type,
    size_t layer_size) {
    
    // 根据后端类型选择合适的*At方法
    switch (backend) {
        case compression::CompressionBackend::DELTA_VARINT: {
            // 对于DELTA_VARINT，使用DeltaCompression的*At方法进行部分解压
            switch (field_type) {
                case compression::FieldType::INT64: {
                    // 使用DeltaCompression的deltaVarintDecompressAt静态方法进行部分解压
                    // 这适用于直接由deltaVarintCompress压缩的数据
                    return compression::algorithms::DeltaCompression::deltaVarintDecompressAt(compressed_data, node_index_in_layer);
                }
                case compression::FieldType::UINT32: {  // String 编码值
                    // 使用DeltaCompression的decompressUint32At虚方法进行部分解压
                    compression::algorithms::DeltaCompression deltaCompressor;
                    return deltaCompressor.decompressUint32At(compressed_data, node_index_in_layer);
                }
                case compression::FieldType::DOUBLE: {
                    // 使用DeltaCompression的decompressDoubleAt虚方法进行部分解压
                    compression::algorithms::DeltaCompression deltaCompressor;
                    return deltaCompressor.decompressDoubleAt(compressed_data, node_index_in_layer);
                }
                case compression::FieldType::TIMESTAMP: {
                    // For TIMESTAMP fields, use the new efficient random access method
                    return compression::algorithms::DeltaCompression::deltaVarintDecompressTimestampAt(compressed_data, node_index_in_layer);
                }
                case compression::FieldType::LOGTYPE: {
                    // For LOGTYPE fields, use the new efficient random access method
                    return compression::algorithms::DeltaCompression::deltaVarintDecompressLogtypeAt(compressed_data, node_index_in_layer);
                }
                default: {
                    // 对于其他类型，回退到完整解压方法
                    std::cout << "DEBUG: Using full decompression for " << static_cast<int>(field_type) << std::endl;
                    std::vector<int64_t> decompressed = compression::algorithms::DeltaCompression::deltaVarintDecompress(compressed_data);
                    std::vector<uint8_t> raw_data = compression::utils::SerializationUtils::int64sToBytes(decompressed);
                    return extractNodeValueFromRawData(raw_data, node_index_in_layer);
                }
            }
        }
        
        case compression::CompressionBackend::BIT_PACKING: {
            // 对于BIT_PACKING，使用BitPackingCompression的decompressBoolAt方法
            if (field_type == compression::FieldType::BOOL) {
                // For bit packing, we need the actual count from layer size
                size_t count = layer_size > 0 ? layer_size : compressed_data.size() * 8;
                return compression::algorithms::BitPackingCompression::bitPackingDecompressAt(
                    compressed_data, node_index_in_layer, count);
            } else {
                // 对于其他类型，回退到完整解压方法
                size_t count = layer_size > 0 ? layer_size : compressed_data.size() * 8;
                std::vector<bool> decompressed = compression::algorithms::BitPackingCompression::bitPackingDecompress(
                    compressed_data, count);
                std::vector<uint8_t> raw_data = compression::utils::SerializationUtils::boolsToBytes(decompressed);
                return extractNodeValueFromRawData(raw_data, node_index_in_layer);
            }
        }
        
        case compression::CompressionBackend::DELTA_DELTA: {
            // 对于DELTA_DELTA，使TIMESTAMP字段与LOGTYPE字段使用相同的处理方式
            switch (field_type) {
                case compression::FieldType::TIMESTAMP:
                case compression::FieldType::LOGTYPE: {
                    // TIMESTAMP和LOGTYPE字段使用相同的处理方式
                    std::vector<int64_t> decompressed = compression::algorithms::DeltaDeltaCompression::deltaDeltaDecompress(compressed_data);
                    std::vector<uint8_t> raw_data = compression::utils::SerializationUtils::int64sToBytes(decompressed);
                    return extractNodeValueFromRawData(raw_data, node_index_in_layer);
                }
                default: {
                    // 对于其他类型，回退到完整解压方法
                    std::vector<int64_t> decompressed = compression::algorithms::DeltaDeltaCompression::deltaDeltaDecompress(
                        compressed_data);
                    std::vector<uint8_t> raw_data = compression::utils::SerializationUtils::int64sToBytes(decompressed);
                    return extractNodeValueFromRawData(raw_data, node_index_in_layer);
                }
            }
        }
        
        case compression::CompressionBackend::RLE: {
            // 对于RLE，使用RLECompression的rleDecompressAt方法
            uint8_t value = compression::algorithms::RLECompression::rleDecompressAt(
                compressed_data, node_index_in_layer);
            return static_cast<uint32_t>(value); // RLE通常用于uint8_t数据
        }
        
        default: {
            // 对于其他后端或不支持部分解压的算法，回退到完整解压方法
            auto compressor = compression::factory::CompressionFactory::createCompressor(backend);
            if (!compressor) {
                throw std::runtime_error("Failed to create compressor for backend: " + std::to_string(static_cast<int>(backend)));
            }
            
            std::vector<uint8_t> decompressed_data = compressor->decompress(compressed_data);
            return extractNodeValueFromRawData(decompressed_data, node_index_in_layer);
        }
    }
}

NodeValue SelectiveDecompressor::extractNodeValueFromRawData(
    const std::vector<uint8_t>& raw_data,
    size_t node_index_in_layer) {
    
    // 解析原始数据以获取指定索引的值
    std::istringstream layer_stream(std::string(raw_data.begin(), raw_data.end()), std::ios::binary);
    
    // 读取节点数量
    size_t node_count;
    layer_stream.read(reinterpret_cast<char*>(&node_count), sizeof(node_count));
    
    // 检查索引是否有效
    if (node_index_in_layer >= node_count) {
        throw std::out_of_range("Node index out of range");
    }
    
    // 跳转到指定节点
    for (size_t i = 0; i < node_index_in_layer; ++i) {
        // 读取类型字节
        uint8_t type_byte;
        layer_stream.read(reinterpret_cast<char*>(&type_byte), 1);
        
        // 根据类型跳过数据
        switch (type_byte) {
            case static_cast<uint8_t>(NodeValueType::UINT32): { // uint32_t
                uint32_t value;
                layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                break;
            }
            case static_cast<uint8_t>(NodeValueType::INT64): { // int64_t
                int64_t value;
                layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                break;
            }
            case static_cast<uint8_t>(NodeValueType::DOUBLE): { // double
                double value;
                layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                break;
            }
            case static_cast<uint8_t>(NodeValueType::BOOL): { // bool
                bool value;
                layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
                break;
            }
            case static_cast<uint8_t>(NodeValueType::NULLPTR): { // nullptr
                break;
            }
            case static_cast<uint8_t>(NodeValueType::TEMPLATE_ENCODED_TIMESTAMP): { // TemplateEncodedTimestamp
                uint32_t template_id;
                layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
                uint32_t n;
                layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
                for (uint32_t j = 0; j < n; ++j) {
                    uint32_t code;
                    layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
                }
                break;
            }
            case static_cast<uint8_t>(NodeValueType::ENCODED_LOG): { // EncodedLog
                uint32_t template_id;
                layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
                uint32_t n;
                layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
                for (uint32_t j = 0; j < n; ++j) {
                    uint32_t code;
                    layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
                }
                break;
            }
            default:
                throw std::runtime_error("Unknown node type: " + std::to_string(type_byte));
        }
    }
    
    // 读取目标节点的类型字节
    uint8_t type_byte;
    layer_stream.read(reinterpret_cast<char*>(&type_byte), 1);
    
    // 根据类型读取并返回值
    switch (type_byte) {
        case static_cast<uint8_t>(NodeValueType::UINT32): { // uint32_t
            uint32_t value;
            layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
            return value;
        }
        case static_cast<uint8_t>(NodeValueType::INT64): { // int64_t
            int64_t value;
            layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
            return value;
        }
        case static_cast<uint8_t>(NodeValueType::DOUBLE): { // double
            double value;
            layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
            return value;
        }
        case static_cast<uint8_t>(NodeValueType::BOOL): { // bool
            bool value;
            layer_stream.read(reinterpret_cast<char*>(&value), sizeof(value));
            return value;
        }
        case static_cast<uint8_t>(NodeValueType::NULLPTR): { // nullptr
            return std::nullptr_t{};
        }
        case static_cast<uint8_t>(NodeValueType::TEMPLATE_ENCODED_TIMESTAMP): { // TemplateEncodedTimestamp
            uint32_t template_id;
            layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
            uint32_t n;
            layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
            std::vector<uint32_t> var_codes(n);
            for (uint32_t& code : var_codes) {
                layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
            }
            return TemplateEncodedTimestamp{template_id, std::move(var_codes)};
        }
        case static_cast<uint8_t>(NodeValueType::ENCODED_LOG): { // EncodedLog
            uint32_t template_id;
            layer_stream.read(reinterpret_cast<char*>(&template_id), sizeof(template_id));
            uint32_t n;
            layer_stream.read(reinterpret_cast<char*>(&n), sizeof(n));
            std::vector<uint32_t> var_codes(n);
            for (uint32_t& code : var_codes) {
                layer_stream.read(reinterpret_cast<char*>(&code), sizeof(code));
            }
            return EncodedLog{template_id, std::move(var_codes)};
        }
        default:
            throw std::runtime_error("Unknown node type: " + std::to_string(type_byte));
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

// ========== 私有辅助方法：类型映射 ==========

compression::FieldType SelectiveDecompressor::mapJsonFieldTypeToCompressionType(FieldType json_field_type) const {
    switch (json_field_type) {
        case FieldType::INT64:
            return compression::FieldType::INT64;
        case FieldType::DOUBLE:
            return compression::FieldType::DOUBLE;
        case FieldType::BOOL:
            return compression::FieldType::BOOL;
        case FieldType::STRING:
            return compression::FieldType::STRING;
        case FieldType::TIMESTAMP:
            return compression::FieldType::TIMESTAMP;
        case FieldType::LOGTYPE:
            return compression::FieldType::LOGTYPE;
        case FieldType::ARRAY:
            return compression::FieldType::ARRAY;
        case FieldType::NULL_TYPE:
            return compression::FieldType::NULL_TYPE;
        default:
            return compression::FieldType::STRING; // Safe default
    }
}

} // namespace query
} // namespace json2