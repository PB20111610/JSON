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

#include "../include/field_key.h"

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

// ========== 新增：FieldKey序列化辅助函数 ==========
void writeFieldKeyVector(std::vector<uint8_t>& data, const std::vector<FieldKey>& vec) {
    uint32_t size = static_cast<uint32_t>(vec.size());
    writeValue(data, size);
    for (const auto& fk : vec) {
        writeString(data, fk.name);
        writeValue(data, static_cast<uint32_t>(fk.type));
    }
}

std::vector<FieldKey> readFieldKeyVector(const std::vector<uint8_t>& data, size_t& pos) {
    uint32_t size = readValue<uint32_t>(data, pos);
    std::vector<FieldKey> vec;
    vec.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        std::string name = readString(data, pos);
        FieldType type = static_cast<FieldType>(readValue<uint32_t>(data, pos));
        vec.push_back(FieldKey{name, type});
    }
    return vec;
}

// ========== 重构：序列化字典数据 ==========
std::vector<uint8_t> Compressor::serializeDictionary(const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    // 1. String字典（按FieldKey分组）
    std::vector<FieldKey> all_field_keys = manager.getAllFieldsAndTypes();
    uint32_t string_dict_count = 0;
    std::vector<std::pair<FieldKey, std::vector<std::string>>> string_dicts;
    const Dictionary& dict = manager.variableDict();
    for (size_t fk_idx = 0; fk_idx < all_field_keys.size(); ++fk_idx) {
        const auto& fk = all_field_keys[fk_idx];
        if (fk.type == FieldType::String) {
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
    writeValue(data, string_dict_count);
    for (size_t i = 0; i < string_dicts.size(); ++i) {
        const auto& [fk, values] = string_dicts[i];
        writeString(data, fk.name);
        writeValue(data, static_cast<uint32_t>(fk.type));
        writeVector(data, values);
    }
    // 2. Timestamp pattern列表
    const auto& ts_dict = manager.timestampDict();
    // pattern_id从1开始，直到getPatternById返回空
    std::vector<std::string> patterns;
    for (uint32_t i = 1;; ++i) {
        std::string pattern = ts_dict.getPatternById(i);
        if (pattern.empty()) break;
        patterns.push_back(pattern);
    }
    writeVector(data, patterns);
    // 3. LogType模板和变量字典
    const auto& log_dict = manager.logtypeDict();
    // 模板
    std::vector<std::string> log_templates;
    for (uint32_t i = 1; i <= log_dict.getLogTypeCount(); ++i) {
        log_templates.push_back(log_dict.getLogTypeById(i));
    }
    writeVector(data, log_templates);
    // 变量字典
    std::vector<std::string> log_vars;
    for (uint32_t i = 1; i < 100000; ++i) { // 假定变量数不会超过10万
        std::string var = log_dict.decodeVariable(i);
        if (var.empty()) break;
        log_vars.push_back(var);
    }
    writeVector(data, log_vars);
    return data;
}

// ========== 重构：反序列化字典数据 ==========
std::unique_ptr<FieldDictionaryManager> Compressor::deserializeDictionary(const std::vector<uint8_t>& data) {
    auto manager = std::make_unique<FieldDictionaryManager>();
    size_t pos = 0;
    // 1. String字典
    uint32_t string_dict_count = readValue<uint32_t>(data, pos);
    for (uint32_t i = 0; i < string_dict_count; ++i) {
        std::string name = readString(data, pos);
        FieldType type = static_cast<FieldType>(readValue<uint32_t>(data, pos));
        std::vector<std::string> values = readVector(data, pos);
        FieldKey fk{name, type};
        for (const auto& v : values) {
            manager->variableDict().addFieldValue(fk, type, v);
        }
    }
    // 2. Timestamp pattern列表
    std::vector<std::string> patterns = readVector(data, pos);
    auto& ts_dict = manager->timestampDict();
    for (const auto& pattern : patterns) {
        if (!pattern.empty()) {
            ts_dict.registerPattern(pattern);
        }
    }
    // 3. LogType模板
    std::vector<std::string> log_templates = readVector(data, pos);
    auto& log_dict = manager->logtypeDict();
    for (const auto& tmpl : log_templates) {
        log_dict.addLogType(tmpl);
    }
    // 变量字典
    std::vector<std::string> log_vars = readVector(data, pos);
    for (const auto& var : log_vars) {
        log_dict.encodeVariable(var);
    }
    return manager;
}

// ========== 重构：序列化元数据 ==========
std::vector<uint8_t> Compressor::serializeMetadata(const Trie& trie, const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    // 直接序列化FieldKey有序序列
    writeFieldKeyVector(data, trie.getOrderedFields());
    return data;
}

// ========== 重构：反序列化元数据 ==========
std::vector<FieldKey> Compressor::deserializeMetadata(const std::vector<uint8_t>& data) {
    size_t pos = 0;
    return readFieldKeyVector(data, pos);
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

// ========== 重构：压缩和解压接口 ==========
CompressedData Compressor::compress(const Trie& trie, const FieldDictionaryManager& manager) {
    CompressedData result;
    try {
        std::vector<uint8_t> trie_serialized = trie.serialize();
        std::vector<uint8_t> dict_serialized = serializeDictionary(manager);
        std::vector<uint8_t> metadata_serialized = serializeMetadata(trie, manager);
        result.original_size = trie_serialized.size() + dict_serialized.size() + metadata_serialized.size();
        result.trie_data = compressWithZstd(trie_serialized);
        result.dictionary_data = compressWithZstd(dict_serialized);
        result.metadata_data = compressWithZstd(metadata_serialized);
        result.compressed_size = result.trie_data.size() + result.dictionary_data.size() + result.metadata_data.size();
    } catch (const std::exception& e) {
        std::cerr << "[DEBUG] Exception in Compressor::compress: " << e.what() << std::endl;
        // 新增：打印trie根节点结构
        const TrieNode* root = trie.getRoot();
        std::cerr << "[DEBUG] Trie root path size: " << root->getPath().size() << ", children: " << root->getChildren().size() << std::endl;
        for (const auto& child_pair : root->getChildren()) {
            const TrieNode* child = child_pair.second.get();
            std::cerr << "[DEBUG]   Child " << child->getPath().size() << ", children=" << child->getChildren().size() << std::endl;
        }
        throw;
    }
    return result;
}

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
Compressor::decompress(const CompressedData& compressed_data) {
    std::vector<uint8_t> trie_serialized = decompressWithZstd(compressed_data.trie_data);
    std::vector<uint8_t> dict_serialized = decompressWithZstd(compressed_data.dictionary_data);
    std::vector<uint8_t> metadata_serialized = decompressWithZstd(compressed_data.metadata_data);
    std::cerr << "[DECOMPRESS] trie_serialized.size=" << trie_serialized.size() << ", dict_serialized.size=" << dict_serialized.size() << ", metadata_serialized.size=" << metadata_serialized.size() << std::endl;
    std::vector<FieldKey> ordered_field_keys = deserializeMetadata(metadata_serialized);
    std::cerr << "[DECOMPRESS] ordered_field_keys.size=" << ordered_field_keys.size() << std::endl;
    auto trie = std::make_unique<Trie>(ordered_field_keys);
    trie->deserialize(trie_serialized);
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