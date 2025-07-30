#include "../include/compress_type_aware.h"
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <fstream>
#include <iostream>

namespace json2 {

// ========== 序列化辅助函数 ==========
template<typename T>
static void writeValue(std::vector<uint8_t>& data, const T& value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(T));
}

static void writeString(std::vector<uint8_t>& data, const std::string& str) {
    uint32_t length = static_cast<uint32_t>(str.length());
    writeValue(data, length);
    data.insert(data.end(), str.begin(), str.end());
}

static void writeVector(std::vector<uint8_t>& data, const std::vector<std::string>& vec) {
    uint32_t size = static_cast<uint32_t>(vec.size());
    writeValue(data, size);
    for (const auto& item : vec) {
        writeString(data, item);
    }
}

// ========== RLE压缩算法 ==========
std::vector<uint8_t> TypeAwareCompressor::rleCompress(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    if (data.empty()) return out;
    
    uint8_t curr = data[0];
    uint8_t count = 1;
    
    for (size_t i = 1; i < data.size(); ++i) {
        if (data[i] == curr && count < 255) {
            ++count;
        } else {
            out.push_back(curr);
            out.push_back(count);
            curr = data[i];
            count = 1;
        }
    }
    out.push_back(curr);
    out.push_back(count);
    
    return out;
}

std::vector<uint8_t> TypeAwareCompressor::rleDecompress(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        uint8_t value = data[i];
        uint8_t count = data[i + 1];
        out.insert(out.end(), count, value);
    }
    return out;
}

// ========== Varint编码/解码 ==========
void TypeAwareCompressor::encodeVarint(int64_t value, std::vector<uint8_t>& output) {
    // 处理负数
    uint64_t uvalue = (value < 0) ? (static_cast<uint64_t>(-value) << 1) | 1 : static_cast<uint64_t>(value) << 1;
    
    while (uvalue >= 0x80) {
        output.push_back(static_cast<uint8_t>(uvalue & 0x7F) | 0x80);
        uvalue >>= 7;
    }
    output.push_back(static_cast<uint8_t>(uvalue & 0x7F));
}

int64_t TypeAwareCompressor::decodeVarint(const std::vector<uint8_t>& data, size_t& pos) {
    uint64_t result = 0;
    int shift = 0;
    
    while (pos < data.size()) {
        uint8_t byte = data[pos++];
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    
    // 处理负数
    if (result & 1) {
        return -static_cast<int64_t>(result >> 1);
    } else {
        return static_cast<int64_t>(result >> 1);
    }
}

// ========== Delta + Varint压缩算法 ==========
std::vector<uint8_t> TypeAwareCompressor::deltaVarintCompress(const std::vector<int64_t>& values) {
    std::vector<uint8_t> compressed;
    if (values.empty()) return compressed;
    
    // 写入第一个值
    encodeVarint(values[0], compressed);
    
    // 对后续值进行delta编码
    int64_t prev = values[0];
    for (size_t i = 1; i < values.size(); ++i) {
        int64_t delta = values[i] - prev;
        encodeVarint(delta, compressed);
        prev = values[i];
    }
    
    return compressed;
}

std::vector<int64_t> TypeAwareCompressor::deltaVarintDecompress(const std::vector<uint8_t>& compressed) {
    std::vector<int64_t> values;
    if (compressed.empty()) return values;
    
    size_t pos = 0;
    
    // 读取第一个值
    int64_t first = decodeVarint(compressed, pos);
    values.push_back(first);
    
    // 解码后续的delta值
    int64_t prev = first;
    while (pos < compressed.size()) {
        int64_t delta = decodeVarint(compressed, pos);
        int64_t value = prev + delta;
        values.push_back(value);
        prev = value;
    }
    
    return values;
}

// ========== 位打包压缩算法 ==========
std::vector<uint8_t> TypeAwareCompressor::bitPackingCompress(const std::vector<bool>& values) {
    std::vector<uint8_t> compressed;
    uint8_t current_byte = 0;
    int bit_pos = 0;
    
    for (bool value : values) {
        if (value) {
            current_byte |= (1 << bit_pos);
        }
        bit_pos++;
        
        if (bit_pos == 8) {
            compressed.push_back(current_byte);
            current_byte = 0;
            bit_pos = 0;
        }
    }
    
    // 处理最后一个不完整的字节
    if (bit_pos > 0) {
        compressed.push_back(current_byte);
    }
    
    return compressed;
}

std::vector<bool> TypeAwareCompressor::bitPackingDecompress(const std::vector<uint8_t>& compressed, size_t count) {
    std::vector<bool> values;
    values.reserve(count);
    
    for (size_t i = 0; i < compressed.size() && values.size() < count; ++i) {
        uint8_t byte = compressed[i];
        for (int bit = 0; bit < 8 && values.size() < count; ++bit) {
            values.push_back((byte & (1 << bit)) != 0);
        }
    }
    
    return values;
}

// ========== 字典压缩算法 ==========
std::vector<uint8_t> TypeAwareCompressor::dictionaryCompress(const std::vector<std::string>& strings) {
    std::vector<uint8_t> compressed;
    
    // 构建字典
    std::unordered_map<std::string, uint32_t> dict;
    std::vector<std::string> unique_strings;
    
    for (const auto& str : strings) {
        if (dict.find(str) == dict.end()) {
            dict[str] = static_cast<uint32_t>(unique_strings.size());
            unique_strings.push_back(str);
        }
    }
    
    // 写入字典大小
    uint32_t dict_size = static_cast<uint32_t>(unique_strings.size());
    compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&dict_size), 
                     reinterpret_cast<uint8_t*>(&dict_size) + sizeof(dict_size));
    
    // 写入字典内容
    for (const auto& str : unique_strings) {
        uint32_t str_len = static_cast<uint32_t>(str.length());
        compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&str_len), 
                         reinterpret_cast<uint8_t*>(&str_len) + sizeof(str_len));
        compressed.insert(compressed.end(), str.begin(), str.end());
    }
    
    // 写入编码序列
    for (const auto& str : strings) {
        uint32_t code = dict[str];
        compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&code), 
                         reinterpret_cast<uint8_t*>(&code) + sizeof(code));
    }
    
    return compressed;
}

