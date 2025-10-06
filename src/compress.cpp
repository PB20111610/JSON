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
        if (fk.type == FieldType::String || fk.type == FieldType::UnstructuredArray) {
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
    // 2. Timestamp模板和变量列表
    const auto& ts_dict = manager.timestampDict();
    // 序列化模板
    std::vector<std::string> templates;
    for (uint32_t i = 1; i <= ts_dict.getTemplateCount(); ++i) {
        std::string template_str = ts_dict.getTemplateById(i);
        if (!template_str.empty()) {
            templates.push_back(template_str);
        }
    }
    writeVector(data, templates);
    // 序列化变量
    std::vector<std::string> variables;
    for (uint32_t i = 1; i <= ts_dict.getVariableCount(); ++i) {
        std::string variable = ts_dict.getVariableByCode(i);
        if (!variable.empty()) {
            variables.push_back(variable);
        }
    }
    writeVector(data, variables);
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

// ========== 新的分离字典序列化方法 ==========

std::vector<uint8_t> Compressor::serializeStringDictionary(const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    
    // 序列化String字典（按FieldKey分组）
    std::vector<FieldKey> all_field_keys = manager.getAllFieldsAndTypes();
    uint32_t string_dict_count = 0;
    std::vector<std::pair<FieldKey, std::vector<std::string>>> string_dicts;
    
    const Dictionary& dict = manager.variableDict();
    for (size_t fk_idx = 0; fk_idx < all_field_keys.size(); ++fk_idx) {
        const auto& fk = all_field_keys[fk_idx];
        if (fk.type == FieldType::String || fk.type == FieldType::UnstructuredArray) {
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
    
    return data;
}

std::vector<uint8_t> Compressor::serializeTimestampDictionary(const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    
    // 序列化Timestamp模板和变量列表
    const auto& ts_dict = manager.timestampDict();
    
    // 序列化模板
    std::vector<std::string> templates;
    for (uint32_t i = 1; i <= ts_dict.getTemplateCount(); ++i) {
        std::string template_str = ts_dict.getTemplateById(i);
        if (!template_str.empty()) {
            templates.push_back(template_str);
        }
    }
    writeVector(data, templates);
    
    // 序列化变量
    std::vector<std::string> variables;
    for (uint32_t i = 1; i <= ts_dict.getVariableCount(); ++i) {
        std::string variable = ts_dict.getVariableByCode(i);
        if (!variable.empty()) {
            variables.push_back(variable);
        }
    }
    writeVector(data, variables);
    
    return data;
}

std::vector<uint8_t> Compressor::serializeLogTypeDictionary(const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    
    // 序列化LogType模板和变量字典
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
    // 2. Timestamp模板和变量列表
    std::vector<std::string> templates = readVector(data, pos);
    std::vector<std::string> variables = readVector(data, pos);
    auto& ts_dict = manager->timestampDict();
    // 预注册模板
    for (const auto& template_str : templates) {
        if (!template_str.empty()) {
            ts_dict.registerTemplate(template_str);
        }
    }
    // 预注册变量
    for (const auto& variable : variables) {
        if (!variable.empty()) {
            ts_dict.registerVariable(variable);
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

// ========== 新的分离字典反序列化方法 ==========

void Compressor::deserializeStringDictionary(const std::vector<uint8_t>& data, FieldDictionaryManager& manager) {
    size_t pos = 0;
    
    // 反序列化String字典
    uint32_t string_dict_count = readValue<uint32_t>(data, pos);
    for (uint32_t i = 0; i < string_dict_count; ++i) {
        std::string name = readString(data, pos);
        FieldType type = static_cast<FieldType>(readValue<uint32_t>(data, pos));
        std::vector<std::string> values = readVector(data, pos);
        FieldKey fk{name, type};
        for (const auto& v : values) {
            manager.variableDict().addFieldValue(fk, type, v);
        }
    }
}

void Compressor::deserializeTimestampDictionary(const std::vector<uint8_t>& data, FieldDictionaryManager& manager) {
    size_t pos = 0;
    
    // 反序列化Timestamp模板和变量列表
    std::vector<std::string> templates = readVector(data, pos);
    std::vector<std::string> variables = readVector(data, pos);
    
    auto& ts_dict = manager.timestampDict();
    
    // 预注册模板
    for (const auto& template_str : templates) {
        if (!template_str.empty()) {
            ts_dict.registerTemplate(template_str);
        }
    }
    
    // 预注册变量
    for (const auto& variable : variables) {
        if (!variable.empty()) {
            ts_dict.registerVariable(variable);
        }
    }
}

void Compressor::deserializeLogTypeDictionary(const std::vector<uint8_t>& data, FieldDictionaryManager& manager) {
    size_t pos = 0;
    
    // 反序列化LogType模板和变量字典
    std::vector<std::string> log_templates = readVector(data, pos);
    std::vector<std::string> log_vars = readVector(data, pos);
    
    auto& log_dict = manager.logtypeDict();
    
    // 预注册模板
    for (const auto& template_str : log_templates) {
        if (!template_str.empty()) {
            log_dict.addLogType(template_str);
        }
    }
    
    // 预注册变量
    for (const auto& var : log_vars) {
        if (!var.empty()) {
            log_dict.encodeVariable(var);
        }
    }
}

// ========== 重构：序列化元数据 ==========
std::vector<uint8_t> Compressor::serializeMetadata(const std::vector<FieldKey>& fields, const FieldDictionaryManager& manager) {
    std::vector<uint8_t> data;
    writeFieldKeyVector(data, fields);
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
    // 1. 构建 LOUDS Trie
    LOUDSTrie louds(trie.getOrderedFields());
    louds.buildFromTrie(trie);
    
    // Use dynamically expanded field order from LOUDS
    auto expanded_field_order = louds.getFieldOrder();
    std::cout << "[DEBUG] Compressor::compress - field order sizes: initial="
              << trie.getOrderedFields().size() << ", expanded=" << expanded_field_order.size() << std::endl;
    
    // 2. 用 LOUDS Trie 进行压缩，使用扩展后的字段顺序
    return compressLouds(louds, manager, expanded_field_order);
}

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>>
Compressor::decompress(const CompressedData& compressed_data) {
    // 1. 先解压 LOUDS Trie 和字典
    auto [louds, manager] = decompressLouds(compressed_data);
    // 2. LOUDS Trie 转 Trie
    auto trie = std::make_unique<Trie>(louds->getFieldOrder());
    loudsToTrie(*louds, *trie);
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

// 保存压缩数据到内存
std::vector<uint8_t> Compressor::saveToMemory(const CompressedData& compressed_data) {
    std::vector<uint8_t> buffer;
    // 写入文件头
    const char* magic = "JSON2COMP";
    buffer.insert(buffer.end(), magic, magic + 9);
    // 写入版本信息
    uint32_t version = 1;
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&version), reinterpret_cast<const uint8_t*>(&version) + sizeof(version));
    // 写入各个组件的大小
    uint32_t trie_size = static_cast<uint32_t>(compressed_data.trie_data.size());
    uint32_t layer_size = static_cast<uint32_t>(compressed_data.layer_data.size());
    uint32_t dict_size = static_cast<uint32_t>(compressed_data.dictionary_data.size());
    uint32_t metadata_size = static_cast<uint32_t>(compressed_data.metadata_data.size());
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&trie_size), reinterpret_cast<const uint8_t*>(&trie_size) + sizeof(trie_size));
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&layer_size), reinterpret_cast<const uint8_t*>(&layer_size) + sizeof(layer_size));
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&dict_size), reinterpret_cast<const uint8_t*>(&dict_size) + sizeof(dict_size));
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t*>(&metadata_size), reinterpret_cast<const uint8_t*>(&metadata_size) + sizeof(metadata_size));
    // 写入各个组件的数据
    buffer.insert(buffer.end(), compressed_data.trie_data.begin(), compressed_data.trie_data.end());
    buffer.insert(buffer.end(), compressed_data.layer_data.begin(), compressed_data.layer_data.end());
    buffer.insert(buffer.end(), compressed_data.dictionary_data.begin(), compressed_data.dictionary_data.end());
    buffer.insert(buffer.end(), compressed_data.metadata_data.begin(), compressed_data.metadata_data.end());
    return buffer;
}

