#include "layer_compressor.h"
#include "../core/compression_utils.h"
#include "../backends/zstd_backend.h"
#include "../algorithms/rle_compression.h"
#include <algorithm>
#include <numeric>

namespace json2 {
namespace compression {
namespace type_aware {

std::vector<uint8_t> LayerCompressor::compressLayers(const std::vector<std::vector<uint8_t>>& layers) {
    if (layers.empty()) return {};
    
    // 分析层次数据特征
    LayerStats stats = analyzeLayers(layers);
    
    // 选择压缩策略
    LayerCompressionType strategy = selectCompressionStrategy(layers);
    
    std::vector<uint8_t> result;
    utils::SerializationUtils::writeValue(result, static_cast<uint8_t>(strategy));
    
    // 根据策略执行压缩
    switch (strategy) {
        case LayerCompressionType::INDIVIDUAL:
            {
                std::vector<uint8_t> compressed = compressIndividualLayers(layers);
                result.insert(result.end(), compressed.begin(), compressed.end());
                break;
            }
        case LayerCompressionType::COMBINED:
            {
                std::vector<uint8_t> compressed = compressCombinedLayers(layers);
                result.insert(result.end(), compressed.begin(), compressed.end());
                break;
            }
        case LayerCompressionType::HIERARCHICAL:
            {
                std::vector<uint8_t> compressed = compressHierarchicalLayers(layers);
                result.insert(result.end(), compressed.begin(), compressed.end());
                break;
            }
    }
    
    return result;
}

std::vector<std::vector<uint8_t>> LayerCompressor::decompressLayers(const std::vector<uint8_t>& compressed) {
    if (compressed.empty()) return {};
    
    size_t pos = 0;
    LayerCompressionType strategy = static_cast<LayerCompressionType>(
        utils::SerializationUtils::readValue<uint8_t>(compressed, pos));
    
    switch (strategy) {
        case LayerCompressionType::INDIVIDUAL:
            return decompressIndividualLayers(compressed, pos);
        case LayerCompressionType::COMBINED:
            return decompressCombinedLayers(compressed, pos);
        case LayerCompressionType::HIERARCHICAL:
            return decompressHierarchicalLayers(compressed, pos);
        default:
            throw std::runtime_error("Unknown layer compression strategy");
    }
}

std::vector<uint8_t> LayerCompressor::compressObjectLayers(const std::unordered_map<std::string, std::vector<uint8_t>>& object_data) {
    std::vector<uint8_t> result;
    
    // 序列化键值对数量
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(object_data.size()));
    
    // 分离键和值
    std::vector<std::string> keys;
    std::vector<std::vector<uint8_t>> values;
    
    for (const auto& pair : object_data) {
        keys.push_back(pair.first);
        values.push_back(pair.second);
    }
    
    // 压缩键（字符串数组）
    std::vector<uint8_t> keys_data;
    utils::SerializationUtils::writeStringVector(keys_data, keys);
    
    backends::ZstdBackend backend;
    std::vector<uint8_t> compressed_keys = backend.compress(keys_data);
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed_keys.size()));
    result.insert(result.end(), compressed_keys.begin(), compressed_keys.end());
    
    // 压缩值（分层数据）
    std::vector<uint8_t> compressed_values = compressLayers(values);
    result.insert(result.end(), compressed_values.begin(), compressed_values.end());
    
    return result;
}

std::unordered_map<std::string, std::vector<uint8_t>> LayerCompressor::decompressObjectLayers(const std::vector<uint8_t>& compressed) {
    std::unordered_map<std::string, std::vector<uint8_t>> result;
    
    size_t pos = 0;
    
    // 读取键值对数量
    uint32_t pair_count = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    
    // 解压键
    uint32_t keys_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<uint8_t> compressed_keys(compressed.begin() + pos, compressed.begin() + pos + keys_size);
    pos += keys_size;
    
    backends::ZstdBackend backend;
    std::vector<uint8_t> keys_data = backend.decompress(compressed_keys);
    
    size_t keys_pos = 0;
    std::vector<std::string> keys = utils::SerializationUtils::readStringVector(keys_data, keys_pos);
    
    // 解压值
    std::vector<uint8_t> compressed_values(compressed.begin() + pos, compressed.end());
    std::vector<std::vector<uint8_t>> values = decompressLayers(compressed_values);
    
    // 重建对象
    for (size_t i = 0; i < pair_count && i < keys.size() && i < values.size(); ++i) {
        result[keys[i]] = values[i];
    }
    
    return result;
}

