#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace json2 {
namespace compression {

/**
 * 通用压缩接口 - 所有压缩算法都需要实现这个接口
 */
class ICompression {
public:
    virtual ~ICompression() = default;
    
    /**
     * 压缩数据
     * @param data 原始数据
     * @param level 压缩级别 (可选，默认6)
     * @return 压缩后的数据
     */
    virtual std::vector<uint8_t> compress(const std::vector<uint8_t>& data, int level = 6) = 0;
    
    /**
     * 解压数据
     * @param compressed_data 压缩的数据
     * @return 解压后的原始数据
     */
    virtual std::vector<uint8_t> decompress(const std::vector<uint8_t>& compressed_data) = 0;
    
    /**
     * 获取压缩算法名称
     */
    virtual std::string getName() const = 0;
    
    /**
     * 是否支持指定的压缩级别
     */
    virtual bool supportsLevel(int level) const = 0;
    
    /**
     * 是否支持部分解压
     */
    virtual bool supportsPartialDecompression() const { return false; }
};

/**
 * 数值序列压缩接口 - 专门用于数值数据压缩
 */
class INumericCompression : public ICompression {
public:
    virtual ~INumericCompression() = default;
    
    virtual std::vector<uint8_t> compressInt64(const std::vector<int64_t>& values) = 0;
    virtual std::vector<int64_t> decompressInt64(const std::vector<uint8_t>& compressed) = 0;
    
    /**
     * 部分解压指定索引的值
     * @param compressed 压缩的数据
     * @param index 要解压的值的索引
     * @return 指定索引处的值
     */
    virtual int64_t decompressInt64At(const std::vector<uint8_t>& compressed, size_t index) { 
        auto values = decompressInt64(compressed);
        if (index >= values.size()) {
            throw std::out_of_range("Index out of range");
        }
        return values[index]; 
    }
    
    virtual std::vector<uint8_t> compressUint32(const std::vector<uint32_t>& values) = 0;
    virtual std::vector<uint32_t> decompressUint32(const std::vector<uint8_t>& compressed) = 0;
    
    /**
     * 部分解压指定索引的值
     * @param compressed 压缩的数据
     * @param index 要解压的值的索引
     * @return 指定索引处的值
     */
    virtual uint32_t decompressUint32At(const std::vector<uint8_t>& compressed, size_t index) { 
        auto values = decompressUint32(compressed);
        if (index >= values.size()) {
            throw std::out_of_range("Index out of range");
        }
        return values[index]; 
    }
    
    virtual std::vector<uint8_t> compressDouble(const std::vector<double>& values) = 0;
    virtual std::vector<double> decompressDouble(const std::vector<uint8_t>& compressed) = 0;
    
    /**
     * 部分解压指定索引的值
     * @param compressed 压缩的数据
     * @param index 要解压的值的索引
     * @return 指定索引处的值
     */
    virtual double decompressDoubleAt(const std::vector<uint8_t>& compressed, size_t index) { 
        auto values = decompressDouble(compressed);
        if (index >= values.size()) {
            throw std::out_of_range("Index out of range");
        }
        return values[index]; 
    }
};

/**
 * 布尔值压缩接口
 */
class IBooleanCompression : public ICompression {
public:
    virtual ~IBooleanCompression() = default;
    
    virtual std::vector<uint8_t> compressBool(const std::vector<bool>& values) = 0;
    virtual std::vector<bool> decompressBool(const std::vector<uint8_t>& compressed, size_t count) = 0;
    
    /**
     * 部分解压指定索引的值
     * @param compressed 压缩的数据
     * @param index 要解压的值的索引
     * @return 指定索引处的值
     */
    virtual bool decompressBoolAt(const std::vector<uint8_t>& compressed, size_t index) { 
        // 默认实现：解压所有然后返回指定索引的值
        auto values = decompressBool(compressed, 0);
        if (index >= values.size()) {
            throw std::out_of_range("Index out of range");
        }
        return values[index]; 
    }
};

/**
 * 字符串压缩接口
 */
class IStringCompression : public ICompression {
public:
    virtual ~IStringCompression() = default;
    
    virtual std::vector<uint8_t> compressStrings(const std::vector<std::string>& strings) = 0;
    virtual std::vector<std::string> decompressStrings(const std::vector<uint8_t>& compressed) = 0;
    
    /**
     * 部分解压指定索引的值
     * @param compressed 压缩的数据
     * @param index 要解压的值的索引
     * @return 指定索引处的值
     */
    virtual std::string decompressStringAt(const std::vector<uint8_t>& compressed, size_t index) { 
        auto values = decompressStrings(compressed);
        if (index >= values.size()) {
            throw std::out_of_range("Index out of range");
        }
        return values[index]; 
    }
};

} // namespace compression
} // namespace json2