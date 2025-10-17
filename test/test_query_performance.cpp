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

class QueryTest {
private:
    QueryEngine engine_;
    std::vector<ChunkedTypeAwareBlock> blocks_;
    std::vector<GranularCompressedData> granular_data_;
    
public:
    QueryTest() {
        // Create a custom query config with higher max_results limit
        QueryConfig config;
        config.max_results = 50000;  // Set to 50,000 to accommodate your 20,000 matching records
        config.enable_parallel = false;
        config.prune_only = false;
        config.sample_limit = 10;
        
        engine_ = QueryEngine(config);
    }
    
    // Method to set the data directory for the query engine
    void setDataDirectory(const std::string& data_dir) {
        engine_.setDataDirectory(data_dir);
    }
    
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
        
        if (!std::filesystem::exists(block_dir)) {
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
            }
            
            // 读取Trie位图
            std::ifstream trie_file(block_dir + "/louds.json2", std::ios::binary);
            if (trie_file.is_open()) {
                uint32_t trie_size;
                trie_file.read(reinterpret_cast<char*>(&trie_size), sizeof(trie_size));
                gdata.trie_bitmap.resize(trie_size);
                trie_file.read(reinterpret_cast<char*>(gdata.trie_bitmap.data()), trie_size);
                trie_file.close();
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
            }
            
            // 时间戳字典
            std::ifstream ts_file(dict_dir + "/timestamps.json2", std::ios::binary);
            if (ts_file.is_open()) {
                uint32_t ts_size;
                ts_file.read(reinterpret_cast<char*>(&ts_size), sizeof(ts_size));
                gdata.timestamp_dict.resize(ts_size);
                ts_file.read(reinterpret_cast<char*>(gdata.timestamp_dict.data()), ts_size);
                ts_file.close();
            }
            
            // LogType字典
            std::ifstream log_file(dict_dir + "/logtypes.json2", std::ios::binary);
            if (log_file.is_open()) {
                uint32_t log_size;
                log_file.read(reinterpret_cast<char*>(&log_size), sizeof(log_size));
                gdata.logtype_dict.resize(log_size);
                log_file.read(reinterpret_cast<char*>(gdata.logtype_dict.data()), log_size);
                log_file.close();
            }
            
            // 细粒度压缩的元数据文件
            std::ifstream granular_metadata_file(block_dir + "/metadata.json2", std::ios::binary);
            if (granular_metadata_file.is_open()) {
                uint32_t metadata_size;
                granular_metadata_file.read(reinterpret_cast<char*>(&metadata_size), sizeof(metadata_size));
                gdata.metadata.resize(metadata_size);
                granular_metadata_file.read(reinterpret_cast<char*>(gdata.metadata.data()), metadata_size);
                granular_metadata_file.close();
            }
            
            // 读取层大小信息（如果存在）
            std::ifstream layer_sizes_file(block_dir + "/layer_sizes.json2", std::ios::binary);
            if (layer_sizes_file.is_open()) {
                uint32_t layer_sizes_size;
                layer_sizes_file.read(reinterpret_cast<char*>(&layer_sizes_size), sizeof(layer_sizes_size));
                gdata.layer_sizes.resize(layer_sizes_size);
                layer_sizes_file.read(reinterpret_cast<char*>(gdata.layer_sizes.data()), layer_sizes_size);
                layer_sizes_file.close();
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
                        break;
                    }
                    uint32_t layer_size;
                    if (!layer_stream.read(reinterpret_cast<char*>(&layer_size), sizeof(layer_size))) {
                        break;
                    }
                    std::vector<uint8_t> layer_data(layer_size);
                    if (!layer_stream.read(reinterpret_cast<char*>(layer_data.data()), layer_size)) {
                        break;
                    }
                    gdata.layer_data_by_level.push_back(std::move(layer_data));
                    layer_stream.close();
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
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        std::vector<std::string> test_fields = {
            "timestamp", "user", "dbname", "pid", "session_id", 
            "error_severity", "message", "application_name"
        };
        
