#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "compress.h"
#include "field_key.h"
#include "field_dictionary_manager.h"
#include "louds.h"

namespace json2 {

// 压缩算法类型枚举
enum class CompressionType : uint8_t {
    RLE = 0,           // Run-Length Encoding
    DELTA_VARINT = 1,  // Delta + Varint encoding
    BIT_PACKING = 2,   // Bit packing for booleans
    DICTIONARY = 3,    // Dictionary compression
    BROTLI = 4,        // Brotli compression
    LZ77 = 5,          // LZ77 compression
    DELTA_DELTA = 6,   // Delta-of-delta encoding
    HUFFMAN = 7,       // Huffman encoding
    NULL_AWARE = 8     // Null-aware compression
};

// 类型感知压缩器
class TypeAwareCompressor {
public:
    // 压缩Trie树和相关数据（与Compressor相同的接口）
    static CompressedData compress(const Trie& trie, const FieldDictionaryManager& manager);
    
    // 解压缩并重建Trie树和字典（与Compressor相同的接口）
    static std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
    decompress(const CompressedData& compressed_data);
    
    // 计算压缩率
    static double getCompressionRatio(const CompressedData& compressed_data);
    
    // 保存压缩数据到文件
    static bool saveToFile(const CompressedData& compressed_data, const std::string& filename);
    
    // 从文件加载压缩数据
    static CompressedData loadFromFile(const std::string& filename);
    
    // 压缩LOUDS Trie和相关数据
    static CompressedData compressLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, const std::vector<FieldKey>& field_order);
    
    // 解压缩LOUDS Trie和相关数据
    static std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
    decompressLouds(const CompressedData& compressed_data);
    
    // 内存序列化/反序列化接口
    static std::vector<uint8_t> saveToMemory(const CompressedData& compressed_data);
    static CompressedData loadFromMemory(const std::vector<uint8_t>& buffer);

    // 具体压缩算法实现（public for testing）
    static std::vector<uint8_t> rleCompress(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> rleDecompress(const std::vector<uint8_t>& compressed);
    
    static std::vector<uint8_t> deltaVarintCompress(const std::vector<int64_t>& values);
    static std::vector<int64_t> deltaVarintDecompress(const std::vector<uint8_t>& compressed);
    
    static std::vector<uint8_t> bitPackingCompress(const std::vector<bool>& values);
    static std::vector<bool> bitPackingDecompress(const std::vector<uint8_t>& compressed, size_t count);
    
    static std::vector<uint8_t> dictionaryCompress(const std::vector<std::string>& strings);
    static std::vector<std::string> dictionaryDecompress(const std::vector<uint8_t>& compressed);
    
    static std::vector<uint8_t> deltaDeltaCompress(const std::vector<int64_t>& timestamps);
    static std::vector<int64_t> deltaDeltaDecompress(const std::vector<uint8_t>& compressed);
    
    // Varint编码/解码
    static void encodeVarint(int64_t value, std::vector<uint8_t>& output);
    static int64_t decodeVarint(const std::vector<uint8_t>& data, size_t& pos);

private:
    // LOUDS位图压缩算法
    static std::vector<uint8_t> compressLoudsBitmap(const std::vector<uint8_t>& bitmap);
    static std::vector<uint8_t> decompressLoudsBitmap(const std::vector<uint8_t>& compressed);
    
    // 分层数据压缩算法
    static std::vector<uint8_t> compressLayerData(const std::vector<uint8_t>& data, FieldType type);
    static std::vector<uint8_t> decompressLayerData(const std::vector<uint8_t>& compressed, FieldType type);
    
    // Null值感知压缩算法
    static std::vector<uint8_t> compressLayerDataWithNulls(const std::vector<uint8_t>& data, FieldType type, const std::vector<bool>& null_mask);
    static std::vector<uint8_t> decompressLayerDataWithNulls(const std::vector<uint8_t>& compressed, FieldType type, size_t total_count);
    static std::vector<uint8_t> compressWithNullHandling(const std::vector<uint8_t>& data, FieldType type, const std::vector<bool>& null_mask);
    static std::vector<uint8_t> decompressWithNullHandling(const std::vector<uint8_t>& compressed, FieldType type, size_t total_count);
    
    // 字典数据压缩算法
    static std::vector<uint8_t> compressDictionaryData(const FieldDictionaryManager& manager);
    static std::unique_ptr<FieldDictionaryManager> decompressDictionaryData(const std::vector<uint8_t>& compressed, const std::vector<FieldKey>& field_keys = {});
    
    // 辅助函数
    static std::vector<int64_t> bytesToInt64s(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> int64sToBytes(const std::vector<int64_t>& values);
    static std::vector<bool> bytesToBools(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> boolsToBytes(const std::vector<bool>& values);
    static std::vector<uint32_t> bytesToUint32s(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> uint32sToBytes(const std::vector<uint32_t>& values);
    
    // 布尔状态压缩（支持null值）
    static std::vector<uint8_t> compressBoolStates(const std::vector<uint8_t>& states);
    static std::vector<uint8_t> decompressBoolStates(const std::vector<uint8_t>& compressed, size_t count);
};

} // namespace json2
