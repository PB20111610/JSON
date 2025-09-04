#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_backend.h"
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace json2 {
namespace compression {
namespace type_aware {

/**
 * 分层数据压缩器
 * 处理嵌套结构数据的压缩，如JSON对象和数组
 */
class LayerCompressor {
public:
    LayerCompressor() = default;
    virtual ~LayerCompressor() = default;
    
    /**
     * 分层压缩数据
     * @param layers 分层数据，每一层代表一个嵌套级别
     * @return 压缩后的数据
     */
    std::vector<uint8_t> compressLayers(const std::vector<std::vector<uint8_t>>& layers);
    
    /**
     * 分层解压数据
     * @param compressed 压缩的数据
     * @return 解压后的分层数据
     */
    std::vector<std::vector<uint8_t>> decompressLayers(const std::vector<uint8_t>& compressed);
    
    /**
     * 压缩JSON对象层次结构
     * @param object_data 对象数据，按层次组织
     * @return 压缩后的数据
     */
    std::vector<uint8_t> compressObjectLayers(const std::unordered_map<std::string, std::vector<uint8_t>>& object_data);
    
    /**
     * 解压JSON对象层次结构
     * @param compressed 压缩的数据
     * @return 解压后的对象数据
     */
    std::unordered_map<std::string, std::vector<uint8_t>> decompressObjectLayers(const std::vector<uint8_t>& compressed);
    
    /**
     * 压缩JSON数组层次结构
     * @param array_data 数组数据，按层次组织
     * @return 压缩后的数据
     */
    std::vector<uint8_t> compressArrayLayers(const std::vector<std::vector<uint8_t>>& array_data);
    
    /**
     * 解压JSON数组层次结构
     * @param compressed 压缩的数据
     * @return 解压后的数组数据
     */
    std::vector<std::vector<uint8_t>> decompressArrayLayers(const std::vector<uint8_t>& compressed);
    
    // 层次分析
    struct LayerStats {
        size_t layer_count;
        size_t avg_layer_size;
        size_t max_layer_size;
        size_t min_layer_size;
        double compression_efficiency;
        std::vector<double> layer_compression_ratios;
    };
    
    /**
     * 分析层次数据特征
     */
    LayerStats analyzeLayers(const std::vector<std::vector<uint8_t>>& layers);
    
    /**
     * 获取最优的层次压缩策略
     */
    CompressionBackend selectOptimalLayerBackend(const std::vector<std::vector<uint8_t>>& layers);
    
private:
    enum class LayerCompressionType : uint8_t {
        INDIVIDUAL = 0,  // 每层独立压缩
        COMBINED = 1,    // 合并压缩
        HIERARCHICAL = 2 // 层次化压缩
    };
    
    // 压缩策略选择
    LayerCompressionType selectCompressionStrategy(const std::vector<std::vector<uint8_t>>& layers);
    
    // 不同策略的实现
    std::vector<uint8_t> compressIndividualLayers(const std::vector<std::vector<uint8_t>>& layers);
    std::vector<uint8_t> compressCombinedLayers(const std::vector<std::vector<uint8_t>>& layers);
    std::vector<uint8_t> compressHierarchicalLayers(const std::vector<std::vector<uint8_t>>& layers);
    
    std::vector<std::vector<uint8_t>> decompressIndividualLayers(const std::vector<uint8_t>& compressed, size_t& pos);
    std::vector<std::vector<uint8_t>> decompressCombinedLayers(const std::vector<uint8_t>& compressed, size_t& pos);
    std::vector<std::vector<uint8_t>> decompressHierarchicalLayers(const std::vector<uint8_t>& compressed, size_t& pos);
    
    // 内部工具
    std::vector<uint8_t> serializeLayers(const std::vector<std::vector<uint8_t>>& layers);
    std::vector<std::vector<uint8_t>> deserializeLayers(const std::vector<uint8_t>& data, size_t& pos);
};

} // namespace type_aware
} // namespace compression
} // namespace json2