        // Process all chunks
        for (size_t chunk_idx = 0; chunk_idx < granular_data_.size(); ++chunk_idx) {
            std::cout << "\n--- 处理块 " << chunk_idx << " ---" << std::endl;
            const auto& granular_data = granular_data_[chunk_idx];
            
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
    }
    
    void testDictionaryQueries() {
        std::cout << "\n=== 字典查询测试 ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // Process all chunks
        for (size_t chunk_idx = 0; chunk_idx < granular_data_.size(); ++chunk_idx) {
            std::cout << "\n--- 处理块 " << chunk_idx << " ---" << std::endl;
            const auto& granular_data = granular_data_[chunk_idx];
            
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
            } else {
                // 输出前几个值用于调试
                std::cout << "  前几个值: ";
                for (size_t i = 0; i < std::min(size_t(5), string_result.values.size()); ++i) {
                    std::cout << "\"" << string_result.values[i] << "\" ";
                }
                std::cout << std::endl;
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
    }
    
    void testExactMatchQueries() {
        std::cout << "\n=== 精确匹配查询测试 ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
               
        // 测试精确匹配查询 - 查找 user = "postgres" (使用所有块)
        std::cout << "\n精确匹配查询 (user = \"postgres\"):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeExactMatchQueryMultiBlock(
            "user", "postgres", blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  访问块数: " << result.chunks_accessed << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            // 显示前几个匹配的记录
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试精确匹配查询 - 查找 dbname = "example" (使用所有块)
        std::cout << "\n精确匹配查询 (dbname = \"example\"):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeExactMatchQueryMultiBlock(
            "dbname", "example", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  访问块数: " << result.chunks_accessed << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            // 显示前几个匹配的记录
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试精确匹配查询 - 查找 application_name = "pgbench" (使用所有块)
        std::cout << "\n精确匹配查询 (application_name = \"pgbench\"):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeExactMatchQueryMultiBlock(
            "application_name", "pgbench", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  访问块数: " << result.chunks_accessed << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            // 显示前几个匹配的记录
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
    
    void testRangeQueries() {
        std::cout << "\n=== 范围查询测试 ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // Process all chunks for initial checks
        for (size_t chunk_idx = 0; chunk_idx < granular_data_.size(); ++chunk_idx) {
            std::cout << "\n--- 处理块 " << chunk_idx << " ---" << std::endl;
            const auto& granular_data = granular_data_[chunk_idx];
            
            // 首先检查字段是否存在
            std::cout << "检查字段存在性:" << std::endl;
            auto pid_existence = engine_.checkFieldExistenceAndType("pid", FieldType::Int, granular_data);
            std::cout << "  pid字段存在: " << (pid_existence.exists ? "是" : "否") << std::endl;
            if (pid_existence.exists) {
                std::cout << "  pid字段类型: " << static_cast<int>(pid_existence.field_type) << std::endl;
            }
        }
        
        // 测试范围查询 - 查找 pid 在 [7880, 7890] 范围内的记录 (使用所有块)
        std::cout << "\n范围查询 (pid 在 [7880, 7890] 范围内):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeRangeQueryMultiBlock(
            "pid", "7880", "7890", blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  访问块数: " << result.chunks_accessed << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            // 显示前几个匹配的记录
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试范围查询 - 查找 timestamp 在特定范围内的记录 (使用所有块)
        std::cout << "\n范围查询 (timestamp 在 [\"2023-03-27 00:32:15.929 EDT\", \"2023-03-27 00:32:15.936 EDT\"] 范围内):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeRangeQueryMultiBlock(
            "timestamp", "2023-03-27 00:32:15.929 EDT", "2023-03-27 00:32:15.936 EDT", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  访问块数: " << result.chunks_accessed << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            // 显示前几个匹配的记录
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
    
    void testAggregateQueries() {
        std::cout << "\n=== 聚合查询测试 ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // 测试COUNT聚合查询 - 计算所有记录数 (使用所有块)
        std::cout << "\n1. COUNT聚合查询 (COUNT(*)):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto count_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::COUNT, "", blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  记录总数: " << count_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!count_result.error_message.empty()) {
            std::cout << "  错误: " << count_result.error_message << std::endl;
        }
        
        // 测试COUNT聚合查询 - 计算特定字段非空值数 (使用所有块)
        std::cout << "\n2. COUNT聚合查询 (COUNT(user)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        count_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::COUNT, "user", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  user字段非空值数: " << count_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!count_result.error_message.empty()) {
            std::cout << "  错误: " << count_result.error_message << std::endl;
        }
        
        // 测试COUNT聚合查询 - 计算pid字段非空值数 (使用所有块)
        std::cout << "\n3. COUNT聚合查询 (COUNT(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        count_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::COUNT, "pid", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段非空值数: " << count_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!count_result.error_message.empty()) {
            std::cout << "  错误: " << count_result.error_message << std::endl;
        }
        
        // 测试SUM聚合查询 - 计算pid字段总和 (使用所有块)
        std::cout << "\n4. SUM聚合查询 (SUM(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sum_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::SUM, "pid", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段总和: " << sum_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!sum_result.error_message.empty()) {
            std::cout << "  错误: " << sum_result.error_message << std::endl;
        }
        