std::vector<std::string> TypeAwareCompressor::dictionaryDecompress(const std::vector<uint8_t>& compressed) {
    std::vector<std::string> strings;
    size_t pos = 0;
    
    // 读取字典大小
    uint32_t dict_size;
    std::memcpy(&dict_size, &compressed[pos], sizeof(dict_size));
    pos += sizeof(dict_size);
    
    // 读取字典内容
    std::vector<std::string> dict;
    dict.reserve(dict_size);
    for (uint32_t i = 0; i < dict_size; ++i) {
        uint32_t str_len;
        std::memcpy(&str_len, &compressed[pos], sizeof(str_len));
        pos += sizeof(str_len);
        
        std::string str(reinterpret_cast<const char*>(&compressed[pos]), str_len);
        pos += str_len;
        dict.push_back(str);
    }
    
    // 读取编码序列
    while (pos < compressed.size()) {
        uint32_t code;
        std::memcpy(&code, &compressed[pos], sizeof(code));
        pos += sizeof(code);
        
        if (code < dict.size()) {
            strings.push_back(dict[code]);
        }
    }
    
    return strings;
}

// ========== Delta-of-Delta压缩算法 ==========
std::vector<uint8_t> TypeAwareCompressor::deltaDeltaCompress(const std::vector<int64_t>& timestamps) {
    std::vector<uint8_t> compressed;
    if (timestamps.size() < 2) {
        // 对于少于2个值的情况，直接使用delta编码
        return deltaVarintCompress(timestamps);
    }
    
    // 写入前两个值
    encodeVarint(timestamps[0], compressed);
    encodeVarint(timestamps[1], compressed);
    
    // 计算delta-of-delta
    int64_t prev1 = timestamps[0];
    int64_t prev2 = timestamps[1];
    
    for (size_t i = 2; i < timestamps.size(); ++i) {
        int64_t delta1 = timestamps[i] - prev1;
        int64_t delta2 = delta1 - (prev1 - prev2);
        encodeVarint(delta2, compressed);
        prev2 = prev1;
        prev1 = timestamps[i];
    }
    
    return compressed;
}

std::vector<int64_t> TypeAwareCompressor::deltaDeltaDecompress(const std::vector<uint8_t>& compressed) {
    std::vector<int64_t> timestamps;
    if (compressed.empty()) return timestamps;
    
    size_t pos = 0;
    
    // 读取前两个值
    int64_t first = decodeVarint(compressed, pos);
    int64_t second = decodeVarint(compressed, pos);
    timestamps.push_back(first);
    timestamps.push_back(second);
    
    // 解码delta-of-delta
    int64_t prev1 = first;
    int64_t prev2 = second;
    
    while (pos < compressed.size()) {
        int64_t delta2 = decodeVarint(compressed, pos);
        int64_t delta1 = delta2 + (prev1 - prev2);
        int64_t value = prev1 + delta1;
        timestamps.push_back(value);
        prev2 = prev1;
        prev1 = value;
    }
    
    return timestamps;
}

// ========== 辅助函数 ==========
std::vector<int64_t> TypeAwareCompressor::bytesToInt64s(const std::vector<uint8_t>& data) {
    std::vector<int64_t> values;
    size_t count = data.size() / sizeof(int64_t);
    values.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        int64_t value;
        std::memcpy(&value, &data[i * sizeof(int64_t)], sizeof(int64_t));
        values.push_back(value);
    }
    
    return values;
}

std::vector<uint8_t> TypeAwareCompressor::int64sToBytes(const std::vector<int64_t>& values) {
    std::vector<uint8_t> data;
    data.reserve(values.size() * sizeof(int64_t));
    
    for (int64_t value : values) {
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&value), 
                   reinterpret_cast<uint8_t*>(&value) + sizeof(int64_t));
    }
    
    return data;
}

std::vector<bool> TypeAwareCompressor::bytesToBools(const std::vector<uint8_t>& data) {
    std::vector<bool> values;
    values.reserve(data.size() * 8);
    
    for (uint8_t byte : data) {
        for (int bit = 0; bit < 8; ++bit) {
            values.push_back((byte & (1 << bit)) != 0);
        }
    }
    
    return values;
}

std::vector<uint8_t> TypeAwareCompressor::boolsToBytes(const std::vector<bool>& values) {
    return bitPackingCompress(values);
}

std::vector<uint32_t> TypeAwareCompressor::bytesToUint32s(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> values;
    size_t count = data.size() / sizeof(uint32_t);
    values.reserve(count);
    
    for (size_t i = 0; i < count; ++i) {
        uint32_t value;
        std::memcpy(&value, &data[i * sizeof(uint32_t)], sizeof(uint32_t));
        values.push_back(value);
    }
    
    return values;
}

std::vector<uint8_t> TypeAwareCompressor::uint32sToBytes(const std::vector<uint32_t>& values) {
    std::vector<uint8_t> data;
    data.reserve(values.size() * sizeof(uint32_t));
    
    for (uint32_t value : values) {
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&value), 
                   reinterpret_cast<uint8_t*>(&value) + sizeof(uint32_t));
    }
    
    return data;
}

// ========== LOUDS位图压缩 ==========
std::vector<uint8_t> TypeAwareCompressor::compressLoudsBitmap(const std::vector<uint8_t>& bitmap) {
    return rleCompress(bitmap);
}

std::vector<uint8_t> TypeAwareCompressor::decompressLoudsBitmap(const std::vector<uint8_t>& compressed) {
    return rleDecompress(compressed);
}