std::vector<uint8_t> LayerCompressor::compressArrayLayers(const std::vector<std::vector<uint8_t>>& array_data) {
    // 数组压缩与普通分层压缩相同
    return compressLayers(array_data);
}

std::vector<std::vector<uint8_t>> LayerCompressor::decompressArrayLayers(const std::vector<uint8_t>& compressed) {
    // 数组解压与普通分层解压相同
    return decompressLayers(compressed);
}

LayerCompressor::LayerStats LayerCompressor::analyzeLayers(const std::vector<std::vector<uint8_t>>& layers) {
    LayerStats stats = {};
    
    stats.layer_count = layers.size();
    if (layers.empty()) return stats;
    
    std::vector<size_t> layer_sizes;
    size_t total_size = 0;
    
    for (const auto& layer : layers) {
        size_t size = layer.size();
        layer_sizes.push_back(size);
        total_size += size;
    }
    
    stats.avg_layer_size = total_size / layers.size();
    stats.max_layer_size = *std::max_element(layer_sizes.begin(), layer_sizes.end());
    stats.min_layer_size = *std::min_element(layer_sizes.begin(), layer_sizes.end());
    
    // 简单估算压缩效率
    stats.compression_efficiency = 0.7; // 默认估值
    
    return stats;
}

CompressionBackend LayerCompressor::selectOptimalLayerBackend(const std::vector<std::vector<uint8_t>>& layers) {
    LayerStats stats = analyzeLayers(layers);
    
    // 根据层次特征选择后端
    if (stats.layer_count > 10 && stats.avg_layer_size > 1000) {
        return CompressionBackend::ZSTD; // 大数据集
    } else {
        return CompressionBackend::ZSTD; // 默认
    }
}

LayerCompressor::LayerCompressionType LayerCompressor::selectCompressionStrategy(const std::vector<std::vector<uint8_t>>& layers) {
    LayerStats stats = analyzeLayers(layers);
    
    if (stats.layer_count <= 3) {
        return LayerCompressionType::INDIVIDUAL; // 少量层次，独立压缩
    } else if (stats.max_layer_size > stats.avg_layer_size * 3) {
        return LayerCompressionType::HIERARCHICAL; // 大小差异很大，层次化压缩
    } else {
        return LayerCompressionType::COMBINED; // 合并压缩
    }
}

std::vector<uint8_t> LayerCompressor::compressIndividualLayers(const std::vector<std::vector<uint8_t>>& layers) {
    std::vector<uint8_t> result;
    
    // 写入层数
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(layers.size()));
    
    backends::ZstdBackend backend;
    
    for (const auto& layer : layers) {
        std::vector<uint8_t> compressed = backend.compress(layer);
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed.size()));
        result.insert(result.end(), compressed.begin(), compressed.end());
    }
    
    return result;
}

std::vector<uint8_t> LayerCompressor::compressCombinedLayers(const std::vector<std::vector<uint8_t>>& layers) {
    std::vector<uint8_t> result;
    
    // 合并所有层次数据
    std::vector<uint8_t> combined = serializeLayers(layers);
    
    // 整体压缩
    backends::ZstdBackend backend;
    std::vector<uint8_t> compressed = backend.compress(combined);
    result.insert(result.end(), compressed.begin(), compressed.end());
    
    return result;
}

