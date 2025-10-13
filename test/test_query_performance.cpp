#include "../include/query/query_engine.h"
#include "../include/chunked_type_aware_compress.h"
#include "../include/compress.h"
#include "../include/louds.h"
#include "../include/loudsTotrie.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>

using namespace json2;
using namespace json2::query;

class GranularQueryTester {
private:
    QueryEngine engine_;
    std::vector<ChunkedTypeAwareBlock> blocks_;
    std::vector<GranularCompressedData> granular_data_;
    
public:
    GranularQueryTester() : engine_(QueryConfig{}) {}
    
    bool loadCompressedData(const std::string& data_dir) {
        try {
            ChunkedTypeAwareCompressor::SelectiveLoadOptions load_options;
            
            // Use the correct compression configuration that matches the one used during compression
            compression::TypeAwareCompressionConfig config;
            config.louds_backend = compression::CompressionBackend::BIT_PACKING;
            config.dictionary_backend = compression::CompressionBackend::ZSTD;
            config.metadata_backend = compression::CompressionBackend::ZSTD;
            
            // Configure field type compression backends to match test_granular_type_aware_chunked_cmp.cpp
            config.layer_config.int_backend = compression::CompressionBackend::DELTA_VARINT;
            config.layer_config.double_backend = compression::CompressionBackend::DELTA_VARINT;
            config.layer_config.bool_backend = compression::CompressionBackend::BIT_PACKING;
            config.layer_config.string_backend = compression::CompressionBackend::DELTA_VARINT;
            config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_DELTA;
            config.layer_config.logtype_backend = compression::CompressionBackend::DELTA_VARINT;
            config.layer_config.array_backend = compression::CompressionBackend::RLE;
            config.layer_config.null_backend = compression::CompressionBackend::BIT_PACKING;
            
            config.compression_level = 3;
            
            blocks_ = ChunkedTypeAwareCompressor::loadFromDirectorySelective(
                data_dir, load_options, config);
            
            if (blocks_.empty()) {
                std::cerr << "错误: 没有加载到任何压缩块" << std::endl;
                return false;
            }
            
            // 从压缩数据中重建GranularCompressedData
            granular_data_.clear();
            for (size_t i = 0; i < blocks_.size(); ++i) {
                GranularCompressedData gdata = loadGranularDataFromDirectory(data_dir, i);
                if (!gdata.metadata.empty()) {
                    granular_data_.push_back(std::move(gdata));
                }
            }
            
            std::cout << "成功加载 " << blocks_.size() << " 个压缩块" << std::endl;
            std::cout << "成功重建 " << granular_data_.size() << " 个细粒度数据" << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "加载压缩数据失败: " << e.what() << std::endl;
            return false;
        }
    }
    
private:
    GranularCompressedData loadGranularDataFromDirectory(const std::string& data_dir, size_t block_index) {
        GranularCompressedData gdata;
        
        char chunk_dir_buf[256];
        snprintf(chunk_dir_buf, sizeof(chunk_dir_buf), "%s/chunks/chunk_%06zu", data_dir.c_str(), block_index);
        std::string block_dir = chunk_dir_buf;
        
        std::cerr << "[DEBUG] Loading granular data from directory: " << block_dir << std::endl;
        
        if (!std::filesystem::exists(block_dir)) {
            std::cerr << "[DEBUG] Directory does not exist: " << block_dir << std::endl;
            return gdata;
        }
        
        try {
            // 读取块元数据
            std::ifstream block_metadata(block_dir + "/block_metadata.json2", std::ios::binary);
            if (block_metadata.is_open()) {
                size_t original_size;
                double placeholder_ratio;
                uint32_t config_compression_level;
                uint8_t flags;
                
                block_metadata.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
                block_metadata.read(reinterpret_cast<char*>(&placeholder_ratio), sizeof(placeholder_ratio));
                block_metadata.read(reinterpret_cast<char*>(&config_compression_level), sizeof(config_compression_level));
                block_metadata.read(reinterpret_cast<char*>(&flags), sizeof(flags));
                block_metadata.close();
                
                gdata.original_size = original_size;
                gdata.use_layer_separation = (flags & 2) != 0;
                
                std::cerr << "[DEBUG] Block metadata - original_size: " << original_size 
                          << ", use_layer_separation: " << gdata.use_layer_separation << std::endl;
            }
            
            // 读取Trie位图
            std::ifstream trie_file(block_dir + "/louds.json2", std::ios::binary);
            if (trie_file.is_open()) {
                uint32_t trie_size;
                trie_file.read(reinterpret_cast<char*>(&trie_size), sizeof(trie_size));
                gdata.trie_bitmap.resize(trie_size);
                trie_file.read(reinterpret_cast<char*>(gdata.trie_bitmap.data()), trie_size);
                trie_file.close();
                
                std::cerr << "[DEBUG] Trie bitmap size: " << trie_size << " bytes" << std::endl;
            }
            
            // 读取字典数据
            std::string dict_dir = block_dir + "/dictionaries";
            
            // 字符串字典
            std::ifstream string_file(dict_dir + "/variables.json2", std::ios::binary);
            if (string_file.is_open()) {
                uint32_t string_size;
                string_file.read(reinterpret_cast<char*>(&string_size), sizeof(string_size));
                gdata.string_dict.resize(string_size);
                string_file.read(reinterpret_cast<char*>(gdata.string_dict.data()), string_size);
                string_file.close();
                
                std::cerr << "[DEBUG] String dict size: " << string_size << " bytes" << std::endl;
            }
            
            // 时间戳字典
            std::ifstream ts_file(dict_dir + "/timestamps.json2", std::ios::binary);
            if (ts_file.is_open()) {
                uint32_t ts_size;
                ts_file.read(reinterpret_cast<char*>(&ts_size), sizeof(ts_size));
                gdata.timestamp_dict.resize(ts_size);
                ts_file.read(reinterpret_cast<char*>(gdata.timestamp_dict.data()), ts_size);
                ts_file.close();
                
                std::cerr << "[DEBUG] Timestamp dict size: " << ts_size << " bytes" << std::endl;
            }
            
            // LogType字典
            std::ifstream log_file(dict_dir + "/logtypes.json2", std::ios::binary);
            if (log_file.is_open()) {
                uint32_t log_size;
                log_file.read(reinterpret_cast<char*>(&log_size), sizeof(log_size));
                gdata.logtype_dict.resize(log_size);
                log_file.read(reinterpret_cast<char*>(gdata.logtype_dict.data()), log_size);
                log_file.close();
                
                std::cerr << "[DEBUG] LogType dict size: " << log_size << " bytes" << std::endl;
            }
            
            // 细粒度压缩的元数据文件
            std::ifstream granular_metadata_file(block_dir + "/metadata.json2", std::ios::binary);
            if (granular_metadata_file.is_open()) {
                uint32_t metadata_size;
                granular_metadata_file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
                gdata.metadata.resize(metadata_size);
                granular_metadata_file.read(reinterpret_cast<char*>(gdata.metadata.data()), metadata_size);
                granular_metadata_file.close();
                
                std::cerr << "[DEBUG] Granular metadata size: " << metadata_size << " bytes" << std::endl;
            }
            
            // 读取层大小信息（如果存在）
            std::ifstream layer_sizes_file(block_dir + "/layer_sizes.json2", std::ios::binary);
            if (layer_sizes_file.is_open()) {
                uint32_t layer_sizes_size;
                layer_sizes_file.read(reinterpret_cast<char*>(&layer_sizes_size), sizeof(layer_sizes_size));
                gdata.layer_sizes.resize(layer_sizes_size);
                layer_sizes_file.read(reinterpret_cast<char*>(gdata.layer_sizes.data()), layer_sizes_size);
                layer_sizes_file.close();
                
                std::cerr << "[DEBUG] Layer sizes size: " << layer_sizes_size << " bytes" << std::endl;
            }
            
            // 读取层数据
            if (gdata.use_layer_separation) {
                // 按层分别读取
                size_t layer_idx = 0;
                while (true) {
                    char layer_file_buf[256];
                    snprintf(layer_file_buf, sizeof(layer_file_buf), "%s/layer_%zu.json2", block_dir.c_str(), layer_idx);
                    std::ifstream layer_stream(layer_file_buf, std::ios::binary);
                    if (!layer_stream.is_open()) {
                        if (layer_idx == 0) {
                            std::cerr << "[DEBUG] No layer files found" << std::endl;
                        } else {
                            std::cerr << "[DEBUG] Loaded " << layer_idx << " layer files" << std::endl;
                        }
                        break;
                    }
                    uint32_t layer_size;
                    if (!layer_stream.read(reinterpret_cast<char*>(&layer_size), sizeof(layer_size))) {
                        std::cerr << "[DEBUG] Failed to read layer size for layer " << layer_idx << std::endl;
                        break;
                    }
                    std::vector<uint8_t> layer_data(layer_size);
                    if (!layer_stream.read(reinterpret_cast<char*>(layer_data.data()), layer_size)) {
                        std::cerr << "[DEBUG] Failed to read layer data for layer " << layer_idx << std::endl;
                        break;
                    }
                    gdata.layer_data_by_level.push_back(std::move(layer_data));
                    layer_stream.close();
                    std::cerr << "[DEBUG] Layer " << layer_idx << " size: " << layer_size << " bytes" << std::endl;
                    layer_idx++;
                }
            } else {
                // 整体读取
                std::ifstream layers_file(block_dir + "/layers.json2", std::ios::binary);
                if (layers_file.is_open()) {
                    uint32_t layers_size;
                    layers_file.read(reinterpret_cast<char*>(&layers_size), sizeof(layers_size));
                    gdata.layer_data_combined.resize(layers_size);
                    layers_file.read(reinterpret_cast<char*>(gdata.layer_data_combined.data()), layers_size);
                    layers_file.close();
                    
                    std::cerr << "[DEBUG] Combined layers size: " << layers_size << " bytes" << std::endl;
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "加载细粒度数据失败: " << e.what() << std::endl;
        }
        
        return gdata;
    }
    
public:
    void testFieldExistenceQueries() {
        std::cout << "\n=== 字段存在性查询测试 ===" << std::endl;
        
        if (granular_data_.empty()) {
            std::cout << "没有可用的细粒度数据" << std::endl;
            return;
        }
        
        const auto& granular_data = granular_data_[0];
        std::vector<std::string> test_fields = {
            "timestamp", "user", "dbname", "pid", "session_id", 
            "error_severity", "message", "application_name"
        };
        
        for (const auto& field_name : test_fields) {
            auto start = std::chrono::high_resolution_clock::now();
            
            auto result = engine_.checkFieldExistenceAndType(
                field_name, FieldType::String, granular_data);
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration<double, std::milli>(end - start);
            
            std::cout << "字段: " << field_name << std::endl;
            std::cout << "  存在: " << (result.exists ? "是" : "否") << std::endl;
            if (result.exists) {
                std::cout << "  类型: " << static_cast<int>(result.field_type) << std::endl;
                std::cout << "  类型匹配: " << (result.type_matches ? "是" : "否") << std::endl;
            }
            std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                      << duration.count() << " ms" << std::endl;
            if (!result.error_message.empty()) {
                std::cout << "  错误: " << result.error_message << std::endl;
            }
            std::cout << std::endl;
        }
    }
    
    void testDictionaryQueries() {
        std::cout << "\n=== 字典查询测试 ===" << std::endl;
        
        if (granular_data_.empty()) {
            std::cout << "没有可用的细粒度数据" << std::endl;
            return;
        }
        
        const auto& granular_data = granular_data_[0];
        
        // 测试字符串字典
        std::cout << "字符串字典查询:" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto string_result = engine_.queryDictionary(
            "user", FieldType::String, granular_data, "");
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到: " << (string_result.found ? "是" : "否") << std::endl;
        std::cout << "  值数量: " << string_result.values.size() << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!string_result.error_message.empty()) {
            std::cout << "  错误: " << string_result.error_message << std::endl;
        }
        
        // 测试时间戳字典
        std::cout << "\n时间戳字典查询:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto timestamp_result = engine_.queryDictionary(
            "timestamp", FieldType::Timestamp, granular_data, "");
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到: " << (timestamp_result.found ? "是" : "否") << std::endl;
        std::cout << "  值数量: " << timestamp_result.values.size() << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!timestamp_result.error_message.empty()) {
            std::cout << "  错误: " << timestamp_result.error_message << std::endl;
        }
        