// ========== Null值处理压缩算法 ==========
std::vector<uint8_t> TypeAwareCompressor::compressWithNullHandling(const std::vector<uint8_t>& data, FieldType type, const std::vector<bool>& null_mask) {
    std::vector<uint8_t> compressed;
    
    // 首先压缩null掩码
    auto null_compressed = bitPackingCompress(null_mask);
    
    // 写入null掩码大小
    uint32_t null_mask_size = static_cast<uint32_t>(null_compressed.size());
    compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&null_mask_size), 
                     reinterpret_cast<uint8_t*>(&null_mask_size) + sizeof(null_mask_size));
    
    // 写入null掩码数据
    compressed.insert(compressed.end(), null_compressed.begin(), null_compressed.end());
    
    // 根据类型压缩非null数据
    switch (type) {
        case FieldType::Int: {
            // 提取非null的整数值
            std::vector<int64_t> non_null_values;
            size_t data_pos = 0;
            for (size_t i = 0; i < null_mask.size(); ++i) {
                if (!null_mask[i]) {
                    int64_t value;
                    std::memcpy(&value, &data[data_pos], sizeof(int64_t));
                    non_null_values.push_back(value);
                    data_pos += sizeof(int64_t);
                }
            }
            
            // 压缩非null值
            auto values_compressed = deltaVarintCompress(non_null_values);
            compressed.insert(compressed.end(), values_compressed.begin(), values_compressed.end());
            break;
        }
        case FieldType::Double: {
            // 类似整数的处理
            std::vector<int64_t> non_null_values;
            size_t data_pos = 0;
            for (size_t i = 0; i < null_mask.size(); ++i) {
                if (!null_mask[i]) {
                    int64_t value;
                    std::memcpy(&value, &data[data_pos], sizeof(int64_t));
                    non_null_values.push_back(value);
                    data_pos += sizeof(int64_t);
                }
            }
            
            auto values_compressed = deltaVarintCompress(non_null_values);
            compressed.insert(compressed.end(), values_compressed.begin(), values_compressed.end());
            break;
        }
        case FieldType::Bool: {
            // 对于布尔值，null作为第三种状态处理
            std::vector<uint8_t> bool_states; // 0=false, 1=true, 2=null
            for (size_t i = 0; i < null_mask.size(); ++i) {
                if (null_mask[i]) {
                    bool_states.push_back(2); // null
                } else {
                    bool value = (data[i / 8] & (1 << (i % 8))) != 0;
                    bool_states.push_back(value ? 1 : 0);
                }
            }
            
            // 使用2位编码：00=false, 01=true, 10=null
            auto states_compressed = compressBoolStates(bool_states);
            compressed.insert(compressed.end(), states_compressed.begin(), states_compressed.end());
            break;
        }
        case FieldType::String:
        case FieldType::Timestamp:
        case FieldType::LogType:
        case FieldType::UnstructuredArray:
        case FieldType::StructuredArray: {
            // 对于编码值，null通常用特殊编码值表示
            // 直接压缩原始数据，null值在编码层面处理
            auto data_compressed = rleCompress(data);
            compressed.insert(compressed.end(), data_compressed.begin(), data_compressed.end());
            break;
        }
        default:
            // 默认处理
            auto data_compressed = rleCompress(data);
            compressed.insert(compressed.end(), data_compressed.begin(), data_compressed.end());
    }
    
    return compressed;
}

std::vector<uint8_t> TypeAwareCompressor::decompressWithNullHandling(const std::vector<uint8_t>& compressed, FieldType type, size_t total_count) {
    std::vector<uint8_t> decompressed;
    size_t pos = 0;
    
    // 读取null掩码大小
    uint32_t null_mask_size;
    std::memcpy(&null_mask_size, &compressed[pos], sizeof(null_mask_size));
    pos += sizeof(null_mask_size);
    
    // 解压null掩码
    std::vector<uint8_t> null_compressed(compressed.begin() + pos, compressed.begin() + pos + null_mask_size);
    pos += null_mask_size;
    std::vector<bool> null_mask = bitPackingDecompress(null_compressed, total_count);
    
    // 根据类型解压非null数据
    switch (type) {
        case FieldType::Int: {
            // 解压非null的整数值
            std::vector<uint8_t> values_compressed(compressed.begin() + pos, compressed.end());
            auto non_null_values = deltaVarintDecompress(values_compressed);
            
            // 重建完整数据，包括null值
            decompressed.resize(total_count * sizeof(int64_t), 0);
            size_t value_idx = 0;
            size_t data_pos = 0;
            
            for (size_t i = 0; i < total_count; ++i) {
                if (!null_mask[i]) {
                    int64_t value = non_null_values[value_idx++];
                    std::memcpy(&decompressed[data_pos], &value, sizeof(int64_t));
                }
                data_pos += sizeof(int64_t);
            }
            break;
        }
        case FieldType::Double: {
            // 类似整数的处理
            std::vector<uint8_t> values_compressed(compressed.begin() + pos, compressed.end());
            auto non_null_values = deltaVarintDecompress(values_compressed);
            
            decompressed.resize(total_count * sizeof(int64_t), 0);
            size_t value_idx = 0;
            size_t data_pos = 0;
            
            for (size_t i = 0; i < total_count; ++i) {
                if (!null_mask[i]) {
                    int64_t value = non_null_values[value_idx++];
                    std::memcpy(&decompressed[data_pos], &value, sizeof(int64_t));
                }
                data_pos += sizeof(int64_t);
            }
            break;
        }
        case FieldType::Bool: {
            // 解压布尔状态
            std::vector<uint8_t> states_compressed(compressed.begin() + pos, compressed.end());
            auto bool_states = decompressBoolStates(states_compressed, total_count);
            
            // 转换为位打包格式
            decompressed.resize((total_count + 7) / 8, 0);
            for (size_t i = 0; i < total_count; ++i) {
                if (bool_states[i] == 1) { // true
                    decompressed[i / 8] |= (1 << (i % 8));
                }
                // null值保持为0
            }
            break;
        }
        case FieldType::String:
        case FieldType::Timestamp:
        case FieldType::LogType:
        case FieldType::UnstructuredArray:
        case FieldType::StructuredArray: {
            // 直接解压原始数据
            std::vector<uint8_t> data_compressed(compressed.begin() + pos, compressed.end());
            decompressed = rleDecompress(data_compressed);
            break;
        }
        default:
            // 默认处理
            std::vector<uint8_t> data_compressed(compressed.begin() + pos, compressed.end());
            decompressed = rleDecompress(data_compressed);
    }
    
    return decompressed;
}

// 压缩布尔状态（支持null值）
std::vector<uint8_t> TypeAwareCompressor::compressBoolStates(const std::vector<uint8_t>& states) {
    std::vector<uint8_t> compressed;
    
    // 使用2位编码：00=false, 01=true, 10=null, 11=未使用
    for (size_t i = 0; i < states.size(); i += 4) {
        uint8_t byte = 0;
        for (int j = 0; j < 4 && i + j < states.size(); ++j) {
            uint8_t state = states[i + j];
            byte |= (state << (j * 2));
        }
        compressed.push_back(byte);
    }
    
    return compressed;
}