        // 测试AVG聚合查询 - 计算pid字段平均值 (使用所有块)
        std::cout << "\n5. AVG聚合查询 (AVG(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto avg_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::AVG, "pid", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段平均值: " << avg_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!avg_result.error_message.empty()) {
            std::cout << "  错误: " << avg_result.error_message << std::endl;
        }
        
        // 测试MAX聚合查询 - 计算pid字段最大值 (使用所有块)
        std::cout << "\n6. MAX聚合查询 (MAX(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto max_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::MAX, "pid", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段最大值: " << max_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!max_result.error_message.empty()) {
            std::cout << "  错误: " << max_result.error_message << std::endl;
        }
        
        // 测试MIN聚合查询 - 计算pid字段最小值 (使用所有块)
        std::cout << "\n7. MIN聚合查询 (MIN(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto min_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::MIN, "pid", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段最小值: " << min_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!min_result.error_message.empty()) {
            std::cout << "  错误: " << min_result.error_message << std::endl;
        }
    }
    
    void testGroupByQueries() {
        std::cout << "\n=== GROUP BY 查询测试 ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        const auto& granular_data = granular_data_[0];
        
        // 测试基本的GROUP BY查询
        std::cout << "\n1. 测试基本 GROUP BY 查询:" << std::endl;
        
        // 测试按user字段分组并计算COUNT(*)
        std::cout << "测试 GROUP BY user COUNT(*) 查询:" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeComplexQueryMultiBlock("COUNT(*) GROUP BY user", blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  分组数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  分组结果示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(5), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试按多个字段分组
        std::cout << "\n2. 测试按多个字段分组:" << std::endl;
        std::cout << "测试 GROUP BY user, pid COUNT(*) 查询:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("COUNT(*) GROUP BY user, pid", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  分组数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  分组结果示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(5), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试带条件的GROUP BY查询
        std::cout << "\n3. 测试带条件的 GROUP BY 查询:" << std::endl;
        std::cout << "测试 user:postgres GROUP BY pid COUNT(*) 查询:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("user:postgres GROUP BY pid COUNT(*)", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  分组数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  分组结果示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(5), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
    
    void testDirectGroupedAggregateQuery() {
        std::cout << "\n=== 分组聚合查询测试 ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        const auto& granular_data = granular_data_[0];
        
        // 直接测试executeGroupedAggregateQuery方法
        std::cout << "\n1. 测试分组聚合查询:" << std::endl;
        
        std::vector<AggregateFunction> agg_funcs = {AggregateFunction::COUNT};
        std::vector<std::string> agg_fields = {""}; // COUNT(*) uses empty field name
        std::vector<std::string> group_fields = {"pid"};
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeGroupedAggregateQueryMultiBlock(agg_funcs, agg_fields, group_fields, blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  总记录数: " << result.total_count << std::endl;
        std::cout << "  分组数: " << result.groups_count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  分组结果示例:" << std::endl;
            size_t count = 0;
            for (const auto& group_entry : result.grouped_values) {
                // if (count >= 5) break;
                
                const std::vector<std::string>& group_values = group_entry.first;
                const auto& agg_values = group_entry.second;
                
                std::cout << "    Group: ";
                for (size_t i = 0; i < std::min(group_fields.size(), group_values.size()); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << group_fields[i] << "=" << group_values[i];
                }
                
                std::cout << " => ";
                for (const auto& agg_entry : agg_values) {
                    std::cout << agg_entry.first << "=" << agg_entry.second << " ";
                }
                std::cout << std::endl;
                
                count++;
            }
        }
    }

    void testComplexQueries() {
        std::cout << "\n=== 复杂查询测试 ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        const auto& granular_data = granular_data_[0];
        
        // 测试字段存在性查询 (这是当前实现中最可靠的查询)
        std::cout << "\n1. 测试字段存在性查询:" << std::endl;
        std::cout << "测试字段 'user' 存在:" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeComplexQueryMultiBlock("user:*", blocks_);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  字段存在: " << (result.count > 0 ? "是" : "否") << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        }
        
        // 测试精确匹配查询 (使用冒号语法)
        std::cout << "\n2. 测试精确匹配查询:" << std::endl;
        std::cout << "测试 user = postgres:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("user:postgres", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试另一个字段的精确匹配
        std::cout << "\n测试 dbname = example:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("dbname:example", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试逻辑操作 (AND)
        std::cout << "\n3. 测试逻辑操作:" << std::endl;
        std::cout << "测试AND操作 (user:postgres AND dbname:example):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("user:postgres AND dbname:example", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试逻辑操作 (OR)
        std::cout << "\n测试OR操作 (user:postgres OR user:alice):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("user:postgres OR user:alice", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试括号表达式
        std::cout << "\n4. 测试括号表达式:" << std::endl;
        std::cout << "测试复杂表达式 ((user:postgres OR user:alice) AND dbname:example):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("(user:postgres OR user:alice) AND dbname:example", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
        
        // 测试NOT操作 (简化测试，因为NOT实现可能不完整)
        std::cout << "\n5. 测试NOT操作:" << std::endl;
        std::cout << "测试NOT操作 (NOT user:postgres) - 注意：此功能可能不完整:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlock("NOT user:postgres", blocks_);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!result.error_message.empty()) {
            std::cout << "  错误: " << result.error_message << std::endl;
        } else {
            std::cout << "  匹配记录示例:" << std::endl;
            for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                std::cout << "    " << result.records[i] << std::endl;
            }
        }
    }
};