        // 测试日志类型字典
        std::cout << "\n日志类型字典查询:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto logtype_result = engine_.queryDictionary(
            "message", FieldType::LogType, granular_data, "");
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到: " << (logtype_result.found ? "是" : "否") << std::endl;
        std::cout << "  值数量: " << logtype_result.values.size() << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!logtype_result.error_message.empty()) {
            std::cout << "  错误: " << logtype_result.error_message << std::endl;
        }
    }
    
    void testRecordQueries() {
        std::cout << "\n=== 记录查询测试 ===" << std::endl;
        
        if (granular_data_.empty()) {
            std::cout << "没有可用的细粒度数据" << std::endl;
            return;
        }
        
        const auto& granular_data = granular_data_[0];
        
        // 测试精确匹配查询
        std::cout << "精确匹配查询 (user=postgres):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto exact_result = engine_.executeExactMatchQuery(
            "user", "postgres", granular_data, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  记录数: " << exact_result.count << std::endl;
        std::cout << "  访问块数: " << exact_result.chunks_accessed << std::endl;
        std::cout << "  解压比例: " << std::fixed << std::setprecision(2) 
                  << (exact_result.decompression_ratio * 100) << "%" << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << exact_result.query_time_ms << " ms" << std::endl;
        std::cout << "  完成: " << (exact_result.is_complete ? "是" : "否") << std::endl;
        if (!exact_result.error_message.empty()) {
            std::cout << "  错误: " << exact_result.error_message << std::endl;
        }
        
        // 测试范围查询
        std::cout << "\n范围查询 (pid: 7880-7890):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto range_result = engine_.executeRangeQuery(
            "pid", "7880", "7890", granular_data, blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  记录数: " << range_result.count << std::endl;
        std::cout << "  访问块数: " << range_result.chunks_accessed << std::endl;
        std::cout << "  解压比例: " << std::fixed << std::setprecision(2) 
                  << (range_result.decompression_ratio * 100) << "%" << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << range_result.query_time_ms << " ms" << std::endl;
        std::cout << "  完成: " << (range_result.is_complete ? "是" : "否") << std::endl;
        if (!range_result.error_message.empty()) {
            std::cout << "  错误: " << range_result.error_message << std::endl;
        }
    }
    
    void analyzeCompressedData() {
        std::cout << "\n=== 压缩数据分析 ===" << std::endl;
        
        std::cout << "总块数: " << blocks_.size() << std::endl;
        std::cout << "细粒度数据数: " << granular_data_.size() << std::endl;
        
        for (size_t i = 0; i < granular_data_.size(); ++i) {
            const auto& granular = granular_data_[i];
            std::cout << "\n细粒度数据 " << i << ":" << std::endl;
            std::cout << "  原始大小: " << granular.original_size << " bytes" << std::endl;
            std::cout << "  元数据大小: " << granular.metadata.size() << " bytes" << std::endl;
            std::cout << "  字符串字典大小: " << granular.string_dict.size() << " bytes" << std::endl;
            std::cout << "  时间戳字典大小: " << granular.timestamp_dict.size() << " bytes" << std::endl;
            std::cout << "  日志类型字典大小: " << granular.logtype_dict.size() << " bytes" << std::endl;
            std::cout << "  层数据数量: " << granular.layer_data_by_level.size() << std::endl;
            std::cout << "  Trie位图大小: " << granular.trie_bitmap.size() << " bytes" << std::endl;
            std::cout << "  使用层分离: " << (granular.use_layer_separation ? "是" : "否") << std::endl;
            
            // 计算总压缩大小
            size_t total_compressed = granular.metadata.size() + 
                                    granular.string_dict.size() + 
                                    granular.timestamp_dict.size() + 
                                    granular.logtype_dict.size() + 
                                    granular.trie_bitmap.size();
            for (const auto& layer : granular.layer_data_by_level) {
                total_compressed += layer.size();
            }
            
            std::cout << "  总压缩大小: " << total_compressed << " bytes" << std::endl;
            if (granular.original_size > 0) {
                std::cout << "  压缩比: " << std::fixed << std::setprecision(2) 
                          << (double)granular.original_size / total_compressed << std::endl;
            }
        }
    }
};

int main() {
    const std::string compressed_data_dir = "compressed_type_aware_data"; // Changed from "build/compressed_type_aware_data"
    
    std::cout << "细粒度压缩数据查询测试" << std::endl;
    std::cout << "压缩数据目录: " << compressed_data_dir << std::endl;
    
    if (!std::filesystem::exists(compressed_data_dir)) {
        std::cerr << "错误: 压缩数据目录不存在: " << compressed_data_dir << std::endl;
        return 1;
    }
    
    GranularQueryTester tester;
    
    // 加载压缩数据
    if (!tester.loadCompressedData(compressed_data_dir)) {
        std::cerr << "加载压缩数据失败" << std::endl;
        return 1;
    }
    
    // 运行测试
    tester.testFieldExistenceQueries();
    tester.testDictionaryQueries();
    tester.analyzeCompressedData();
    tester.testRecordQueries();
    
    std::cout << "\n测试完成！" << std::endl;
    return 0;
}