std::vector<uint8_t> TypeAwareCompressor::decompressBoolStates(const std::vector<uint8_t>& compressed, size_t count) {
    std::vector<uint8_t> states;
    states.reserve(count);
    
    for (size_t i = 0; i < compressed.size() && states.size() < count; ++i) {
        uint8_t byte = compressed[i];
        for (int j = 0; j < 4 && states.size() < count; ++j) {
            uint8_t state = (byte >> (j * 2)) & 0x03;
            states.push_back(state);
        }
    }
    
    return states;
}

// ========== 改进的分层数据压缩 ==========
std::vector<uint8_t> TypeAwareCompressor::compressLayerDataWithNulls(const std::vector<uint8_t>& data, FieldType type, const std::vector<bool>& null_mask) {
    // 检查是否有null值
    bool has_nulls = false;
    for (bool is_null : null_mask) {
        if (is_null) {
            has_nulls = true;
            break;
        }
    }
    
    if (has_nulls) {
        // 使用null感知压缩
        auto compressed = compressWithNullHandling(data, type, null_mask);
        compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::NULL_AWARE));
        return compressed;
    } else {
        // 使用普通压缩
        return compressLayerData(data, type);
    }
}

std::vector<uint8_t> TypeAwareCompressor::decompressLayerDataWithNulls(const std::vector<uint8_t>& compressed, FieldType type, size_t total_count) {
    if (compressed.empty()) return {};
    
    CompressionType comp_type = static_cast<CompressionType>(compressed[0]);
    
    if (comp_type == CompressionType::NULL_AWARE) {
        std::vector<uint8_t> data(compressed.begin() + 1, compressed.end());
        return decompressWithNullHandling(data, type, total_count);
    } else {
        return decompressLayerData(compressed, type);
    }
}

// ========== 字典数据压缩 ==========
std::vector<uint8_t> TypeAwareCompressor::compressDictionaryData(const FieldDictionaryManager& manager) {
    std::vector<uint8_t> compressed;
    
    // 1. 压缩String字典
    std::vector<FieldKey> all_field_keys = manager.getAllFieldsAndTypes();
    std::vector<std::string> string_values;
    const Dictionary& dict = manager.variableDict();
    
    for (const auto& fk : all_field_keys) {
        if (fk.type == FieldType::String || fk.type == FieldType::UnstructuredArray || fk.type == FieldType::StructuredArray) {
            size_t count = dict.getFieldValueCount(fk);
            for (uint32_t code = 1; code <= count; ++code) {
                auto opt_value = dict.getFieldValueByCode(fk, code);
                if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                    string_values.push_back(std::get<std::string>(*opt_value));
                }
            }
        }
    }
    
    auto string_compressed = dictionaryCompress(string_values);
    
    // 写入String字典大小
    uint32_t string_dict_size = static_cast<uint32_t>(string_compressed.size());
    compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&string_dict_size), 
                     reinterpret_cast<uint8_t*>(&string_dict_size) + sizeof(string_dict_size));
    compressed.insert(compressed.end(), string_compressed.begin(), string_compressed.end());
    
    // 2. 压缩Timestamp字典
    const auto& ts_dict = manager.timestampDict();
    std::vector<std::string> ts_templates;
    std::vector<std::string> ts_variables;
    
    // 收集模板
    for (uint32_t i = 1; i <= ts_dict.getTemplateCount(); ++i) {
        std::string template_str = ts_dict.getTemplateById(i);
        if (!template_str.empty()) {
            ts_templates.push_back(template_str);
        }
    }
    
    // 收集变量
    for (uint32_t i = 1; i <= ts_dict.getVariableCount(); ++i) {
        std::string variable = ts_dict.getVariableByCode(i);
        if (!variable.empty()) {
            ts_variables.push_back(variable);
        }
    }
    
    auto ts_templates_compressed = dictionaryCompress(ts_templates);
    auto ts_variables_compressed = dictionaryCompress(ts_variables);
    
    // 写入Timestamp字典大小
    uint32_t ts_templates_size = static_cast<uint32_t>(ts_templates_compressed.size());
    uint32_t ts_variables_size = static_cast<uint32_t>(ts_variables_compressed.size());
    compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&ts_templates_size), 
                     reinterpret_cast<uint8_t*>(&ts_templates_size) + sizeof(ts_templates_size));
    compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&ts_variables_size), 
                     reinterpret_cast<uint8_t*>(&ts_variables_size) + sizeof(ts_variables_size));
    compressed.insert(compressed.end(), ts_templates_compressed.begin(), ts_templates_compressed.end());
    compressed.insert(compressed.end(), ts_variables_compressed.begin(), ts_variables_compressed.end());
    
    // 3. 压缩LogType字典
    const auto& log_dict = manager.logtypeDict();
    std::vector<std::string> log_templates;
    std::vector<std::string> log_variables;
    
    // 收集LogType模板
    for (uint32_t i = 1; i <= log_dict.getLogTypeCount(); ++i) {
        log_templates.push_back(log_dict.getLogTypeById(i));
    }
    
    // 收集LogType变量
    for (uint32_t i = 1; i < 100000; ++i) { // 假设变量数不会超过10万
        std::string var = log_dict.decodeVariable(i);
        if (var.empty()) break;
        log_variables.push_back(var);
    }
    
    auto log_templates_compressed = dictionaryCompress(log_templates);
    auto log_variables_compressed = dictionaryCompress(log_variables);
    
    // 写入LogType字典大小
    uint32_t log_templates_size = static_cast<uint32_t>(log_templates_compressed.size());
    uint32_t log_variables_size = static_cast<uint32_t>(log_variables_compressed.size());
    compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&log_templates_size), 
                     reinterpret_cast<uint8_t*>(&log_templates_size) + sizeof(log_templates_size));
    compressed.insert(compressed.end(), reinterpret_cast<uint8_t*>(&log_variables_size), 
                     reinterpret_cast<uint8_t*>(&log_variables_size) + sizeof(log_variables_size));
    compressed.insert(compressed.end(), log_templates_compressed.begin(), log_templates_compressed.end());
    compressed.insert(compressed.end(), log_variables_compressed.begin(), log_variables_compressed.end());
    
    return compressed;
}