// 从内存加载压缩数据
CompressedData Compressor::loadFromMemory(const std::vector<uint8_t>& buffer) {
    CompressedData result;
    size_t offset = 0;
    // 读取文件头
    if (buffer.size() < 9) throw std::runtime_error("Buffer too small for header");
    std::string magic(reinterpret_cast<const char*>(&buffer[offset]), 9);
    if (magic != "JSON2COMP") throw std::runtime_error("Invalid buffer format");
    offset += 9;
    // 读取版本信息
    if (buffer.size() < offset + sizeof(uint32_t)) throw std::runtime_error("Buffer too small for version");
    uint32_t version;
    std::memcpy(&version, &buffer[offset], sizeof(version));
    if (version != 1) throw std::runtime_error("Unsupported buffer version");
    offset += sizeof(uint32_t);
    // 读取各个组件的大小
    if (buffer.size() < offset + 4 * sizeof(uint32_t)) throw std::runtime_error("Buffer too small for sizes");
    uint32_t trie_size, layer_size, dict_size, metadata_size;
    std::memcpy(&trie_size, &buffer[offset], sizeof(trie_size)); offset += sizeof(trie_size);
    std::memcpy(&layer_size, &buffer[offset], sizeof(layer_size)); offset += sizeof(layer_size);
    std::memcpy(&dict_size, &buffer[offset], sizeof(dict_size)); offset += sizeof(dict_size);
    std::memcpy(&metadata_size, &buffer[offset], sizeof(metadata_size)); offset += sizeof(metadata_size);
    // 读取各个组件的数据
    if (buffer.size() < offset + trie_size + layer_size + dict_size + metadata_size)
        throw std::runtime_error("Buffer too small for data");
    result.trie_data.assign(buffer.begin() + offset, buffer.begin() + offset + trie_size); offset += trie_size;
    result.layer_data.assign(buffer.begin() + offset, buffer.begin() + offset + layer_size); offset += layer_size;
    result.dictionary_data.assign(buffer.begin() + offset, buffer.begin() + offset + dict_size); offset += dict_size;
    result.metadata_data.assign(buffer.begin() + offset, buffer.begin() + offset + metadata_size); offset += metadata_size;
    result.compressed_size = trie_size + layer_size + dict_size + metadata_size;
    result.original_size = 0;
    return result;
}

