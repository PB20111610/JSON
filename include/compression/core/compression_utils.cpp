#include "compression_utils.h"
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace json2 {
namespace compression {
namespace utils {

// SerializationUtils 实现
void SerializationUtils::writeString(std::vector<uint8_t>& data, const std::string& str) {
    uint32_t length = static_cast<uint32_t>(str.length());
    writeValue(data, length);
    data.insert(data.end(), str.begin(), str.end());
}

std::string SerializationUtils::readString(const std::vector<uint8_t>& data, size_t& pos) {
    // First check if we can read the length
    if (pos + sizeof(uint32_t) > data.size()) {
        throw std::runtime_error("SerializationUtils::readString: Insufficient data for string length");
    }
    
    uint32_t length = readValue<uint32_t>(data, pos);
    
    // Check if we have enough data for the string content
    if (pos + length > data.size()) {
        throw std::runtime_error("SerializationUtils::readString: Insufficient data for string content, requested: " + 
                                std::to_string(length) + ", available: " + std::to_string(data.size() - pos));
    }
    
    std::string str(reinterpret_cast<const char*>(&data[pos]), length);
    pos += length;
    return str;
}

void SerializationUtils::writeStringVector(std::vector<uint8_t>& data, const std::vector<std::string>& vec) {
    uint32_t size = static_cast<uint32_t>(vec.size());
    writeValue(data, size);
    for (const auto& item : vec) {
        writeString(data, item);
    }
}

std::vector<std::string> SerializationUtils::readStringVector(const std::vector<uint8_t>& data, size_t& pos) {
    uint32_t size = readValue<uint32_t>(data, pos);
    std::vector<std::string> vec;
    vec.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        vec.push_back(readString(data, pos));
    }
    return vec;
}

std::vector<int64_t> SerializationUtils::bytesToInt64s(const std::vector<uint8_t>& data) {
    std::vector<int64_t> values;
    for (size_t i = 0; i + sizeof(int64_t) <= data.size(); i += sizeof(int64_t)) {
        int64_t value;
        std::memcpy(&value, &data[i], sizeof(int64_t));
        values.push_back(value);
    }
    return values;
}

std::vector<uint8_t> SerializationUtils::int64sToBytes(const std::vector<int64_t>& values) {
    std::vector<uint8_t> data;
    data.reserve(values.size() * sizeof(int64_t));
    for (int64_t value : values) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(int64_t));
    }
    return data;
}

std::vector<uint32_t> SerializationUtils::bytesToUint32s(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> values;
    for (size_t i = 0; i + sizeof(uint32_t) <= data.size(); i += sizeof(uint32_t)) {
        uint32_t value;
        std::memcpy(&value, &data[i], sizeof(uint32_t));
        values.push_back(value);
    }
    return values;
}

std::vector<uint8_t> SerializationUtils::uint32sToBytes(const std::vector<uint32_t>& values) {
    std::vector<uint8_t> data;
    data.reserve(values.size() * sizeof(uint32_t));
    for (uint32_t value : values) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(uint32_t));
    }
    return data;
}

std::vector<bool> SerializationUtils::bytesToBools(const std::vector<uint8_t>& data, size_t count) {
    std::vector<bool> values;
    values.reserve(count);
    
    for (size_t i = 0; i < data.size() && values.size() < count; ++i) {
        uint8_t byte = data[i];
        for (int bit = 0; bit < 8 && values.size() < count; ++bit) {
            values.push_back((byte & (1 << bit)) != 0);
        }
    }
    
    return values;
}

std::vector<uint8_t> SerializationUtils::boolsToBytes(const std::vector<bool>& values) {
    std::vector<uint8_t> data;
    uint8_t current_byte = 0;
    int bit_pos = 0;
    
    for (bool value : values) {
        if (value) {
            current_byte |= (1 << bit_pos);
        }
        bit_pos++;
        
        if (bit_pos == 8) {
            data.push_back(current_byte);
            current_byte = 0;
            bit_pos = 0;
        }
    }
    
    if (bit_pos > 0) {
        data.push_back(current_byte);
    }
    
    return data;
}

std::vector<double> SerializationUtils::bytesToDoubles(const std::vector<uint8_t>& data) {
    std::vector<double> values;
    for (size_t i = 0; i + sizeof(double) <= data.size(); i += sizeof(double)) {
        double value;
        std::memcpy(&value, &data[i], sizeof(double));
        values.push_back(value);
    }
    return values;
}

std::vector<uint8_t> SerializationUtils::doublesToBytes(const std::vector<double>& values) {
    std::vector<uint8_t> data;
    data.reserve(values.size() * sizeof(double));
    for (double value : values) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(double));
    }
    return data;
}

// VarintUtils 实现
uint64_t VarintUtils::zigzagEncode(int64_t value) {
    return (value < 0) ? (static_cast<uint64_t>(-value) << 1) | 1 : static_cast<uint64_t>(value) << 1;
}

int64_t VarintUtils::zigzagDecode(uint64_t value) {
    return (value & 1) ? -static_cast<int64_t>(value >> 1) : static_cast<int64_t>(value >> 1);
}