std::unique_ptr<FieldDictionaryManager> TypeAwareCompressor::decompressDictionaryData(const std::vector<uint8_t>& compressed, const std::vector<FieldKey>& field_keys) {
    auto manager = std::make_unique<FieldDictionaryManager>();
    size_t pos = 0;
    
    // 1. 解压String字典
    uint32_t string_dict_size;
    std::memcpy(&string_dict_size, &compressed[pos], sizeof(string_dict_size));
    pos += sizeof(string_dict_size);
    
    std::vector<uint8_t> string_compressed(compressed.begin() + pos, compressed.begin() + pos + string_dict_size);
    pos += string_dict_size;
    auto string_values = dictionaryDecompress(string_compressed);
    
    // 重建String字典
    if (!field_keys.empty()) {
        Dictionary& dict = manager->variableDict();
        size_t value_idx = 0;
        for (const auto& fk : field_keys) {
            if (fk.type == FieldType::String || fk.type == FieldType::UnstructuredArray || fk.type == FieldType::StructuredArray) {
                // 为每个String字段重建字典
                // 这里假设每个字段的字符串值按顺序存储
                // 实际实现中可能需要更复杂的逻辑来确定每个字段有多少个值
                size_t values_per_field = string_values.size() / field_keys.size(); // 简化处理
                for (size_t i = 0; i < values_per_field && value_idx < string_values.size(); ++i) {
                    dict.addFieldValue(fk, fk.type, string_values[value_idx++]);
                }
            }
        }
    }
    
    // 2. 解压Timestamp字典
    uint32_t ts_templates_size, ts_variables_size;
    std::memcpy(&ts_templates_size, &compressed[pos], sizeof(ts_templates_size));
    pos += sizeof(ts_templates_size);
    std::memcpy(&ts_variables_size, &compressed[pos], sizeof(ts_variables_size));
    pos += sizeof(ts_variables_size);
    
    std::vector<uint8_t> ts_templates_compressed(compressed.begin() + pos, compressed.begin() + pos + ts_templates_size);
    pos += ts_templates_size;
    std::vector<uint8_t> ts_variables_compressed(compressed.begin() + pos, compressed.begin() + pos + ts_variables_size);
    pos += ts_variables_size;
    
    auto ts_templates = dictionaryDecompress(ts_templates_compressed);
    auto ts_variables = dictionaryDecompress(ts_variables_compressed);
    
    // 重建Timestamp字典
    auto& ts_dict = manager->timestampDict();
    for (const auto& template_str : ts_templates) {
        if (!template_str.empty()) {
            ts_dict.registerTemplate(template_str);
        }
    }
    for (const auto& variable : ts_variables) {
        if (!variable.empty()) {
            ts_dict.registerVariable(variable);
        }
    }
    
    // 3. 解压LogType字典
    uint32_t log_templates_size, log_variables_size;
    std::memcpy(&log_templates_size, &compressed[pos], sizeof(log_templates_size));
    pos += sizeof(log_templates_size);
    std::memcpy(&log_variables_size, &compressed[pos], sizeof(log_variables_size));
    pos += sizeof(log_variables_size);
    
    std::vector<uint8_t> log_templates_compressed(compressed.begin() + pos, compressed.begin() + pos + log_templates_size);
    pos += log_templates_size;
    std::vector<uint8_t> log_variables_compressed(compressed.begin() + pos, compressed.begin() + pos + log_variables_size);
    pos += log_variables_size;
    
    auto log_templates = dictionaryDecompress(log_templates_compressed);
    auto log_variables = dictionaryDecompress(log_variables_compressed);
    
    // 重建LogType字典
    auto& log_dict = manager->logtypeDict();
    for (const auto& template_str : log_templates) {
        log_dict.addLogType(template_str);
    }
    for (const auto& variable : log_variables) {
        log_dict.encodeVariable(variable);
    }
    
    return manager;
}

