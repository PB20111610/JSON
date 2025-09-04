#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <cstring>

namespace json2 {
namespace compression {
namespace utils {

/**
 * 序列化工具 - 用于将各种数据类型转换为字节序列
 */
class SerializationUtils {
public:
    // 基本类型序列化
    template<typename T>
    static void writeValue(std::vector<uint8_t>& data, const T& value) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
    }

    template<typename T>
    static T readValue(const std::vector<uint8_t>& data, size_t& pos) {
        T value;
        std::memcpy(&value, &data[pos], sizeof(T));
        pos += sizeof(T);
        return value;
    }

    // 字符串序列化
    static void writeString(std::vector<uint8_t>& data, const std::string& str);
    static std::string readString(const std::vector<uint8_t>& data, size_t& pos);

    // 字符串向量序列化
    static void writeStringVector(std::vector<uint8_t>& data, const std::vector<std::string>& vec);
    static std::vector<std::string> readStringVector(const std::vector<uint8_t>& data, size_t& pos);

    // 数据类型转换工具
    static std::vector<int64_t> bytesToInt64s(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> int64sToBytes(const std::vector<int64_t>& values);
    
    static std::vector<uint32_t> bytesToUint32s(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> uint32sToBytes(const std::vector<uint32_t>& values);
    
    static std::vector<bool> bytesToBools(const std::vector<uint8_t>& data, size_t count);
    static std::vector<uint8_t> boolsToBytes(const std::vector<bool>& values);
    
    static std::vector<double> bytesToDoubles(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> doublesToBytes(const std::vector<double>& values);
};

/**
 * Varint编码工具 - 可变长度整数编码
 */
class VarintUtils {
public:
    // ZigZag编码：将有符号整数映射为无符号整数
    static uint64_t zigzagEncode(int64_t value);
    static int64_t zigzagDecode(uint64_t value);
    
    // Varint编码/解码
    static void encodeVarint(int64_t value, std::vector<uint8_t>& output);
    static int64_t decodeVarint(const std::vector<uint8_t>& data, size_t& pos);
    
    // 计算varint编码后的字节数
    static size_t getVarintSize(int64_t value);
};

/**
 * 数据分析工具 - 分析数据特征以选择最优压缩算法
 */
class DataAnalysisUtils {
public:
    // 重复性分析
    static double calculateRepetitionRatio(const std::vector<uint8_t>& data);
    static bool isHighlyRepetitive(const std::vector<uint8_t>& data, double threshold = 0.5);
    
    // 数值序列分析
    static bool isSorted(const std::vector<int64_t>& values);
    static bool hasSmallDeltas(const std::vector<int64_t>& values, int64_t threshold = 1000);
    static double calculateDeltaVariance(const std::vector<int64_t>& values);
    
    // 时间序列分析
    static bool hasRegularIntervals(const std::vector<int64_t>& timestamps, double tolerance = 0.1);
    static bool isTimestampLike(const std::vector<int64_t>& values);
    
    // 布尔值分析
    static bool isSparse(const std::vector<bool>& values, double threshold = 0.1);
    static double calculateTrueFalseRatio(const std::vector<bool>& values);
    
    // 字符串分析
    static double calculateUniqueRatio(const std::vector<std::string>& strings);
    static bool isHighlyRedundant(const std::vector<std::string>& strings, double threshold = 0.5);
};

} // namespace utils
} // namespace compression
} // namespace json2