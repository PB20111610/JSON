#include "../include/compress.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <algorithm>

// 如果系统支持Zstandard，则包含相关头文件
#ifdef USE_ZSTD
#include <zstd.h>
#endif

namespace json2 {

// 简单的序列化辅助函数
template<typename T>
void writeValue(std::vector<uint8_t>& data, const T& value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(T));
}

template<typename T>
T readValue(const std::vector<uint8_t>& data, size_t& pos) {
    T value;
    std::memcpy(&value, &data[pos], sizeof(T));
    pos += sizeof(T);
    return value;
}

void writeString(std::vector<uint8_t>& data, const std::string& str) {
    uint32_t length = static_cast<uint32_t>(str.length());
    writeValue(data, length);
    data.insert(data.end(), str.begin(), str.end());
}

std::string readString(const std::vector<uint8_t>& data, size_t& pos) {
    uint32_t length = readValue<uint32_t>(data, pos);
    std::string str(reinterpret_cast<const char*>(&data[pos]), length);
    pos += length;
    return str;
}

void writeVector(std::vector<uint8_t>& data, const std::vector<std::string>& vec) {
    uint32_t size = static_cast<uint32_t>(vec.size());
    writeValue(data, size);
    for (const auto& item : vec) {
        writeString(data, item);
    }
}

std::vector<std::string> readVector(const std::vector<uint8_t>& data, size_t& pos) {
    uint32_t size = readValue<uint32_t>(data, pos);
    std::vector<std::string> vec;
    vec.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        vec.push_back(readString(data, pos));
    }
    return vec;
}

// 序列化字典数据
std::vector<uint8_t> Compressor::serializeDictionary(const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    
    // 序列化字段类型信息
    const auto& all_fields_and_types = manager.getAllFieldsAndTypes();
    uint32_t field_count = static_cast<uint32_t>(all_fields_and_types.size());
    writeValue(data, field_count);
    
    for (const auto& [field, type] : all_fields_and_types) {
        writeString(data, field);
        writeValue(data, static_cast<uint32_t>(type));
    }
    
    // 只序列化需要字典编码的类型：String、Timestamp、LogType
    // 对于数值型和布尔型，Trie节点直接存储原始值，不需要字典
    
    // 序列化VariableDict（String类型）
    const Dictionary& dict = manager.variableDict();
    
    // 收集所有String类型的字段值
    std::unordered_map<std::string, std::vector<std::string>> string_dicts;
    for (const auto& [field, type] : all_fields_and_types) {
        if (type == FieldType::String || type == FieldType::Timestamp || type == FieldType::LogType) {
            std::vector<std::string> values;
            size_t count = dict.getFieldValueCount(field, type);
            for (uint32_t code = 1; code <= count; ++code) {
                auto opt_value = dict.getFieldValueByCode(field, type, code);
                if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                    values.push_back(std::get<std::string>(*opt_value));
                }
            }
            if (!values.empty()) {
                string_dicts[field] = values;
            }
        }
    }
    
    // 写入String字典数量
    uint32_t string_dict_count = static_cast<uint32_t>(string_dicts.size());
    writeValue(data, string_dict_count);
    
    // 写入每个String字典
    for (const auto& [field, values] : string_dicts) {
        writeString(data, field);
        writeVector(data, values);
    }
    
    return data;
}

// 反序列化字典数据
std::unique_ptr<FieldDictionaryManager> Compressor::deserializeDictionary(const std::vector<uint8_t>& data) {
    auto manager = std::make_unique<FieldDictionaryManager>();
    size_t pos = 0;
    
    // 反序列化字段类型信息
    uint32_t field_count = readValue<uint32_t>(data, pos);
    std::unordered_map<std::string, FieldType> field_types;
    
    for (uint32_t i = 0; i < field_count; ++i) {
        std::string field = readString(data, pos);
        FieldType type = static_cast<FieldType>(readValue<uint32_t>(data, pos));
        field_types[field] = type;
    }
    
    // 反序列化String字典（只包含String、Timestamp、LogType）
    Dictionary& dict = manager->variableDict();
    
    // 读取String字典数量
    uint32_t string_dict_count = readValue<uint32_t>(data, pos);
    
    // 读取每个String字典
    for (uint32_t i = 0; i < string_dict_count; ++i) {
        std::string field = readString(data, pos);
        std::vector<std::string> values = readVector(data, pos);
        
        // 获取字段的实际类型
        auto type_it = field_types.find(field);
        if (type_it != field_types.end()) {
            FieldType field_type = type_it->second;
            
            // 重建字典映射
            for (size_t j = 0; j < values.size(); ++j) {
                Value value = values[j];
                dict.addFieldValue(field, field_type, value);
            }
        }
    }
    
    return manager;
}