// ========== 主要压缩接口 ==========
CompressedData TypeAwareCompressor::compressLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, const std::vector<FieldKey>& field_order) {
    CompressedData result;
    
    // 1. 压缩LOUDS位图
    std::ostringstream bv_stream(std::ios::binary);
    louds.serializeBitmap(bv_stream);
    std::string bv_str = bv_stream.str();
    std::vector<uint8_t> bv_vec(bv_str.begin(), bv_str.end());
    result.trie_data = compressLoudsBitmap(bv_vec);
    
    // 2. 压缩分层数据
    size_t layer_count = louds.getLayeredStorage().getLayerCount();
    std::vector<std::vector<uint8_t>> compressed_layers;
    std::vector<uint32_t> layer_sizes;
    
    for (size_t i = 0; i < layer_count; ++i) {
        std::ostringstream layer_stream(std::ios::binary);
        louds.getLayeredStorage().serializeLayer(i, layer_stream);
        std::string layer_str = layer_stream.str();
        std::vector<uint8_t> layer_vec(layer_str.begin(), layer_str.end());
        
        FieldType type = field_order[i].type;
        auto compressed = compressLayerData(layer_vec, type);
        layer_sizes.push_back(static_cast<uint32_t>(compressed.size()));
        compressed_layers.push_back(std::move(compressed));
    }
    
    // 存储分层数据
    std::vector<uint8_t> layer_data;
    uint32_t layer_count_u32 = static_cast<uint32_t>(layer_count);
    layer_data.insert(layer_data.end(), reinterpret_cast<uint8_t*>(&layer_count_u32), 
                     reinterpret_cast<uint8_t*>(&layer_count_u32) + sizeof(layer_count_u32));
    
    for (uint32_t sz : layer_sizes) {
        layer_data.insert(layer_data.end(), reinterpret_cast<uint8_t*>(&sz), 
                         reinterpret_cast<uint8_t*>(&sz) + sizeof(sz));
    }
    
    for (const auto& block : compressed_layers) {
        layer_data.insert(layer_data.end(), block.begin(), block.end());
    }
    result.layer_data = std::move(layer_data);
    
    // 3. 压缩字典数据
    result.dictionary_data = compressDictionaryData(manager);
    
    // 4. 压缩元数据
    result.metadata_data = Compressor::serializeMetadata(field_order, manager);
    
    // 计算大小 - 与原始Compressor保持一致
    size_t layers_raw_total = 0;
    for (size_t i = 0; i < layer_count; ++i) {
        std::ostringstream layer_stream(std::ios::binary);
        louds.getLayeredStorage().serializeLayer(i, layer_stream);
        std::string layer_str = layer_stream.str();
        layers_raw_total += layer_str.size();
    }
    
    // 计算字典原始大小
    std::vector<uint8_t> string_dict_raw, ts_dict_raw, log_templates_raw, log_vars_raw;
    
    // String字典原始大小
    std::vector<FieldKey> all_field_keys = manager.getAllFieldsAndTypes();
    std::vector<std::string> string_values;
    const Dictionary& dict = manager.variableDict();
    for (const auto& fk : all_field_keys) {
        if (fk.type == FieldType::String || fk.type == FieldType::UnstructuredArray || fk.type == FieldType::StructuredArray) {
            size_t count = dict.getFieldValueCount(fk);
            for (uint32_t code = 1; code <= count; ++code) {
                auto opt_value = dict.getFieldValueByCode(fk, code);
                if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                    string_values.push_back(std::get<std::string>(*opt_value));
                }
            }
        }
    }
    
    // 序列化String字典原始数据
    uint32_t string_dict_count = 0;
    std::vector<std::pair<FieldKey, std::vector<std::string>>> string_dicts;
    for (size_t fk_idx = 0; fk_idx < all_field_keys.size(); ++fk_idx) {
        const auto& fk = all_field_keys[fk_idx];
        if (fk.type == FieldType::String || fk.type == FieldType::UnstructuredArray || fk.type == FieldType::StructuredArray) {
            std::vector<std::string> values;
            size_t count = dict.getFieldValueCount(fk);
            for (uint32_t code = 1; code <= count; ++code) {
                auto opt_value = dict.getFieldValueByCode(fk, code);
                if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                    values.push_back(std::get<std::string>(*opt_value));
                }
            }
            if (!values.empty()) {
                string_dicts.emplace_back(fk, values);
                ++string_dict_count;
            }
        }
    }
    
    // 序列化原始数据用于计算大小
    writeValue(string_dict_raw, string_dict_count);
    for (size_t i = 0; i < string_dicts.size(); ++i) {
        const auto& [fk, values] = string_dicts[i];
        writeString(string_dict_raw, fk.name);
        writeValue(string_dict_raw, static_cast<uint32_t>(fk.type));
        writeVector(string_dict_raw, values);
    }
    
    // Timestamp字典原始大小
    const auto& ts_dict = manager.timestampDict();
    std::vector<std::string> templates;
    for (uint32_t i = 1; i <= ts_dict.getTemplateCount(); ++i) {
        std::string template_str = ts_dict.getTemplateById(i);
        if (!template_str.empty()) {
            templates.push_back(template_str);
        }
    }
    writeVector(ts_dict_raw, templates);
    std::vector<std::string> variables;
    for (uint32_t i = 1; i <= ts_dict.getVariableCount(); ++i) {
        std::string variable = ts_dict.getVariableByCode(i);
        if (!variable.empty()) {
            variables.push_back(variable);
        }
    }
    writeVector(ts_dict_raw, variables);
    
    // LogType字典原始大小
    const auto& log_dict = manager.logtypeDict();
    std::vector<std::string> log_templates;
    for (uint32_t i = 1; i <= log_dict.getLogTypeCount(); ++i) {
        log_templates.push_back(log_dict.getLogTypeById(i));
    }
    writeVector(log_templates_raw, log_templates);
    std::vector<std::string> log_vars;
    for (uint32_t i = 1; i < 100000; ++i) {
        std::string var = log_dict.decodeVariable(i);
        if (var.empty()) break;
        log_vars.push_back(var);
    }
    writeVector(log_vars_raw, log_vars);
    
    // 元数据原始大小
    std::vector<uint8_t> meta_raw = Compressor::serializeMetadata(field_order, manager);
    
    result.original_size = bv_str.size() + layers_raw_total + string_dict_raw.size() + 
                          ts_dict_raw.size() + log_templates_raw.size() + log_vars_raw.size() + meta_raw.size();
    result.compressed_size = result.trie_data.size() + result.layer_data.size() + 
                           result.dictionary_data.size() + result.metadata_data.size();
    
    return result;
}

// ========== 原始分层数据压缩 ==========
std::vector<uint8_t> TypeAwareCompressor::compressLayerData(const std::vector<uint8_t>& data, FieldType type) {
    switch (type) {
        case FieldType::Int: {
            auto ints = bytesToInt64s(data);
            auto compressed = deltaVarintCompress(ints);
            // 添加类型标记
            compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::DELTA_VARINT));
            return compressed;
        }
        case FieldType::Double: {
            // 对于double，先转换为int64，然后使用delta编码
            auto ints = bytesToInt64s(data);
            auto compressed = deltaVarintCompress(ints);
            compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::DELTA_VARINT));
            return compressed;
        }
        case FieldType::Bool: {
            auto bools = bytesToBools(data);
            auto compressed = bitPackingCompress(bools);
            compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::BIT_PACKING));
            return compressed;
        }
        case FieldType::String:
        case FieldType::UnstructuredArray:
        case FieldType::StructuredArray: {
            // 对于字符串编码值，尝试解析为编码序列
            // 假设数据是uint32_t编码值的序列
            if (data.size() % sizeof(uint32_t) == 0) {
                auto codes = bytesToUint32s(data);
                
                // 对编码序列使用delta编码
                std::vector<int64_t> code_ints(codes.begin(), codes.end());
                auto compressed = deltaVarintCompress(code_ints);
                compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::DELTA_VARINT));
                return compressed;
            } else {
                // 如果不是编码序列，使用RLE
                auto compressed = rleCompress(data);
                compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::RLE));
                return compressed;
            }
        }
        case FieldType::Timestamp: {
            auto ints = bytesToInt64s(data);
            auto compressed = deltaDeltaCompress(ints);
            compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::DELTA_DELTA));
            return compressed;
        }
        case FieldType::LogType: {
            // 对于LogType编码值，类似String的处理
            // 假设数据是uint32_t编码值的序列
            if (data.size() % sizeof(uint32_t) == 0) {
                auto codes = bytesToUint32s(data);
                
                // 对编码序列使用delta编码
                std::vector<int64_t> code_ints(codes.begin(), codes.end());
                auto compressed = deltaVarintCompress(code_ints);
                compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::DELTA_VARINT));
                return compressed;
            } else {
                // 如果不是编码序列，使用RLE
                auto compressed = rleCompress(data);
                compressed.insert(compressed.begin(), static_cast<uint8_t>(CompressionType::RLE));
                return compressed;
            }
        }
        default:
            return data;
    }
}