int main() {
    const std::string compressed_data_dir = "compressed_type_aware_data";
    
    std::cout << "查询测试" << std::endl;
    std::cout << "压缩数据目录: " << compressed_data_dir << std::endl;
    
    if (!std::filesystem::exists(compressed_data_dir)) {
        std::cerr << "错误: 压缩数据目录不存在: " << compressed_data_dir << std::endl;
        return 1;
    }
    
    QueryTest tester;
    
    // Set the data directory for the query engine
    // This is needed for granular data extraction from chunk directories
    tester.setDataDirectory(compressed_data_dir);
    
    // 加载压缩数据
    if (!tester.loadCompressedData(compressed_data_dir)) {
        std::cerr << "加载压缩数据失败" << std::endl;
        return 1;
    }
    
    // 运行测试
    tester.testFieldExistenceQueries();  // 字段存在性测试
    tester.testDictionaryQueries();  // 字典查询测试
    tester.testExactMatchQueries();  // 精确匹配查询测试
    tester.testRangeQueries();       // 范围查询测试
    tester.testAggregateQueries();   // 聚合查询测试
    tester.testComplexQueries();     // 复杂查询测试
    tester.testDirectGroupedAggregateQuery(); // 分组聚合查询测试

    std::cout << "\n测试完成！" << std::endl;
    return 0;
}