// 序列化元数据
std::vector<uint8_t> Compressor::serializeMetadata(const Trie& trie, const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    
    // 序列化有序字段列表
    const auto& ordered_fields = trie.getOrderedFields();
    writeVector(data, ordered_fields);
    
    // 序列化字段类型映射
    const auto& all_fields_and_types = manager.getAllFieldsAndTypes();
    uint32_t type_count = static_cast<uint32_t>(all_fields_and_types.size());
    writeValue(data, type_count);
    
    for (const auto& [field, type] : all_fields_and_types) {
        writeString(data, field);
        writeValue(data, static_cast<uint32_t>(type));
    }
    
    return data;
}

// 反序列化元数据
std::pair<std::vector<std::string>, std::unordered_map<std::string, FieldType>> 
Compressor::deserializeMetadata(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    
    // 反序列化有序字段列表
    std::vector<std::string> ordered_fields = readVector(data, pos);
    
    // 反序列化字段类型映射
    std::unordered_map<std::string, FieldType> field_types;
    uint32_t type_count = readValue<uint32_t>(data, pos);
    
    for (uint32_t i = 0; i < type_count; ++i) {
        std::string field = readString(data, pos);
        FieldType type = static_cast<FieldType>(readValue<uint32_t>(data, pos));
        field_types[field] = type;
    }
    
    return {ordered_fields, field_types};
}

// 使用Zstandard压缩数据
std::vector<uint8_t> Compressor::compressWithZstd(const std::vector<uint8_t>& data) {
#ifdef USE_ZSTD
    size_t compressed_size = ZSTD_compressBound(data.size());
    std::vector<uint8_t> compressed_data(compressed_size);
    
    size_t actual_size = ZSTD_compress(compressed_data.data(), compressed_size,
                                      data.data(), data.size(), ZSTD_CLEVEL_DEFAULT);
    
    if (ZSTD_isError(actual_size)) {
        throw std::runtime_error("ZSTD compression failed");
    }
    
    compressed_data.resize(actual_size);
    return compressed_data;
#else
    // 如果没有ZSTD，返回原始数据
    return data;
#endif
}

// 使用Zstandard解压缩数据
std::vector<uint8_t> Compressor::decompressWithZstd(const std::vector<uint8_t>& compressed_data) {
#ifdef USE_ZSTD
    size_t decompressed_size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("ZSTD decompression failed: invalid frame");
    }
    
    std::vector<uint8_t> decompressed_data(decompressed_size);
    size_t actual_size = ZSTD_decompress(decompressed_data.data(), decompressed_size,
                                        compressed_data.data(), compressed_data.size());
    
    if (ZSTD_isError(actual_size)) {
        throw std::runtime_error("ZSTD decompression failed");
    }
    
    decompressed_data.resize(actual_size);
    return decompressed_data;
#else
    // 如果没有ZSTD，返回原始数据
    return compressed_data;
#endif
}

// 压缩Trie树和相关数据
CompressedData Compressor::compress(const Trie& trie, const FieldDictionaryManager& manager) {
    CompressedData result;
    
    // 序列化各个组件
    std::vector<uint8_t> trie_serialized = trie.serialize();
    std::vector<uint8_t> dict_serialized = serializeDictionary(manager);
    std::vector<uint8_t> metadata_serialized = serializeMetadata(trie, manager);
    
    std::cout << "Debug - Serialized sizes:\n";
    std::cout << "  Trie: " << trie_serialized.size() << " bytes\n";
    std::cout << "  Dictionary: " << dict_serialized.size() << " bytes\n";
    std::cout << "  Metadata: " << metadata_serialized.size() << " bytes\n";
    
    // 计算原始大小
    result.original_size = trie_serialized.size() + dict_serialized.size() + metadata_serialized.size();
    
    // 压缩策略说明：
    // 1. Trie树：包含所有字段的编码值（字符串类型）和原始值（数值类型、布尔类型）
    // 2. 字典：只包含String、Timestamp、LogType字典，数值型和布尔型不需要字典
    // 3. 元数据：字段列表和类型映射信息
    
    // 压缩各个组件
    result.trie_data = compressWithZstd(trie_serialized);
    result.dictionary_data = compressWithZstd(dict_serialized);
    result.metadata_data = compressWithZstd(metadata_serialized);
    
    std::cout << "Debug - Compressed sizes:\n";
    std::cout << "  Trie: " << result.trie_data.size() << " bytes\n";
    std::cout << "  Dictionary: " << result.dictionary_data.size() << " bytes\n";
    std::cout << "  Metadata: " << result.metadata_data.size() << " bytes\n";
    
    // 计算压缩后大小
    result.compressed_size = result.trie_data.size() + result.dictionary_data.size() + result.metadata_data.size();
    
    return result;
}

