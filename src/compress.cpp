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
    // 2. 用 LOUDS Trie 进行压缩
    return compressLouds(louds, manager, trie.getOrderedFields());
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
        writeValue(string_dict_raw, string_dict_count);
        for (size_t i = 0; i < string_dicts.size(); ++i) {
            const auto& [fk, values] = string_dicts[i];
            writeString(string_dict_raw, fk.name);
            writeValue(string_dict_raw, static_cast<uint32_t>(fk.type));
            writeVector(string_dict_raw, values);
        }
    }
    auto compressed_string_dict = compressWithZstd(string_dict_raw);

    // 3.2 Timestamp pattern
    std::vector<uint8_t> ts_dict_raw;
    {
        const auto& ts_dict = manager.timestampDict();
        std::vector<std::string> patterns;
        for (uint32_t i = 1;; ++i) {
            std::string pattern = ts_dict.getPatternById(i);
            if (pattern.empty()) break;
            patterns.push_back(pattern);
        }
        writeVector(ts_dict_raw, patterns);
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
    // 2. Timestamp pattern列表
    pos = 0;
    std::vector<std::string> patterns = readVector(ts_dict_raw, pos);
    auto& ts_dict = manager->timestampDict();
    for (const auto& pattern : patterns) {
        if (!pattern.empty()) {
            ts_dict.registerPattern(pattern);
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

} // namespace json2 