CompressedData Compressor::compressLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, const std::vector<FieldKey>& field_order) {
    CompressedData result;
    // 1. LOUDS位图整体压缩
    std::ostringstream bv_stream(std::ios::binary);
    louds.serializeBitmap(bv_stream);
    std::string bv_str = bv_stream.str();
    result.trie_data = compressWithZstd(std::vector<uint8_t>(bv_str.begin(), bv_str.end()));

    // 2. 层内容分层单独压缩
    size_t layer_count = louds.getLayeredStorage().getLayerCount();
    std::vector<std::vector<uint8_t>> compressed_layers;
    std::vector<uint32_t> layer_sizes;
    size_t layers_raw_total = 0;
    for (size_t i = 0; i < layer_count; ++i) {
        std::ostringstream layer_stream(std::ios::binary);
        louds.getLayeredStorage().serializeLayer(i, layer_stream);
        std::string layer_str = layer_stream.str();
        layers_raw_total += layer_str.size();
        auto compressed = compressWithZstd(std::vector<uint8_t>(layer_str.begin(), layer_str.end()));
        layer_sizes.push_back(static_cast<uint32_t>(compressed.size()));
        compressed_layers.push_back(std::move(compressed));
    }
    // 存储格式：[层数][每层长度][每层数据]...
    std::vector<uint8_t> layer_data;
    uint32_t layer_count_u32 = static_cast<uint32_t>(layer_count);
    layer_data.insert(layer_data.end(), reinterpret_cast<uint8_t*>(&layer_count_u32), reinterpret_cast<uint8_t*>(&layer_count_u32) + sizeof(layer_count_u32));
    for (uint32_t sz : layer_sizes) {
        layer_data.insert(layer_data.end(), reinterpret_cast<uint8_t*>(&sz), reinterpret_cast<uint8_t*>(&sz) + sizeof(sz));
    }
    for (const auto& block : compressed_layers) {
        layer_data.insert(layer_data.end(), block.begin(), block.end());
    }
    result.layer_data = std::move(layer_data);

    // 3. 字典内容分块压缩（string/timestamp/logtype变量/模板分开）
    // 3.1 String字典
    std::vector<uint8_t> string_dict_raw;
    {
        std::vector<FieldKey> all_field_keys = manager.getAllFieldsAndTypes();
        uint32_t string_dict_count = 0;
        std::vector<std::pair<FieldKey, std::vector<std::string>>> string_dicts;
        const Dictionary& dict = manager.variableDict();
        for (size_t fk_idx = 0; fk_idx < all_field_keys.size(); ++fk_idx) {
            const auto& fk = all_field_keys[fk_idx];
            if (fk.type == FieldType::String || fk.type == FieldType::UnstructuredArray) {
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
        writeValue(string_dict_raw, string_dict_count);
        for (size_t i = 0; i < string_dicts.size(); ++i) {
            const auto& [fk, values] = string_dicts[i];
            writeString(string_dict_raw, fk.name);
            writeValue(string_dict_raw, static_cast<uint32_t>(fk.type));
            writeVector(string_dict_raw, values);
        }
    }
    auto compressed_string_dict = compressWithZstd(string_dict_raw);

    // 3.2 Timestamp模板和变量
    std::vector<uint8_t> ts_dict_raw;
    {
        const auto& ts_dict = manager.timestampDict();
        // 序列化模板
        std::vector<std::string> templates;
        for (uint32_t i = 1; i <= ts_dict.getTemplateCount(); ++i) {
            std::string template_str = ts_dict.getTemplateById(i);
            if (!template_str.empty()) {
                templates.push_back(template_str);
            }
        }
        writeVector(ts_dict_raw, templates);
        // 序列化变量
        std::vector<std::string> variables;
        for (uint32_t i = 1; i <= ts_dict.getVariableCount(); ++i) {
            std::string variable = ts_dict.getVariableByCode(i);
            if (!variable.empty()) {
                variables.push_back(variable);
            }
        }
        writeVector(ts_dict_raw, variables);
    }
    auto compressed_ts_dict = compressWithZstd(ts_dict_raw);

    // 3.3 LogType模板
    std::vector<uint8_t> log_templates_raw;
    {
        const auto& log_dict = manager.logtypeDict();
        std::vector<std::string> log_templates;
        for (uint32_t i = 1; i <= log_dict.getLogTypeCount(); ++i) {
            log_templates.push_back(log_dict.getLogTypeById(i));
        }
        writeVector(log_templates_raw, log_templates);
    }
    auto compressed_log_templates = compressWithZstd(log_templates_raw);

    // 3.4 LogType变量字典
    std::vector<uint8_t> log_vars_raw;
    {
        const auto& log_dict = manager.logtypeDict();
        std::vector<std::string> log_vars;
        for (uint32_t i = 1; i < 100000; ++i) {
            std::string var = log_dict.decodeVariable(i);
            if (var.empty()) break;
            log_vars.push_back(var);
        }
        writeVector(log_vars_raw, log_vars);
    }
    auto compressed_log_vars = compressWithZstd(log_vars_raw);

    // 字典内容格式：[4块长度][4块数据]
    std::vector<uint8_t> dict_data;
    uint32_t dict_block_sizes[4] = {
        static_cast<uint32_t>(compressed_string_dict.size()),
        static_cast<uint32_t>(compressed_ts_dict.size()),
        static_cast<uint32_t>(compressed_log_templates.size()),
        static_cast<uint32_t>(compressed_log_vars.size())
    };
    for (int i = 0; i < 4; ++i) {
        dict_data.insert(dict_data.end(), reinterpret_cast<uint8_t*>(&dict_block_sizes[i]), reinterpret_cast<uint8_t*>(&dict_block_sizes[i]) + sizeof(uint32_t));
    }
    dict_data.insert(dict_data.end(), compressed_string_dict.begin(), compressed_string_dict.end());
    dict_data.insert(dict_data.end(), compressed_ts_dict.begin(), compressed_ts_dict.end());
    dict_data.insert(dict_data.end(), compressed_log_templates.begin(), compressed_log_templates.end());
    dict_data.insert(dict_data.end(), compressed_log_vars.begin(), compressed_log_vars.end());
    result.dictionary_data = std::move(dict_data);

    // 4. 元数据单独压缩
    std::vector<uint8_t> meta_raw = serializeMetadata(field_order, manager);
    auto compressed_meta = compressWithZstd(meta_raw);
    std::vector<uint8_t> meta_data;
    uint32_t meta_size = static_cast<uint32_t>(compressed_meta.size());
    meta_data.insert(meta_data.end(), reinterpret_cast<uint8_t*>(&meta_size), reinterpret_cast<uint8_t*>(&meta_size) + sizeof(meta_size));
    meta_data.insert(meta_data.end(), compressed_meta.begin(), compressed_meta.end());
    result.metadata_data = std::move(meta_data);

    // 统计原始大小
    result.original_size = bv_str.size() + layers_raw_total + string_dict_raw.size() + ts_dict_raw.size() + log_templates_raw.size() + log_vars_raw.size() + meta_raw.size();
    // 统计压缩后大小
    result.compressed_size = result.trie_data.size() + result.layer_data.size() + result.dictionary_data.size() + result.metadata_data.size();
    return result;
}

std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> Compressor::decompressLouds(const CompressedData& compressed_data) {
    // 1. 解压LOUDS位图
    std::vector<uint8_t> trie_data = decompressWithZstd(compressed_data.trie_data);
    std::istringstream bv_stream(std::string(trie_data.begin(), trie_data.end()), std::ios::binary);
    auto louds = std::make_unique<LOUDSTrie>();
    louds->deserializeBitmap(bv_stream);

    // 2. 解压分层内容
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
    // 先初始化分层存储结构
    for (uint32_t i = 0; i < layer_count; ++i) {
        if (i >= louds->getLayeredStorage().getLayerCount())
            louds->getLayeredStorage().addLayer(louds->getFieldOrder()[i], 0);
    }
    for (uint32_t i = 0; i < layer_count; ++i) {
        std::vector<uint8_t> compressed_layer(layer_data.begin() + offset, layer_data.begin() + offset + layer_sizes[i]);
        offset += layer_sizes[i];
        std::vector<uint8_t> layer_raw = decompressWithZstd(compressed_layer);
        std::istringstream layer_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
        louds->getLayeredStorage().deserializeLayer(i, layer_stream);
    }

    // 3. 解压字典内容
    const std::vector<uint8_t>& dict_data = compressed_data.dictionary_data;
    size_t dict_offset = 0;
    uint32_t dict_block_sizes[4];
    for (int i = 0; i < 4; ++i) {
        std::memcpy(&dict_block_sizes[i], &dict_data[dict_offset], sizeof(uint32_t));
        dict_offset += sizeof(uint32_t);
    }
    // 3.1 String字典
    std::vector<uint8_t> compressed_string_dict(dict_data.begin() + dict_offset, dict_data.begin() + dict_offset + dict_block_sizes[0]);
    dict_offset += dict_block_sizes[0];
    std::vector<uint8_t> string_dict_raw = decompressWithZstd(compressed_string_dict);
    // 3.2 Timestamp pattern
    std::vector<uint8_t> compressed_ts_dict(dict_data.begin() + dict_offset, dict_data.begin() + dict_offset + dict_block_sizes[1]);
    dict_offset += dict_block_sizes[1];
    std::vector<uint8_t> ts_dict_raw = decompressWithZstd(compressed_ts_dict);
    // 3.3 LogType模板
    std::vector<uint8_t> compressed_log_templates(dict_data.begin() + dict_offset, dict_data.begin() + dict_offset + dict_block_sizes[2]);
    dict_offset += dict_block_sizes[2];
    std::vector<uint8_t> log_templates_raw = decompressWithZstd(compressed_log_templates);
    // 3.4 LogType变量字典
    std::vector<uint8_t> compressed_log_vars(dict_data.begin() + dict_offset, dict_data.begin() + dict_offset + dict_block_sizes[3]);
    dict_offset += dict_block_sizes[3];
    std::vector<uint8_t> log_vars_raw = decompressWithZstd(compressed_log_vars);

    // 反序列化字典
    auto manager = std::make_unique<FieldDictionaryManager>();
    size_t pos = 0;
    // 1. String字典
    uint32_t string_dict_count = readValue<uint32_t>(string_dict_raw, pos);
    for (uint32_t i = 0; i < string_dict_count; ++i) {
        std::string name = readString(string_dict_raw, pos);
        FieldType type = static_cast<FieldType>(readValue<uint32_t>(string_dict_raw, pos));
        std::vector<std::string> values = readVector(string_dict_raw, pos);
        FieldKey fk{name, type};
        for (const auto& v : values) {
            manager->variableDict().addFieldValue(fk, type, v);
        }
    }
    // 2. Timestamp模板和变量列表
    pos = 0;
    std::vector<std::string> templates = readVector(ts_dict_raw, pos);
    std::vector<std::string> variables = readVector(ts_dict_raw, pos);
    auto& ts_dict = manager->timestampDict();
    // 预注册模板
    for (const auto& template_str : templates) {
        if (!template_str.empty()) {
            ts_dict.registerTemplate(template_str);
        }
    }
    // 预注册变量
    for (const auto& variable : variables) {
        if (!variable.empty()) {
            ts_dict.registerVariable(variable);
        }
    }
    // 3. LogType模板
    pos = 0;
    std::vector<std::string> log_templates = readVector(log_templates_raw, pos);
    auto& log_dict = manager->logtypeDict();
    for (const auto& tmpl : log_templates) {
        log_dict.addLogType(tmpl);
    }
    // 4. 变量字典
    pos = 0;
    std::vector<std::string> log_vars = readVector(log_vars_raw, pos);
    for (const auto& var : log_vars) {
        log_dict.encodeVariable(var);
    }

    // 4. 解压元数据
    const std::vector<uint8_t>& meta_data = compressed_data.metadata_data;
    size_t meta_offset = 0;
    uint32_t meta_size = 0;
    std::memcpy(&meta_size, &meta_data[meta_offset], sizeof(meta_size));
    meta_offset += sizeof(meta_size);
    std::vector<uint8_t> compressed_meta(meta_data.begin() + meta_offset, meta_data.begin() + meta_offset + meta_size);
    std::vector<uint8_t> meta_raw = decompressWithZstd(compressed_meta);
    std::vector<FieldKey> ordered_field_keys = deserializeMetadata(meta_raw);
    louds->setFieldOrder(ordered_field_keys);

    return {std::move(louds), std::move(manager)};
}

// ========== LOUDS Trie Serialization Methods ==========

std::vector<uint8_t> Compressor::serializeLoudsTrie(const LOUDSTrie& louds) {
    std::vector<uint8_t> result;
    
    // LOUDSTrie 就是 01 位串，直接序列化位向量
    std::ostringstream bv_stream(std::ios::binary);
    louds.serializeBitmap(bv_stream);  // 使用高效的 SDSL 序列化
    std::string bv_str = bv_stream.str();
    
    // 直接返回位向量数据，不需要额外的封装
    result.assign(bv_str.begin(), bv_str.end());
    
    return result;
}

std::unique_ptr<LOUDSTrie> Compressor::deserializeLoudsTrie(const std::vector<uint8_t>& data, 
                                                           const FieldDictionaryManager& dict_mgr, 
                                                           const std::vector<FieldKey>& field_order) {
    // LOUDSTrie 就是 01 位串，直接反序列化位向量
    std::string bv_str(data.begin(), data.end());
    std::istringstream bv_stream(bv_str, std::ios::binary);
    
    auto louds = std::make_unique<LOUDSTrie>(field_order);
    louds->deserializeBitmap(bv_stream);  // 使用高效的 SDSL 反序列化
    
    return louds;
}

// ========== 细粒度压缩实现 ==========

GranularCompressedData Compressor::compressGranular(const Trie& trie, const FieldDictionaryManager& manager, bool use_layer_separation) {
    GranularCompressedData result;
    result.use_layer_separation = use_layer_separation;
    
    // 1. 分别序列化各类字典
    std::vector<uint8_t> string_dict_raw = serializeStringDictionary(manager);
    std::vector<uint8_t> timestamp_dict_raw = serializeTimestampDictionary(manager);
    std::vector<uint8_t> logtype_dict_raw = serializeLogTypeDictionary(manager);
    
    // 2. 序列化Trie位图
    LOUDSTrie louds(trie.getOrderedFields());
    louds.buildFromTrie(trie);
    std::ostringstream bv_stream(std::ios::binary);
    louds.serializeBitmap(bv_stream);
    std::string bv_str = bv_stream.str();
    std::vector<uint8_t> trie_raw(bv_str.begin(), bv_str.end());
    
    // 3. 序列化分层内容
    std::vector<uint8_t> layer_raw;
    if (use_layer_separation) {
        // 按层分别序列化和压缩
        size_t layer_count = louds.getLayeredStorage().getLayerCount();
        result.layer_data_by_level.reserve(layer_count);
        
        for (size_t i = 0; i < layer_count; ++i) {
            std::ostringstream layer_stream(std::ios::binary);
            louds.getLayeredStorage().serializeLayer(i, layer_stream);
            std::string layer_str = layer_stream.str();
            std::vector<uint8_t> layer_data(layer_str.begin(), layer_str.end());
            std::vector<uint8_t> compressed_layer = compressWithZstd(layer_data);
            result.layer_data_by_level.push_back(std::move(compressed_layer));
            
            // 累计原始大小
            layer_raw.insert(layer_raw.end(), layer_data.begin(), layer_data.end());
        }
    } else {
        // 整体序列化和压缩
        // Serialize all layers individually and combine
        std::ostringstream all_layers_stream(std::ios::binary);
        louds.getLayeredStorage().serialize(all_layers_stream);
        std::string all_layers_str = all_layers_stream.str();
        layer_raw.assign(all_layers_str.begin(), all_layers_str.end());
        result.layer_data_combined = compressWithZstd(layer_raw);
    }
    
    // 4. 序列化元数据
    std::vector<uint8_t> metadata_raw = serializeMetadata(trie.getOrderedFields(), manager);
    
    // 5. 分别压缩各组件
    result.trie_bitmap = compressWithZstd(trie_raw);
    result.string_dict = compressWithZstd(string_dict_raw);
    result.timestamp_dict = compressWithZstd(timestamp_dict_raw);
    result.logtype_dict = compressWithZstd(logtype_dict_raw);
    result.metadata = compressWithZstd(metadata_raw);
    
    // 6. 记录原始大小
    result.trie_original_size = trie_raw.size();
    result.string_dict_original_size = string_dict_raw.size();
    result.timestamp_dict_original_size = timestamp_dict_raw.size();
    result.logtype_dict_original_size = logtype_dict_raw.size();
    result.layer_original_size = layer_raw.size();
    result.metadata_original_size = metadata_raw.size();
    
    result.original_size = result.trie_original_size + result.string_dict_original_size + 
                          result.timestamp_dict_original_size + result.logtype_dict_original_size + 
                          result.layer_original_size + result.metadata_original_size;
    
    // 7. 计算压缩后总大小
    result.compressed_size = result.trie_bitmap.size() + result.string_dict.size() + 
                            result.timestamp_dict.size() + result.logtype_dict.size() + 
                            result.metadata.size();
    
    if (use_layer_separation) {
        for (const auto& layer : result.layer_data_by_level) {
            result.compressed_size += layer.size();
        }
    } else {
        result.compressed_size += result.layer_data_combined.size();
    }
    
    return result;
}

GranularCompressedData Compressor::compressGranularLouds(const LOUDSTrie& louds, const FieldDictionaryManager& manager, const std::vector<FieldKey>& field_order, bool use_layer_separation) {
    GranularCompressedData result;
    result.use_layer_separation = use_layer_separation;
    
    // 1. 分别序列化各类字典
    std::vector<uint8_t> string_dict_raw = serializeStringDictionary(manager);
    std::vector<uint8_t> timestamp_dict_raw = serializeTimestampDictionary(manager);
    std::vector<uint8_t> logtype_dict_raw = serializeLogTypeDictionary(manager);
    
    // 2. 序列化LOUDS Trie位图
    std::vector<uint8_t> trie_raw = serializeLoudsTrie(louds);
    
    // 3. 序列化分层内容
    std::vector<uint8_t> layer_raw;
    if (use_layer_separation) {
        // 按层分别序列化和压缩
        size_t layer_count = louds.getLayeredStorage().getLayerCount();
        result.layer_data_by_level.reserve(layer_count);
        
        for (size_t i = 0; i < layer_count; ++i) {
            std::ostringstream layer_stream(std::ios::binary);
            louds.getLayeredStorage().serializeLayer(i, layer_stream);
            std::string layer_str = layer_stream.str();
            std::vector<uint8_t> layer_data(layer_str.begin(), layer_str.end());
            std::vector<uint8_t> compressed_layer = compressWithZstd(layer_data);
            result.layer_data_by_level.push_back(std::move(compressed_layer));
            
            // 累计原始大小
            layer_raw.insert(layer_raw.end(), layer_data.begin(), layer_data.end());
        }
    } else {
        // 整体序列化和压缩
        // Serialize all layers individually and combine
        std::ostringstream all_layers_stream(std::ios::binary);
        louds.getLayeredStorage().serialize(all_layers_stream);
        std::string all_layers_str = all_layers_stream.str();
        layer_raw.assign(all_layers_str.begin(), all_layers_str.end());
        result.layer_data_combined = compressWithZstd(layer_raw);
    }
    
    // 4. 序列化元数据
    std::vector<uint8_t> metadata_raw = serializeMetadata(field_order, manager);
    
    // 5. 分别压缩各组件
    result.trie_bitmap = compressWithZstd(trie_raw);
    result.string_dict = compressWithZstd(string_dict_raw);
    result.timestamp_dict = compressWithZstd(timestamp_dict_raw);
    result.logtype_dict = compressWithZstd(logtype_dict_raw);
    result.metadata = compressWithZstd(metadata_raw);
    
    // 6. 记录原始大小
    result.trie_original_size = trie_raw.size();
    result.string_dict_original_size = string_dict_raw.size();
    result.timestamp_dict_original_size = timestamp_dict_raw.size();
    result.logtype_dict_original_size = logtype_dict_raw.size();
    result.layer_original_size = layer_raw.size();
    result.metadata_original_size = metadata_raw.size();
    
    result.original_size = result.trie_original_size + result.string_dict_original_size + 
                          result.timestamp_dict_original_size + result.logtype_dict_original_size + 
                          result.layer_original_size + result.metadata_original_size;
    
    // 7. 计算压缩后总大小
    result.compressed_size = result.trie_bitmap.size() + result.string_dict.size() + 
                            result.timestamp_dict.size() + result.logtype_dict.size() + 
                            result.metadata.size();
    
    if (use_layer_separation) {
        for (const auto& layer : result.layer_data_by_level) {
            result.compressed_size += layer.size();
        }
    } else {
        result.compressed_size += result.layer_data_combined.size();
    }
    
    return result;
}

double Compressor::getGranularCompressionRatio(const GranularCompressedData& compressed_data) {
    if (compressed_data.compressed_size == 0) return 0.0;
    return static_cast<double>(compressed_data.original_size) / compressed_data.compressed_size;
}

// ========== 细粒度解压缩实现 ==========

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
Compressor::decompressGranular(const GranularCompressedData& compressed_data) {
    // 1. 解压缩元数据
    std::vector<uint8_t> metadata_raw = decompressWithZstd(compressed_data.metadata);
    std::vector<FieldKey> field_order = deserializeMetadata(metadata_raw);
    
    // 2. 重建字典管理器
    auto manager = std::make_unique<FieldDictionaryManager>();
    
    // 分别解压缩和反序列化各类字典
    if (!compressed_data.string_dict.empty()) {
        std::vector<uint8_t> string_dict_raw = decompressWithZstd(compressed_data.string_dict);
        deserializeStringDictionary(string_dict_raw, *manager);
    }
    
    if (!compressed_data.timestamp_dict.empty()) {
        std::vector<uint8_t> timestamp_dict_raw = decompressWithZstd(compressed_data.timestamp_dict);
        deserializeTimestampDictionary(timestamp_dict_raw, *manager);
    }
    
    if (!compressed_data.logtype_dict.empty()) {
        std::vector<uint8_t> logtype_dict_raw = decompressWithZstd(compressed_data.logtype_dict);
        deserializeLogTypeDictionary(logtype_dict_raw, *manager);
    }
    
    // 3. 重建LOUDS Trie结构，然后转换为Trie
    std::vector<uint8_t> trie_raw = decompressWithZstd(compressed_data.trie_bitmap);
    auto louds = deserializeLoudsTrie(trie_raw, *manager, field_order);
    
    // 4. 重建分层内容
    if (compressed_data.use_layer_separation) {
        // 确保layers_向量有足够的空间来容纳所有层
        auto& layered_storage = louds->getLayeredStorage();
        size_t required_layers = compressed_data.layer_data_by_level.size();
        while (layered_storage.getLayerCount() < required_layers) {
            layered_storage.addLayer(FieldKey{}, 0);
        }
        
        // 按层分别解压缩
        for (size_t i = 0; i < compressed_data.layer_data_by_level.size(); ++i) {
            std::vector<uint8_t> layer_data = decompressWithZstd(compressed_data.layer_data_by_level[i]);
            std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
            layered_storage.deserializeLayer(i, layer_stream);
        }
    } else {
        // 整体解压缩
        std::vector<uint8_t> layer_raw = decompressWithZstd(compressed_data.layer_data_combined);
        std::istringstream all_layers_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
        louds->getLayeredStorage().deserialize(all_layers_stream);
    }
    
    // 5. 将LOUDS Trie转换为普通Trie
    auto trie = std::make_unique<Trie>(field_order);
    loudsToTrie(*louds, *trie);
    
    return std::make_pair(std::move(trie), std::move(manager));
}

std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
Compressor::decompressGranularLouds(const GranularCompressedData& compressed_data) {
    // 1. 解压缩元数据
    std::vector<uint8_t> metadata_raw = decompressWithZstd(compressed_data.metadata);
    std::vector<FieldKey> field_order = deserializeMetadata(metadata_raw);
    
    // 2. 重建字典管理器
    auto manager = std::make_unique<FieldDictionaryManager>();
    
    // 分别解压缩和反序列化各类字典
    if (!compressed_data.string_dict.empty()) {
        std::vector<uint8_t> string_dict_raw = decompressWithZstd(compressed_data.string_dict);
        deserializeStringDictionary(string_dict_raw, *manager);
    }
    
    if (!compressed_data.timestamp_dict.empty()) {
        std::vector<uint8_t> timestamp_dict_raw = decompressWithZstd(compressed_data.timestamp_dict);
        deserializeTimestampDictionary(timestamp_dict_raw, *manager);
    }
    
    if (!compressed_data.logtype_dict.empty()) {
        std::vector<uint8_t> logtype_dict_raw = decompressWithZstd(compressed_data.logtype_dict);
        deserializeLogTypeDictionary(logtype_dict_raw, *manager);
    }
    
    // 3. 重建LOUDS Trie结构
    std::vector<uint8_t> trie_raw = decompressWithZstd(compressed_data.trie_bitmap);
    auto louds = deserializeLoudsTrie(trie_raw, *manager, field_order);
    
    // 4. 重建分层内容
    if (compressed_data.use_layer_separation) {
        // 确保layers_向量有足够的空间来容纳所有层
        auto& layered_storage = louds->getLayeredStorage();
        size_t required_layers = compressed_data.layer_data_by_level.size();
        while (layered_storage.getLayerCount() < required_layers) {
            layered_storage.addLayer(FieldKey{}, 0);
        }
        
        // 按层分别解压缩
        for (size_t i = 0; i < compressed_data.layer_data_by_level.size(); ++i) {
            std::vector<uint8_t> layer_data = decompressWithZstd(compressed_data.layer_data_by_level[i]);
            std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
            layered_storage.deserializeLayer(i, layer_stream);
        }
    } else {
        // 整体解压缩
        std::vector<uint8_t> layer_raw = decompressWithZstd(compressed_data.layer_data_combined);
        std::istringstream all_layers_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
        louds->getLayeredStorage().deserialize(all_layers_stream);
    }
    
    return std::make_pair(std::move(louds), std::move(manager));
}

// ========== 部分解压缩实现 ==========

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
Compressor::decompressGranularPartial(const GranularCompressedData& compressed_data, const PartialDecompressionOptions& options) {
    std::unique_ptr<Trie> trie = nullptr;
    auto manager = std::make_unique<FieldDictionaryManager>();
    
    // 1. 解压缩元数据（通常需要）
    std::vector<FieldKey> field_order;
    if (options.load_metadata) {
        std::vector<uint8_t> metadata_raw = decompressWithZstd(compressed_data.metadata);
        field_order = deserializeMetadata(metadata_raw);
    }
    
    // 2. 按需解压缩字典
    if (options.load_string_dict && !compressed_data.string_dict.empty()) {
        std::vector<uint8_t> string_dict_raw = decompressWithZstd(compressed_data.string_dict);
        deserializeStringDictionary(string_dict_raw, *manager);
    }
    
    if (options.load_timestamp_dict && !compressed_data.timestamp_dict.empty()) {
        std::vector<uint8_t> timestamp_dict_raw = decompressWithZstd(compressed_data.timestamp_dict);
        deserializeTimestampDictionary(timestamp_dict_raw, *manager);
    }
    
    if (options.load_logtype_dict && !compressed_data.logtype_dict.empty()) {
        std::vector<uint8_t> logtype_dict_raw = decompressWithZstd(compressed_data.logtype_dict);
        deserializeLogTypeDictionary(logtype_dict_raw, *manager);
    }
    
    // 3. 按需重建Trie结构
    std::unique_ptr<LOUDSTrie> louds = nullptr;
    if (options.load_trie && !field_order.empty()) {
        // 先重建LOUDS Trie结构
        std::vector<uint8_t> trie_raw = decompressWithZstd(compressed_data.trie_bitmap);
        louds = deserializeLoudsTrie(trie_raw, *manager, field_order);
    }
    
    // 4. 按需重建分层内容
    if (options.load_layers && louds) {
        if (compressed_data.use_layer_separation) {
            // 确保layers_向量有足够的空间来容纳所有层
            auto& layered_storage = louds->getLayeredStorage();
            size_t required_layers = compressed_data.layer_data_by_level.size();
            while (layered_storage.getLayerCount() < required_layers) {
                layered_storage.addLayer(FieldKey{}, 0);
            }
            
            // 按指定层解压缩
            if (!options.specific_layers.empty()) {
                for (size_t layer_idx : options.specific_layers) {
                    if (layer_idx < compressed_data.layer_data_by_level.size()) {
                        std::vector<uint8_t> layer_data = decompressWithZstd(compressed_data.layer_data_by_level[layer_idx]);
                        std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
                        layered_storage.deserializeLayer(layer_idx, layer_stream);
                    }
                }
            } else {
                // 解压缩所有层
                for (size_t i = 0; i < compressed_data.layer_data_by_level.size(); ++i) {
                    std::vector<uint8_t> layer_data = decompressWithZstd(compressed_data.layer_data_by_level[i]);
                    std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
                    layered_storage.deserializeLayer(i, layer_stream);
                }
            }
        } else {
            // 整体解压缩
            std::vector<uint8_t> layer_raw = decompressWithZstd(compressed_data.layer_data_combined);
            std::istringstream all_layers_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
            louds->getLayeredStorage().deserialize(all_layers_stream);
        }
    }
    
    // 5. 如果需要Trie，将LOUDS Trie转换为普通Trie
    if (louds && options.load_trie) {
        trie = std::make_unique<Trie>(field_order);
        loudsToTrie(*louds, *trie);
    }
    
    return std::make_pair(std::move(trie), std::move(manager));
}

} // namespace json2