// 解压缩并重建Trie树和字典
std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
Compressor::decompress(const CompressedData& compressed_data) {
    // 解压缩各个组件
    std::vector<uint8_t> trie_serialized = decompressWithZstd(compressed_data.trie_data);
    std::vector<uint8_t> dict_serialized = decompressWithZstd(compressed_data.dictionary_data);
    std::vector<uint8_t> metadata_serialized = decompressWithZstd(compressed_data.metadata_data);
    
    // 反序列化元数据
    auto [ordered_fields, field_types] = deserializeMetadata(metadata_serialized);
    
    // 重建Trie树
    auto trie = std::make_unique<Trie>(ordered_fields);
    trie->deserialize(trie_serialized);
    
    // 重建字典管理器
    auto manager = deserializeDictionary(dict_serialized);
    
    return {std::move(trie), std::move(manager)};
}

// 计算压缩率
double Compressor::getCompressionRatio(const CompressedData& compressed_data) {
    if (compressed_data.original_size == 0) return 0.0;
    return static_cast<double>(compressed_data.compressed_size) / compressed_data.original_size;
}

// 保存压缩数据到文件
bool Compressor::saveToFile(const CompressedData& compressed_data, const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;
    
    // 写入文件头
    const char* magic = "JSON2COMP";
    file.write(magic, 9);
    
    // 写入版本信息
    uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    // 写入各个组件的大小
    uint32_t trie_size = static_cast<uint32_t>(compressed_data.trie_data.size());
    uint32_t dict_size = static_cast<uint32_t>(compressed_data.dictionary_data.size());
    uint32_t metadata_size = static_cast<uint32_t>(compressed_data.metadata_data.size());
    
    file.write(reinterpret_cast<const char*>(&trie_size), sizeof(trie_size));
    file.write(reinterpret_cast<const char*>(&dict_size), sizeof(dict_size));
    file.write(reinterpret_cast<const char*>(&metadata_size), sizeof(metadata_size));
    
    // 写入各个组件的数据
    file.write(reinterpret_cast<const char*>(compressed_data.trie_data.data()), trie_size);
    file.write(reinterpret_cast<const char*>(compressed_data.dictionary_data.data()), dict_size);
    file.write(reinterpret_cast<const char*>(compressed_data.metadata_data.data()), metadata_size);
    
    return true;
}

// 从文件加载压缩数据
CompressedData Compressor::loadFromFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }
    
    CompressedData result;
    
    // 读取文件头
    char magic[9];
    file.read(magic, 9);
    if (std::string(magic, 9) != "JSON2COMP") {
        throw std::runtime_error("Invalid file format");
    }
    
    // 读取版本信息
    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) {
        throw std::runtime_error("Unsupported file version");
    }
    
    // 读取各个组件的大小
    uint32_t trie_size, dict_size, metadata_size;
    file.read(reinterpret_cast<char*>(&trie_size), sizeof(trie_size));
    file.read(reinterpret_cast<char*>(&dict_size), sizeof(dict_size));
    file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
    
    // 读取各个组件的数据
    result.trie_data.resize(trie_size);
    result.dictionary_data.resize(dict_size);
    result.metadata_data.resize(metadata_size);
    
    file.read(reinterpret_cast<char*>(result.trie_data.data()), trie_size);
    file.read(reinterpret_cast<char*>(result.dictionary_data.data()), dict_size);
    file.read(reinterpret_cast<char*>(result.metadata_data.data()), metadata_size);
    
    // 计算大小信息
    result.compressed_size = trie_size + dict_size + metadata_size;
    // 原始大小需要解压缩后才能计算，这里暂时设为0
    result.original_size = 0;
    
    return result;
}

} // namespace json2 