std::vector<uint8_t> TypeAwareCompressor::decompressLayerData(const std::vector<uint8_t>& compressed, FieldType type) {
    if (compressed.empty()) return {};
    
    CompressionType comp_type = static_cast<CompressionType>(compressed[0]);
    std::vector<uint8_t> data(compressed.begin() + 1, compressed.end());
    
    switch (comp_type) {
        case CompressionType::DELTA_VARINT: {
            auto ints = deltaVarintDecompress(data);
            
            // 对于String、LogType和数组类型，需要转换回uint32_t编码序列
            if (type == FieldType::String || type == FieldType::LogType || 
                type == FieldType::UnstructuredArray || type == FieldType::StructuredArray) {
                std::vector<uint32_t> codes(ints.begin(), ints.end());
                return uint32sToBytes(codes);
            } else {
                return int64sToBytes(ints);
            }
        }
        case CompressionType::BIT_PACKING: {
            // 需要知道原始bool值的数量，这里假设data.size() * 8
            auto bools = bitPackingDecompress(data, data.size() * 8);
            return boolsToBytes(bools);
        }
        case CompressionType::DELTA_DELTA: {
            auto ints = deltaDeltaDecompress(data);
            return int64sToBytes(ints);
        }
        case CompressionType::RLE:
        default:
            return rleDecompress(data);
    }
}

std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
TypeAwareCompressor::decompressLouds(const CompressedData& compressed_data) {
    // 1. 解压LOUDS位图
    std::vector<uint8_t> bv_vec = decompressLoudsBitmap(compressed_data.trie_data);
    std::istringstream bv_stream(std::string(bv_vec.begin(), bv_vec.end()), std::ios::binary);
    auto louds = std::make_unique<LOUDSTrie>();
    louds->deserializeBitmap(bv_stream);
    
    // 2. 解压分层数据
    const std::vector<uint8_t>& layer_data = compressed_data.layer_data;
    size_t offset = 0;
    
    uint32_t layer_count = 0;
    std::memcpy(&layer_count, &layer_data[offset], sizeof(layer_count));
    offset += sizeof(layer_count);
    
    std::vector<uint32_t> layer_sizes(layer_count);
    for (uint32_t i = 0; i < layer_count; ++i) {
        std::memcpy(&layer_sizes[i], &layer_data[offset], sizeof(uint32_t));
        offset += sizeof(uint32_t);
    }
    
    // 4. 先解压元数据以获取字段类型信息
    std::vector<FieldKey> ordered_field_keys = Compressor::deserializeMetadata(compressed_data.metadata_data);
    louds->setFieldOrder(ordered_field_keys);
    
    // 初始化分层存储
    for (uint32_t i = 0; i < layer_count; ++i) {
        if (i >= louds->getLayeredStorage().getLayerCount()) {
            FieldKey fk = (i < ordered_field_keys.size()) ? ordered_field_keys[i] : FieldKey{"", FieldType::String};
            louds->getLayeredStorage().addLayer(fk, 0);
        }
    }
    
    // 解压每层数据
    for (uint32_t i = 0; i < layer_count; ++i) {
        std::vector<uint8_t> compressed_layer(layer_data.begin() + offset, 
                                            layer_data.begin() + offset + layer_sizes[i]);
        offset += layer_sizes[i];
        
        // 使用正确的字段类型
        FieldType type = (i < ordered_field_keys.size()) ? ordered_field_keys[i].type : FieldType::String;
        std::vector<uint8_t> layer_raw = decompressLayerData(compressed_layer, type);
        std::istringstream layer_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
        louds->getLayeredStorage().deserializeLayer(i, layer_stream);
    }
    
    // 3. 解压字典数据
    auto manager = decompressDictionaryData(compressed_data.dictionary_data, ordered_field_keys);
    
    return {std::move(louds), std::move(manager)};
}

// ========== 内存序列化接口 ==========
std::vector<uint8_t> TypeAwareCompressor::saveToMemory(const CompressedData& compressed_data) {
    std::vector<uint8_t> buffer;
    
    // Magic number for type-aware compressor
    const char* magic = "JSON2CTYP";
    buffer.insert(buffer.end(), magic, magic + 9);
    
    // 版本
    uint32_t version = 1;
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&version), 
                 reinterpret_cast<const uint8_t*>(&version) + sizeof(version));
    
    // 各部分大小
    uint32_t trie_size = static_cast<uint32_t>(compressed_data.trie_data.size());
    uint32_t layer_size = static_cast<uint32_t>(compressed_data.layer_data.size());
    uint32_t dict_size = static_cast<uint32_t>(compressed_data.dictionary_data.size());
    uint32_t metadata_size = static_cast<uint32_t>(compressed_data.metadata_data.size());
    
    buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(&trie_size), 
                 reinterpret_cast<uint8_t*>(&trie_size) + sizeof(trie_size));
    buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(&layer_size), 
                 reinterpret_cast<uint8_t*>(&layer_size) + sizeof(layer_size));
    buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(&dict_size), 
                 reinterpret_cast<uint8_t*>(&dict_size) + sizeof(dict_size));
    buffer.insert(buffer.end(), reinterpret_cast<uint8_t*>(&metadata_size), 
                 reinterpret_cast<uint8_t*>(&metadata_size) + sizeof(metadata_size));
    
    // 数据
    buffer.insert(buffer.end(), compressed_data.trie_data.begin(), compressed_data.trie_data.end());
    buffer.insert(buffer.end(), compressed_data.layer_data.begin(), compressed_data.layer_data.end());
    buffer.insert(buffer.end(), compressed_data.dictionary_data.begin(), compressed_data.dictionary_data.end());
    buffer.insert(buffer.end(), compressed_data.metadata_data.begin(), compressed_data.metadata_data.end());
    
    return buffer;
}

