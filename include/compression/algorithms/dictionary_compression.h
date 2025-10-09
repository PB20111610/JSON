#pragma once

#include "../core/compression_interface.h"
#include "../core/compression_backend.h"
#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace json2 {
namespace compression {
namespace algorithms {

/**
 * Dictionary 压缩算法
 * 适用于: 字符串数据、重复性高的数据
 */
class DictionaryCompression : public IStringCompression {
public:
    DictionaryCompression() = default;
    virtual ~DictionaryCompression() = default;
    
    // ICompression 接口实现 (from base class)
    std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) override;
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) override;
    std::string getName() const override;
    bool supportsLevel(int level) const override;
    
    // 支持部分解压
    bool supportsPartialDecompression() const override { return true; }
    
    // IStringCompression 接口实现
    std::vector<uint8_t> compressStrings(const std::vector<std::string>& strings) override;
    std::vector<std::string> decompressStrings(const std::vector<uint8_t>& compressed) override;
    
    // 部分解压指定索引的值
    std::string decompressStringAt(const std::vector<uint8_t>& compressed, size_t index) override;
    
    // 压缩类型常量
    enum CompressionType : uint8_t {
        COMPRESSION_NONE = 0,
        COMPRESSION_DICTIONARY = 1,
        COMPRESSION_BACKEND = 2
    };
    
    // Dictionary专用接口 - 支持指定后端压缩
    enum class Backend {
        NONE = 0,    // 只使用字典，不进一步压缩
        RLE = 1,     // 字典后使用RLE压缩索引
        VARINT = 2,  // 字典后使用Varint压缩索引
        ZSTD = 3,    // 字典后使用ZSTD压缩
        BROTLI = 4   // 字典后使用Brotli压缩
    };
    
    static std::vector<uint8_t> dictionaryCompress(const std::vector<std::string>& strings, 
                                                  Backend backend = Backend::NONE);
    static std::vector<std::string> dictionaryDecompress(const std::vector<uint8_t>& compressed, 
                                                        Backend backend = Backend::NONE);
    
    // 部分解压指定索引的值
    static std::string dictionaryDecompressAt(const std::vector<uint8_t>& compressed, size_t index,
                                            Backend backend = Backend::NONE);
    
    // 增量字典构建 (适用于大数据集)
    class IncrementalDictionary {
    public:
        void addStrings(const std::vector<std::string>& strings);
        std::vector<uint8_t> compress(const std::vector<std::string>& strings, Backend backend = Backend::NONE);
        void reset();
        
        size_t getDictionarySize() const { return dictionary_.size(); }
        double getCompressionRatio() const;
        
    private:
        std::unordered_map<std::string, uint32_t> dict_map_;
        std::vector<std::string> dictionary_;
        size_t total_input_size_ = 0;
        size_t total_output_size_ = 0;
    };
    
    // 数据分析
    static double calculateUniqueRatio(const std::vector<std::string>& strings);
    static bool isHighlyRedundant(const std::vector<std::string>& strings, double threshold = 0.5);
    static double estimateCompressionRatio(const std::vector<std::string>& strings);
    
    // 字典构建选项
    struct DictionaryStats {
        size_t unique_count;
        size_t total_count;
        double unique_ratio;
        size_t dict_size_bytes;
        size_t indices_size_bytes;
        double estimated_ratio;
        Backend recommended_backend;
        bool is_worth_compressing;
        // Additional fields used by implementation
        size_t total_size;
        double avg_string_length;
        size_t min_length;
        size_t max_length;
    };
    
    static DictionaryStats analyzeDictionary(const std::vector<std::string>& strings);
    static Backend selectOptimalBackend(const std::vector<std::string>& strings);
    
    // Additional methods used by implementation
    static DictionaryStats analyzeStrings(const std::vector<std::string>& strings);
    static std::vector<uint8_t> compressWithDictionary(const std::vector<std::string>& strings);
    static std::vector<std::string> decompressFromDictionary(const std::vector<uint8_t>& compressed, size_t& pos);
    static std::vector<uint8_t> compressWithBackend(const std::vector<std::string>& strings, CompressionBackend backend);
    static std::vector<std::string> decompressFromBackend(const std::vector<uint8_t>& compressed, size_t& pos);
    static std::vector<uint8_t> serializeStrings(const std::vector<std::string>& strings);
    static std::vector<std::string> deserializeStrings(const std::vector<uint8_t>& data, size_t& pos);
    
private:
    // 内部辅助函数
    static void writeUint32(std::vector<uint8_t>& data, uint32_t value);
    static uint32_t readUint32(const std::vector<uint8_t>& data, size_t& pos);
    static void writeString(std::vector<uint8_t>& data, const std::string& str);
    static std::string readString(const std::vector<uint8_t>& data, size_t& pos);
    
    // 后端压缩处理
    static std::vector<uint8_t> compressIndices(const std::vector<uint32_t>& indices, Backend backend);
    static std::vector<uint32_t> decompressIndices(const std::vector<uint8_t>& compressed, 
                                                   size_t expected_count, Backend backend);
    
    // 部分解压辅助函数
    static std::string partialDecompressHelper(const std::vector<uint8_t>& compressed, size_t index, 
                                              size_t& pos, const std::vector<std::string>& dictionary,
                                              Backend backend);
};

} // namespace algorithms
} // namespace compression
} // namespace json2