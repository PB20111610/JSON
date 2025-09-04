#pragma once

#include <vector>
#include <cstdint>
#include <string>

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
};

/**
 * 数值序列压缩接口 - 专门用于数值数据压缩
 */
class INumericCompression : public ICompression {
public:
    virtual ~INumericCompression() = default;
    
    virtual std::vector<uint8_t> compressInt64(const std::vector<int64_t>& values) = 0;
    virtual std::vector<int64_t> decompressInt64(const std::vector<uint8_t>& compressed) = 0;
    
    virtual std::vector<uint8_t> compressUint32(const std::vector<uint32_t>& values) = 0;
    virtual std::vector<uint32_t> decompressUint32(const std::vector<uint8_t>& compressed) = 0;
    
    virtual std::vector<uint8_t> compressDouble(const std::vector<double>& values) = 0;
    virtual std::vector<double> decompressDouble(const std::vector<uint8_t>& compressed) = 0;
};

/**
 * 布尔值压缩接口
 */
class IBooleanCompression : public ICompression {
public:
    virtual ~IBooleanCompression() = default;
    
    virtual std::vector<uint8_t> compressBool(const std::vector<bool>& values) = 0;
    virtual std::vector<bool> decompressBool(const std::vector<uint8_t>& compressed, size_t count) = 0;
};

/**
 * 字符串压缩接口
 */
class IStringCompression : public ICompression {
public:
    virtual ~IStringCompression() = default;
    
    virtual std::vector<uint8_t> compressStrings(const std::vector<std::string>& strings) = 0;
    virtual std::vector<std::string> decompressStrings(const std::vector<uint8_t>& compressed) = 0;
};

} // namespace compression
} // namespace json2