CompressedData TypeAwareCompressor::loadFromMemory(const std::vector<uint8_t>& buffer) {
    CompressedData result;
    size_t offset = 0;
    
    // 检查Magic number
    if (buffer.size() < 9) throw std::runtime_error("Buffer too small for header");
    std::string magic(reinterpret_cast<const char*>(&buffer[offset]), 9);
    if (magic != "JSON2CTYP") throw std::runtime_error("Invalid buffer format for type-aware compressor");
    offset += 9;
    
    // 读取版本
    if (buffer.size() < offset + sizeof(uint32_t)) throw std::runtime_error("Buffer too small for version");
    uint32_t version;
    std::memcpy(&version, &buffer[offset], sizeof(version));
    if (version != 1) throw std::runtime_error("Unsupported buffer version");
    offset += sizeof(uint32_t);
    
    // 读取各部分大小
    if (buffer.size() < offset + 4 * sizeof(uint32_t)) throw std::runtime_error("Buffer too small for sizes");
    uint32_t trie_size, layer_size, dict_size, metadata_size;
    std::memcpy(&trie_size, &buffer[offset], sizeof(trie_size)); offset += sizeof(trie_size);
    std::memcpy(&layer_size, &buffer[offset], sizeof(layer_size)); offset += sizeof(layer_size);
    std::memcpy(&dict_size, &buffer[offset], sizeof(dict_size)); offset += sizeof(dict_size);
    std::memcpy(&metadata_size, &buffer[offset], sizeof(metadata_size)); offset += sizeof(metadata_size);
    
    // 读取各部分数据
    if (buffer.size() < offset + trie_size + layer_size + dict_size + metadata_size)
        throw std::runtime_error("Buffer too small for data");
    
    result.trie_data.assign(buffer.begin() + offset, buffer.begin() + offset + trie_size); 
    offset += trie_size;
    result.layer_data.assign(buffer.begin() + offset, buffer.begin() + offset + layer_size); 
    offset += layer_size;
    result.dictionary_data.assign(buffer.begin() + offset, buffer.begin() + offset + dict_size); 
    offset += dict_size;
    result.metadata_data.assign(buffer.begin() + offset, buffer.begin() + offset + metadata_size); 
    offset += metadata_size;
    
    result.compressed_size = trie_size + layer_size + dict_size + metadata_size;
    result.original_size = 0;
    
    return result;
}

// ========== 与Compressor相同的接口实现 ==========
CompressedData TypeAwareCompressor::compress(const Trie& trie, const FieldDictionaryManager& manager) {
    // 1. 构建 LOUDS Trie
    LOUDSTrie louds(trie.getOrderedFields());
    louds.buildFromTrie(trie);
    // 2. 用 LOUDS Trie 进行压缩
    return compressLouds(louds, manager, trie.getOrderedFields());
}

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>>
TypeAwareCompressor::decompress(const CompressedData& compressed_data) {
    // 1. 先解压 LOUDS Trie 和字典
    auto [louds, manager] = decompressLouds(compressed_data);
    // 2. LOUDS Trie 转 Trie
    auto trie = std::make_unique<Trie>(louds->getFieldOrder());
    loudsToTrie(*louds, *trie);
    return {std::move(trie), std::move(manager)};
}

// 计算压缩率
double TypeAwareCompressor::getCompressionRatio(const CompressedData& compressed_data) {
    if (compressed_data.original_size == 0) return 0.0;
    return static_cast<double>(compressed_data.compressed_size) / compressed_data.original_size;
}

// 保存压缩数据到文件
bool TypeAwareCompressor::saveToFile(const CompressedData& compressed_data, const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;
    // 写入文件头
    const char* magic = "JSON2CTYP"; // 使用不同的magic number区分类型感知压缩
    file.write(magic, 9);
    // 写入版本信息
    uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    // 写入各个组件的大小
    uint32_t trie_size = static_cast<uint32_t>(compressed_data.trie_data.size());
    uint32_t layer_size = static_cast<uint32_t>(compressed_data.layer_data.size());
    uint32_t dict_size = static_cast<uint32_t>(compressed_data.dictionary_data.size());
    uint32_t metadata_size = static_cast<uint32_t>(compressed_data.metadata_data.size());
    file.write(reinterpret_cast<const char*>(&trie_size), sizeof(trie_size));
    file.write(reinterpret_cast<const char*>(&layer_size), sizeof(layer_size));
    file.write(reinterpret_cast<const char*>(&dict_size), sizeof(dict_size));
    file.write(reinterpret_cast<const char*>(&metadata_size), sizeof(metadata_size));
    // 写入各个组件的数据
    file.write(reinterpret_cast<const char*>(compressed_data.trie_data.data()), trie_size);
    file.write(reinterpret_cast<const char*>(compressed_data.layer_data.data()), layer_size);
    file.write(reinterpret_cast<const char*>(compressed_data.dictionary_data.data()), dict_size);
    file.write(reinterpret_cast<const char*>(compressed_data.metadata_data.data()), metadata_size);
    return true;
}

// 从文件加载压缩数据
CompressedData TypeAwareCompressor::loadFromFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }
    CompressedData result;
    // 读取文件头
    char magic[9];
    file.read(magic, 9);
    if (std::string(magic, 9) != "JSON2CTYP") {
        throw std::runtime_error("Invalid file format for type-aware compressor");
    }
    // 读取版本信息
    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) {
        throw std::runtime_error("Unsupported file version");
    }
    // 读取各个组件的大小
    uint32_t trie_size, layer_size, dict_size, metadata_size;
    file.read(reinterpret_cast<char*>(&trie_size), sizeof(trie_size));
    file.read(reinterpret_cast<char*>(&layer_size), sizeof(layer_size));
    file.read(reinterpret_cast<char*>(&dict_size), sizeof(dict_size));
    file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
    // 读取各个组件的数据
    result.trie_data.resize(trie_size);
    result.layer_data.resize(layer_size);
    result.dictionary_data.resize(dict_size);
    result.metadata_data.resize(metadata_size);
    file.read(reinterpret_cast<char*>(result.trie_data.data()), trie_size);
    file.read(reinterpret_cast<char*>(result.layer_data.data()), layer_size);
    file.read(reinterpret_cast<char*>(result.dictionary_data.data()), dict_size);
    file.read(reinterpret_cast<char*>(result.metadata_data.data()), metadata_size);
    // 计算大小信息
    result.compressed_size = trie_size + layer_size + dict_size + metadata_size;
    result.original_size = 0;
    return result;
}

} // namespace json2
