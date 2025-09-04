#include "varint_compression.h"
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace json2 {
namespace compression {
namespace algorithms {

// ========== Helper Functions (模仿compress.cpp的风格) ==========
static void writeVarintHeader(std::vector<uint8_t>& output, size_t count, uint8_t type) {
    // 写入元素数量
    uint32_t count32 = static_cast<uint32_t>(count);
    output.insert(output.end(), reinterpret_cast<uint8_t*>(&count32), 
                  reinterpret_cast<uint8_t*>(&count32) + sizeof(count32));
    // 写入数据类型标志
    output.push_back(type);
}

static std::pair<size_t, uint8_t> readVarintHeader(const std::vector<uint8_t>& data, size_t& pos) {
    if (pos + sizeof(uint32_t) + 1 > data.size()) {
        throw std::runtime_error("Varint: Insufficient data for header");
    }
    
    uint32_t count;
    std::memcpy(&count, &data[pos], sizeof(count));
    pos += sizeof(count);
    
    uint8_t type = data[pos++];
    
    return {count, type};
}

// 数据类型标志
static constexpr uint8_t TYPE_INT64 = 1;
static constexpr uint8_t TYPE_UINT32 = 2;
static constexpr uint8_t TYPE_DOUBLE = 3;

// ========== ICompression 接口实现 ==========
std::vector<uint8_t> VarintCompression::compress(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return data;
    
    // 简单实现：将字节数据转为整数序列进行压缩
    std::vector<int64_t> int_values;
    int_values.reserve(data.size());
    
    for (uint8_t byte : data) {
        int_values.push_back(static_cast<int64_t>(byte));
    }
    
    return compressInt64Array(int_values);
}

std::vector<uint8_t> VarintCompression::decompress(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return compressed_data;
    
    auto int_values = decompressInt64Array(compressed_data);
    std::vector<uint8_t> result;
    result.reserve(int_values.size());
    
    for (int64_t val : int_values) {
        result.push_back(static_cast<uint8_t>(val));
    }
    
    return result;
}

// ICompression interface methods
std::string VarintCompression::getName() const {
    return "Varint";
}

bool VarintCompression::supportsLevel(int level) const {
    // Varint compression doesn't use compression levels, but we accept any level
    return (level >= 0 && level <= 22);
}

// INumericCompression interface implementation
std::vector<uint8_t> VarintCompression::compressInt64(const std::vector<int64_t>& values) {
    return compressInt64Array(values);
}

std::vector<int64_t> VarintCompression::decompressInt64(const std::vector<uint8_t>& compressed) {
    return decompressInt64Array(compressed);
}

std::vector<uint8_t> VarintCompression::compressUint32(const std::vector<uint32_t>& values) {
    return compressUint32Array(values);
}

std::vector<uint32_t> VarintCompression::decompressUint32(const std::vector<uint8_t>& compressed) {
    return decompressUint32Array(compressed);
}

std::vector<uint8_t> VarintCompression::compressDouble(const std::vector<double>& values) {
    // 对于double，转换为int64位表示再压缩（简化实现）
    std::vector<int64_t> int64_values;
    int64_values.reserve(values.size());
    
    for (double val : values) {
        int64_t* int_ptr = reinterpret_cast<int64_t*>(&val);
        int64_values.push_back(*int_ptr);
    }
    
    return compressInt64Array(int64_values);
}

std::vector<double> VarintCompression::decompressDouble(const std::vector<uint8_t>& compressed) {
    auto int64_values = decompressInt64Array(compressed);
    std::vector<double> values;
    values.reserve(int64_values.size());
    
    for (int64_t val : int64_values) {
        double* double_ptr = reinterpret_cast<double*>(&val);
        values.push_back(*double_ptr);
    }
    
    return values;
}

// 静态方法实现

std::vector<uint8_t> VarintCompression::compressInt64Array(const std::vector<int64_t>& values) {
    std::vector<uint8_t> compressed;
    compressed.reserve(values.size() * 4); // 预估大小
    
    for (int64_t value : values) {
        encodeVarint(value, compressed);
    }
    
    return compressed;
}

std::vector<int64_t> VarintCompression::decompressInt64Array(const std::vector<uint8_t>& compressed) {
    std::vector<int64_t> values;
    size_t pos = 0;
    
    while (pos < compressed.size()) {
        values.push_back(decodeVarint(compressed, pos));
    }
    
    return values;
}

std::vector<uint8_t> VarintCompression::compressUint32Array(const std::vector<uint32_t>& values) {
    std::vector<int64_t> int64_values;
    int64_values.reserve(values.size());
    for (uint32_t val : values) {
        int64_values.push_back(static_cast<int64_t>(val));
    }
    return compressInt64Array(int64_values);
}

std::vector<uint32_t> VarintCompression::decompressUint32Array(const std::vector<uint8_t>& compressed) {
    auto int64_values = decompressInt64Array(compressed);
    std::vector<uint32_t> values;
    values.reserve(int64_values.size());
    for (int64_t val : int64_values) {
        values.push_back(static_cast<uint32_t>(val));
    }
    return values;
}

void VarintCompression::encodeVarint(int64_t value, std::vector<uint8_t>& output) {
    uint64_t uvalue = zigzagEncode(value);
    
    while (uvalue >= 0x80) {
        output.push_back(static_cast<uint8_t>(uvalue & 0x7F) | 0x80);
        uvalue >>= 7;
    }
    output.push_back(static_cast<uint8_t>(uvalue & 0x7F));
}

int64_t VarintCompression::decodeVarint(const std::vector<uint8_t>& data, size_t& pos) {
    uint64_t result = 0;
    int shift = 0;
    
    while (pos < data.size()) {
        uint8_t byte = data[pos++];
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
        if (shift >= 64) {
            throw std::runtime_error("Varint decoding overflow");
        }
    }
    
    return zigzagDecode(result);
}

uint64_t VarintCompression::zigzagEncode(int64_t value) {
    return (value < 0) ? (static_cast<uint64_t>(-value) << 1) | 1 : static_cast<uint64_t>(value) << 1;
}

int64_t VarintCompression::zigzagDecode(uint64_t value) {
    return (value & 1) ? -static_cast<int64_t>(value >> 1) : static_cast<int64_t>(value >> 1);
}

size_t VarintCompression::estimateCompressedSize(const std::vector<int64_t>& values) {
    size_t total_size = 0;
    for (int64_t value : values) {
        total_size += getVarintSize(value);
    }
    return total_size;
}

double VarintCompression::estimateCompressionRatio(const std::vector<int64_t>& values) {
    if (values.empty()) return 1.0;
    
    size_t compressed_size = estimateCompressedSize(values);
    size_t original_size = values.size() * sizeof(int64_t);
    
    return static_cast<double>(compressed_size) / original_size;
}

size_t VarintCompression::getVarintSize(int64_t value) {
    uint64_t uvalue = zigzagEncode(value);
    size_t size = 1;
    
    while (uvalue >= 0x80) {
        uvalue >>= 7;
        size++;
    }
    
    return size;
}

bool VarintCompression::isVarintEfficient(const std::vector<int64_t>& values, double threshold) {
    return estimateCompressionRatio(values) <= threshold;
}

} // namespace algorithms
} // namespace compression
} // namespace json2