void VarintUtils::encodeVarint(int64_t value, std::vector<uint8_t>& output) {
    uint64_t uvalue = zigzagEncode(value);
    
    while (uvalue >= 0x80) {
        output.push_back(static_cast<uint8_t>(uvalue & 0x7F) | 0x80);
        uvalue >>= 7;
    }
    output.push_back(static_cast<uint8_t>(uvalue & 0x7F));
}

int64_t VarintUtils::decodeVarint(const std::vector<uint8_t>& data, size_t& pos) {
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

size_t VarintUtils::getVarintSize(int64_t value) {
    uint64_t uvalue = zigzagEncode(value);
    size_t size = 1;
    
    while (uvalue >= 0x80) {
        uvalue >>= 7;
        size++;
    }
    
    return size;
}

// DataAnalysisUtils 实现
double DataAnalysisUtils::calculateRepetitionRatio(const std::vector<uint8_t>& data) {
    if (data.empty()) return 0.0;
    
    std::unordered_map<uint8_t, size_t> freq;
    for (uint8_t byte : data) {
        freq[byte]++;
    }
    
    size_t max_freq = 0;
    for (const auto& pair : freq) {
        max_freq = std::max(max_freq, pair.second);
    }
    
    return static_cast<double>(max_freq) / data.size();
}

bool DataAnalysisUtils::isHighlyRepetitive(const std::vector<uint8_t>& data, double threshold) {
    return calculateRepetitionRatio(data) >= threshold;
}

bool DataAnalysisUtils::isSorted(const std::vector<int64_t>& values) {
    if (values.size() <= 1) return true;
    
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] < values[i-1]) {
            return false;
        }
    }
    return true;
}

bool DataAnalysisUtils::hasSmallDeltas(const std::vector<int64_t>& values, int64_t threshold) {
    if (values.size() <= 1) return true;
    
    for (size_t i = 1; i < values.size(); ++i) {
        int64_t delta = std::abs(values[i] - values[i-1]);
        if (delta > threshold) {
            return false;
        }
    }
    return true;
}

double DataAnalysisUtils::calculateDeltaVariance(const std::vector<int64_t>& values) {
    if (values.size() <= 1) return 0.0;
    
    std::vector<int64_t> deltas;
    for (size_t i = 1; i < values.size(); ++i) {
        deltas.push_back(values[i] - values[i-1]);
    }
    
    // 计算平均值
    int64_t sum = 0;
    for (int64_t delta : deltas) {
        sum += delta;
    }
    double mean = static_cast<double>(sum) / deltas.size();
    
    // 计算方差
    double variance_sum = 0.0;
    for (int64_t delta : deltas) {
        double diff = static_cast<double>(delta) - mean;
        variance_sum += diff * diff;
    }
    
    return variance_sum / deltas.size();
}

bool DataAnalysisUtils::hasRegularIntervals(const std::vector<int64_t>& timestamps, double tolerance) {
    if (timestamps.size() < 3) return false;
    
    std::vector<int64_t> intervals;
    for (size_t i = 1; i < timestamps.size(); ++i) {
        intervals.push_back(timestamps[i] - timestamps[i-1]);
    }
    
    int64_t sum = 0;
    for (int64_t interval : intervals) {
        sum += interval;
    }
    double avg_interval = static_cast<double>(sum) / intervals.size();
    
    size_t regular_count = 0;
    for (int64_t interval : intervals) {
        double deviation = std::abs(static_cast<double>(interval) - avg_interval) / avg_interval;
        if (deviation <= tolerance) {
            regular_count++;
        }
    }
    
    return static_cast<double>(regular_count) / intervals.size() >= 0.8;
}

bool DataAnalysisUtils::isTimestampLike(const std::vector<int64_t>& values) {
    return isSorted(values) && (hasRegularIntervals(values) || hasSmallDeltas(values, 1000000));
}

bool DataAnalysisUtils::isSparse(const std::vector<bool>& values, double threshold) {
    if (values.empty()) return false;
    
    size_t true_count = 0;
    for (bool val : values) {
        if (val) true_count++;
    }
    
    double true_ratio = static_cast<double>(true_count) / values.size();
    return true_ratio <= threshold || true_ratio >= (1.0 - threshold);
}

double DataAnalysisUtils::calculateTrueFalseRatio(const std::vector<bool>& values) {
    if (values.empty()) return 0.0;
    
    size_t true_count = 0;
    for (bool val : values) {
        if (val) true_count++;
    }
    
    return static_cast<double>(true_count) / values.size();
}

double DataAnalysisUtils::calculateUniqueRatio(const std::vector<std::string>& strings) {
    if (strings.empty()) return 0.0;
    
    std::unordered_map<std::string, size_t> freq;
    for (const auto& str : strings) {
        freq[str]++;
    }
    
    return static_cast<double>(freq.size()) / strings.size();
}

bool DataAnalysisUtils::isHighlyRedundant(const std::vector<std::string>& strings, double threshold) {
    return calculateUniqueRatio(strings) <= threshold;
}

} // namespace utils
} // namespace compression
} // namespace json2