std::vector<uint8_t> LayerCompressor::compressHierarchicalLayers(const std::vector<std::vector<uint8_t>>& layers) {
    std::vector<uint8_t> result;
    
    // 分级压缩：先压缩小层，再压缩大层
    std::vector<std::pair<size_t, size_t>> layer_indices; // (索引, 大小)
    for (size_t i = 0; i < layers.size(); ++i) {
        layer_indices.emplace_back(i, layers[i].size());
    }
    
    // 按大小排序
    std::sort(layer_indices.begin(), layer_indices.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    
    // 写入排序后的索引
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(layers.size()));
    for (const auto& pair : layer_indices) {
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(pair.first));
    }
    
    // 按大小顺序压缩
    backends::ZstdBackend backend;
    for (const auto& pair : layer_indices) {
        const auto& layer = layers[pair.first];
        std::vector<uint8_t> compressed = backend.compress(layer);
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(compressed.size()));
        result.insert(result.end(), compressed.begin(), compressed.end());
    }
    
    return result;
}

std::vector<std::vector<uint8_t>> LayerCompressor::decompressIndividualLayers(const std::vector<uint8_t>& compressed, size_t& pos) {
    uint32_t layer_count = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    std::vector<std::vector<uint8_t>> layers(layer_count);
    
    backends::ZstdBackend backend;
    
    for (uint32_t i = 0; i < layer_count; ++i) {
        uint32_t compressed_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
        std::vector<uint8_t> layer_compressed(compressed.begin() + pos, compressed.begin() + pos + compressed_size);
        pos += compressed_size;
        
        layers[i] = backend.decompress(layer_compressed);
    }
    
    return layers;
}

std::vector<std::vector<uint8_t>> LayerCompressor::decompressCombinedLayers(const std::vector<uint8_t>& compressed, size_t& pos) {
    backends::ZstdBackend backend;
    std::vector<uint8_t> combined = backend.decompress(
        std::vector<uint8_t>(compressed.begin() + pos, compressed.end()));
    
    size_t deserialize_pos = 0;
    return deserializeLayers(combined, deserialize_pos);
}

std::vector<std::vector<uint8_t>> LayerCompressor::decompressHierarchicalLayers(const std::vector<uint8_t>& compressed, size_t& pos) {
    uint32_t layer_count = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    
    // 读取索引顺序
    std::vector<uint32_t> indices(layer_count);
    for (uint32_t i = 0; i < layer_count; ++i) {
        indices[i] = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
    }
    
    // 解压数据
    std::vector<std::vector<uint8_t>> sorted_layers(layer_count);
    backends::ZstdBackend backend;
    
    for (uint32_t i = 0; i < layer_count; ++i) {
        uint32_t compressed_size = utils::SerializationUtils::readValue<uint32_t>(compressed, pos);
        std::vector<uint8_t> layer_compressed(compressed.begin() + pos, compressed.begin() + pos + compressed_size);
        pos += compressed_size;
        
        sorted_layers[i] = backend.decompress(layer_compressed);
    }
    
    // 恢复原始顺序
    std::vector<std::vector<uint8_t>> layers(layer_count);
    for (uint32_t i = 0; i < layer_count; ++i) {
        layers[indices[i]] = sorted_layers[i];
    }
    
    return layers;
}

std::vector<uint8_t> LayerCompressor::serializeLayers(const std::vector<std::vector<uint8_t>>& layers) {
    std::vector<uint8_t> result;
    
    utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(layers.size()));
    
    for (const auto& layer : layers) {
        utils::SerializationUtils::writeValue(result, static_cast<uint32_t>(layer.size()));
        result.insert(result.end(), layer.begin(), layer.end());
    }
    
    return result;
}

std::vector<std::vector<uint8_t>> LayerCompressor::deserializeLayers(const std::vector<uint8_t>& data, size_t& pos) {
    uint32_t layer_count = utils::SerializationUtils::readValue<uint32_t>(data, pos);
    std::vector<std::vector<uint8_t>> layers(layer_count);
    
    for (uint32_t i = 0; i < layer_count; ++i) {
        uint32_t layer_size = utils::SerializationUtils::readValue<uint32_t>(data, pos);
        layers[i] = std::vector<uint8_t>(data.begin() + pos, data.begin() + pos + layer_size);
        pos += layer_size;
    }
    
    return layers;
}

} // namespace type_aware
} // namespace compression
} // namespace json2