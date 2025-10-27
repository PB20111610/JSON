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
#include <cstring>  // For strchr and strncmp
#include <limits>

// Handle getopt for cross-platform compatibility
#ifdef _WIN32
    // Simple getopt implementation for Windows
    char* optarg;
    int optind = 1;
    int opterr = 1;
    int optopt;

    struct option {
        const char* name;
        int has_arg;
        int* flag;
        int val;
    };

    enum {
        no_argument = 0,
        required_argument = 1,
        optional_argument = 2
    };

    int getopt_long(int argc, char* const argv[], const char* optstring, const struct option* longopts, int* longindex) {
        if (optind >= argc) return -1;
        
        const char* arg = argv[optind];
        if (arg[0] != '-' || arg[1] == '\0') return -1;
        
        if (arg[1] == '-') {
            // Long option
            const char* long_arg = arg + 2;
            for (int i = 0; longopts[i].name != nullptr; i++) {
                const char* opt_name = longopts[i].name;
                // Check if the option matches
                const char* eq_pos = strchr(long_arg, '=');
                size_t name_len = eq_pos ? eq_pos - long_arg : strlen(long_arg);
                
                if (strncmp(long_arg, opt_name, name_len) == 0 && opt_name[name_len] == '\0') {
                    if (longindex) *longindex = i;
                    
                    if (longopts[i].has_arg == required_argument) {
                        if (eq_pos && eq_pos[1] != '\0') {
                            optarg = const_cast<char*>(eq_pos + 1);
                        } else if (optind + 1 < argc) {
                            optarg = argv[++optind];
                        } else {
                            if (opterr) std::cerr << "Option --" << opt_name << " requires an argument\n";
                            return '?';
                        }
                    } else if (longopts[i].has_arg == optional_argument) {
                        if (eq_pos && eq_pos[1] != '\0') {
                            optarg = const_cast<char*>(eq_pos + 1);
                        } else {
                            optarg = nullptr;
                        }
                    } else {
                        optarg = nullptr;
                    }
                    
                    optind++;
                    if (longopts[i].flag) {
                        *(longopts[i].flag) = longopts[i].val;
                        return 0;
                    }
                    return longopts[i].val;
                }
            }
            if (opterr) std::cerr << "Unknown option --" << long_arg << "\n";
            optind++;
            return '?';
        } else {
            // Short option
            char opt = arg[1];
            const char* opt_ptr = strchr(optstring, opt);
            if (!opt_ptr) {
                optopt = opt;
                if (opterr) std::cerr << "Unknown option -" << opt << "\n";
                optind++;
                return '?';
            }
            
            if (opt_ptr[1] == ':') {
                // Option requires argument
                if (arg[2] != '\0') {
                    optarg = const_cast<char*>(arg + 2);
                } else if (optind + 1 < argc) {
                    optarg = argv[++optind];
                } else {
                    optopt = opt;
                    if (opterr) std::cerr << "Option -" << opt << " requires an argument\n";
                    optind++;
                    return '?';
                }
            } else {
                optarg = nullptr;
            }
            
            optind++;
            return opt;
        }
    }
#else
    #include <unistd.h>
    #include <getopt.h>
#endif

using namespace json2;
using namespace json2::query;

class ParallelQueryTest {
private:
    QueryEngine engine_;
    std::vector<ChunkedTypeAwareBlock> blocks_;
    std::vector<GranularCompressedData> granular_data_;
    
public:
    ParallelQueryTest() {
        // Create a custom query config with higher max_results limit
        QueryConfig config;
        config.max_results = std::numeric_limits<size_t>::max(); 
        config.enable_parallel = true;  // Enable parallel processing
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
    
    void testExactMatchQueries(int num_threads) {
        std::cout << "\n=== 精确匹配查询测试 (并行版本) ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // 测试并行精确匹配查询 - 查找 user = "postgres" (使用所有块)
        std::cout << "\n并行精确匹配查询 (user = \"postgres\"):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeExactMatchQueryMultiBlockParallel(
            "user", "postgres", blocks_, num_threads);  
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
        
        // 测试并行精确匹配查询 - 查找 dbname = "example" (使用所有块)
        std::cout << "\n并行精确匹配查询 (dbname = \"example\"):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeExactMatchQueryMultiBlockParallel(
            "dbname", "example", blocks_, num_threads);  
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
        
        // 测试并行精确匹配查询 - 查找 application_name = "pgbench" (使用所有块)
        std::cout << "\n并行精确匹配查询 (application_name = \"pgbench\"):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeExactMatchQueryMultiBlockParallel(
            "application_name", "pgbench", blocks_, num_threads);  
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
        
        // 性能对比测试：顺序 vs 并行
        std::cout << "\n=== 性能对比测试 ===" << std::endl;
        
        // 顺序执行
        std::cout << "\n顺序执行 (user = \"postgres\"):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sequential_result = engine_.executeExactMatchQueryMultiBlock(
            "user", "postgres", blocks_);
        end = std::chrono::high_resolution_clock::now();
        auto sequential_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << sequential_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << sequential_duration.count() << " ms" << std::endl;
        
        // 并行执行
        std::cout << "\n并行执行 (user = \"postgres\", 多线程):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto parallel_result = engine_.executeExactMatchQueryMultiBlockParallel(
            "user", "postgres", blocks_, num_threads);
        end = std::chrono::high_resolution_clock::now();
        auto parallel_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << parallel_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << parallel_duration.count() << " ms" << std::endl;
        
        // 计算加速比
        if (parallel_duration.count() > 0) {
            double speedup = sequential_duration.count() / parallel_duration.count();
            std::cout << "  加速比: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        }
    }
    
    void testRangeQueries(int num_threads) {
        std::cout << "\n=== 范围查询测试 (并行版本) ===" << std::endl;
        
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
        
        // 测试并行范围查询 - 查找 pid 在 [7880, 7890] 范围内的记录 (使用所有块)
        std::cout << "\n并行范围查询 (pid 在 [7880, 7890] 范围内):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeRangeQueryMultiBlockParallel(
            "pid", "7880", "7890", blocks_, num_threads);  
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
        
        // 测试并行范围查询 - 查找 timestamp 在特定范围内的记录 (使用所有块)
        std::cout << "\n并行范围查询 (timestamp 在 [\"2023-03-27 00:32:15.929 EDT\", \"2023-03-27 00:32:15.936 EDT\"] 范围内):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeRangeQueryMultiBlockParallel(
            "timestamp", "2023-03-27 00:32:15.929 EDT", "2023-03-27 00:32:15.936 EDT", blocks_, num_threads);  
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
        
        // 性能对比测试：顺序 vs 并行
        std::cout << "\n=== 性能对比测试 ===" << std::endl;
        
        // 顺序执行
        std::cout << "\n顺序执行 (pid 在 [7880, 7890] 范围内):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sequential_result = engine_.executeRangeQueryMultiBlock(
            "pid", "7880", "7890", blocks_);
        end = std::chrono::high_resolution_clock::now();
        auto sequential_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << sequential_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << sequential_duration.count() << " ms" << std::endl;
        
        // 并行执行
        std::cout << "\n并行执行 (pid 在 [7880, 7890] 范围内, 多线程):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto parallel_result = engine_.executeRangeQueryMultiBlockParallel(
            "pid", "7880", "7890", blocks_, num_threads);
        end = std::chrono::high_resolution_clock::now();
        auto parallel_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << parallel_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << parallel_duration.count() << " ms" << std::endl;
        
        // 计算加速比
        if (parallel_duration.count() > 0) {
            double speedup = sequential_duration.count() / parallel_duration.count();
            std::cout << "  加速比: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        }
    }
    
    void testAggregateQueries(int num_threads) {
        std::cout << "\n=== 聚合查询测试 (并行版本) ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // 测试并行COUNT聚合查询 - 计算所有记录数 (使用所有块)
        std::cout << "\n1. 并行COUNT聚合查询 (COUNT(*)):" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto count_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::COUNT, "", blocks_, num_threads);  
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  记录总数: " << count_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!count_result.error_message.empty()) {
            std::cout << "  错误: " << count_result.error_message << std::endl;
        }
        
        // 测试并行COUNT聚合查询 - 计算特定字段非空值数 (使用所有块)
        std::cout << "\n2. 并行COUNT聚合查询 (COUNT(user)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        count_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::COUNT, "user", blocks_, num_threads);  
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  user字段非空值数: " << count_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!count_result.error_message.empty()) {
            std::cout << "  错误: " << count_result.error_message << std::endl;
        }
        
        // 测试并行COUNT聚合查询 - 计算pid字段非空值数 (使用所有块)
        std::cout << "\n3. 并行COUNT聚合查询 (COUNT(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        count_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::COUNT, "pid", blocks_, num_threads);  
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段非空值数: " << count_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!count_result.error_message.empty()) {
            std::cout << "  错误: " << count_result.error_message << std::endl;
        }
        
        // 测试并行SUM聚合查询 - 计算pid字段总和 (使用所有块)
        std::cout << "\n4. 并行SUM聚合查询 (SUM(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sum_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::SUM, "pid", blocks_, num_threads);  
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段总和: " << sum_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!sum_result.error_message.empty()) {
            std::cout << "  错误: " << sum_result.error_message << std::endl;
        }
        
        // 测试并行AVG聚合查询 - 计算pid字段平均值 (使用所有块)
        std::cout << "\n5. 并行AVG聚合查询 (AVG(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto avg_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::AVG, "pid", blocks_, num_threads);  
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段平均值: " << avg_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!avg_result.error_message.empty()) {
            std::cout << "  错误: " << avg_result.error_message << std::endl;
        }
        
        // 测试并行MAX聚合查询 - 计算pid字段最大值 (使用所有块)
        std::cout << "\n6. 并行MAX聚合查询 (MAX(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto max_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::MAX, "pid", blocks_, num_threads);  
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段最大值: " << max_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!max_result.error_message.empty()) {
            std::cout << "  错误: " << max_result.error_message << std::endl;
        }
        
        // 测试并行MIN聚合查询 - 计算pid字段最小值 (使用所有块)
        std::cout << "\n7. 并行MIN聚合查询 (MIN(pid)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto min_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::MIN, "pid", blocks_, num_threads);  
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  pid字段最小值: " << min_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << duration.count() << " ms" << std::endl;
        if (!min_result.error_message.empty()) {
            std::cout << "  错误: " << min_result.error_message << std::endl;
        }
        
        // 性能对比测试：顺序 vs 并行
        std::cout << "\n=== 性能对比测试 ===" << std::endl;
        
        // 顺序执行
        std::cout << "\n顺序执行 (COUNT(*)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sequential_result = engine_.executeAggregateQueryMultiBlock(
            AggregateFunction::COUNT, "", blocks_);
        end = std::chrono::high_resolution_clock::now();
        auto sequential_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  记录总数: " << sequential_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << sequential_duration.count() << " ms" << std::endl;
        
        // 并行执行
        std::cout << "\n并行执行 (COUNT(*), 多线程):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto parallel_result = engine_.executeAggregateQueryMultiBlockParallel(
            AggregateFunction::COUNT, "", blocks_, num_threads);
        end = std::chrono::high_resolution_clock::now();
        auto parallel_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  记录总数: " << parallel_result.value << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << parallel_duration.count() << " ms" << std::endl;
        
        // 计算加速比
        if (parallel_duration.count() > 0) {
            double speedup = sequential_duration.count() / parallel_duration.count();
            std::cout << "  加速比: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        }
    }
    
    void testGroupByQueries(int num_threads) {
        std::cout << "\n=== GROUP BY 查询测试 (并行版本) ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // 测试基本的GROUP BY查询
        std::cout << "\n1. 测试基本 GROUP BY 查询:" << std::endl;
        
        // 测试并行GROUP BY user COUNT(*) 查询
        std::cout << "测试并行 GROUP BY user COUNT(*) 查询:" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeComplexQueryMultiBlockParallel(
            "COUNT(*) GROUP BY user", blocks_, num_threads);  
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
        std::cout << "测试并行 GROUP BY user, pid COUNT(*) 查询:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlockParallel(
            "COUNT(*) GROUP BY user, pid", blocks_, num_threads);  
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
        std::cout << "测试并行 user:postgres GROUP BY pid COUNT(*) 查询:" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        result = engine_.executeComplexQueryMultiBlockParallel(
            "user:postgres GROUP BY pid COUNT(*)", blocks_, num_threads);  
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
        
        // 性能对比测试：顺序 vs 并行
        std::cout << "\n=== 性能对比测试 ===" << std::endl;
        
        // 顺序执行
        std::cout << "\n顺序执行 (GROUP BY user COUNT(*)):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sequential_result = engine_.executeComplexQueryMultiBlock(
            "COUNT(*) GROUP BY user", blocks_);
        end = std::chrono::high_resolution_clock::now();
        auto sequential_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  分组数: " << sequential_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << sequential_duration.count() << " ms" << std::endl;
        
        // 并行执行
        std::cout << "\n并行执行 (GROUP BY user COUNT(*), 多线程):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto parallel_result = engine_.executeComplexQueryMultiBlockParallel(
            "COUNT(*) GROUP BY user", blocks_, num_threads);
        end = std::chrono::high_resolution_clock::now();
        auto parallel_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  分组数: " << parallel_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << parallel_duration.count() << " ms" << std::endl;
        
        // 计算加速比
        if (parallel_duration.count() > 0) {
            double speedup = sequential_duration.count() / parallel_duration.count();
            std::cout << "  加速比: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        }
    }
    
    void testDirectGroupedAggregateQuery(int num_threads) {
        std::cout << "\n=== 分组聚合查询测试 (并行版本) ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // 直接测试executeGroupedAggregateQuery方法
        std::cout << "\n1. 测试分组聚合查询:" << std::endl;
        
        std::vector<AggregateFunction> agg_funcs = {AggregateFunction::COUNT};
        std::vector<std::string> agg_fields = {""}; // COUNT(*) uses empty field name
        std::vector<std::string> group_fields = {"pid"};
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeGroupedAggregateQueryMultiBlockParallel(
            agg_funcs, agg_fields, group_fields, blocks_, num_threads);  
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
        
        // 性能对比测试：顺序 vs 并行
        std::cout << "\n=== 性能对比测试 ===" << std::endl;
        
        // 顺序执行
        std::cout << "\n顺序执行 (分组聚合查询):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sequential_result = engine_.executeGroupedAggregateQueryMultiBlock(
            agg_funcs, agg_fields, group_fields, blocks_);
        end = std::chrono::high_resolution_clock::now();
        auto sequential_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  总记录数: " << sequential_result.total_count << std::endl;
        std::cout << "  分组数: " << sequential_result.groups_count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << sequential_duration.count() << " ms" << std::endl;
        
        // 并行执行
        std::cout << "\n并行执行 (分组聚合查询, 多线程):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto parallel_result = engine_.executeGroupedAggregateQueryMultiBlockParallel(
            agg_funcs, agg_fields, group_fields, blocks_, num_threads);
        end = std::chrono::high_resolution_clock::now();
        auto parallel_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  总记录数: " << parallel_result.total_count << std::endl;
        std::cout << "  分组数: " << parallel_result.groups_count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << parallel_duration.count() << " ms" << std::endl;
        
        // 计算加速比
        if (parallel_duration.count() > 0) {
            double speedup = sequential_duration.count() / parallel_duration.count();
            std::cout << "  加速比: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        }
    }

    void testComplexQueries(int num_threads) {
        std::cout << "\n=== 复杂查询测试 (并行版本) ===" << std::endl;
        
        if (blocks_.empty()) {
            std::cout << "没有可用的压缩块数据" << std::endl;
            return;
        }
        
        // 测试字段存在性查询 (这是当前实现中最可靠的查询)
        std::cout << "\n1. 测试字段存在性查询:" << std::endl;
        std::cout << "测试字段 'user' 存在:" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();
        auto result = engine_.executeComplexQueryMultiBlockParallel(
            "user:*", blocks_, num_threads);  
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
        result = engine_.executeComplexQueryMultiBlockParallel(
            "user:postgres", blocks_, num_threads);  
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
        result = engine_.executeComplexQueryMultiBlockParallel(
            "dbname:example", blocks_, num_threads);  
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
        result = engine_.executeComplexQueryMultiBlockParallel(
            "user:postgres AND dbname:example", blocks_, num_threads);  
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
        result = engine_.executeComplexQueryMultiBlockParallel(
            "user:postgres OR user:alice", blocks_, num_threads);  
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
        result = engine_.executeComplexQueryMultiBlockParallel(
            "(user:postgres OR user:alice) AND dbname:example", blocks_, num_threads);  
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
        result = engine_.executeComplexQueryMultiBlockParallel(
            "NOT user:postgres", blocks_, num_threads);  
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
        
        // 性能对比测试：顺序 vs 并行
        std::cout << "\n=== 性能对比测试 ===" << std::endl;
        
        // 顺序执行
        std::cout << "\n顺序执行 (user:postgres AND dbname:example):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto sequential_result = engine_.executeComplexQueryMultiBlock(
            "user:postgres AND dbname:example", blocks_);
        end = std::chrono::high_resolution_clock::now();
        auto sequential_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << sequential_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << sequential_duration.count() << " ms" << std::endl;
        
        // 并行执行
        std::cout << "\n并行执行 (user:postgres AND dbname:example, 多线程):" << std::endl;
        start = std::chrono::high_resolution_clock::now();
        auto parallel_result = engine_.executeComplexQueryMultiBlockParallel(
            "user:postgres AND dbname:example", blocks_, num_threads);
        end = std::chrono::high_resolution_clock::now();
        auto parallel_duration = std::chrono::duration<double, std::milli>(end - start);
        
        std::cout << "  找到记录数: " << parallel_result.count << std::endl;
        std::cout << "  查询时间: " << std::fixed << std::setprecision(2) 
                  << parallel_duration.count() << " ms" << std::endl;
        
        // 计算加速比
        if (parallel_duration.count() > 0) {
            double speedup = sequential_duration.count() / parallel_duration.count();
            std::cout << "  加速比: " << std::fixed << std::setprecision(2) << speedup << "x" << std::endl;
        }
    }
};

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  -d, --data-dir DIRECTORY    Directory containing compressed data (default: postgresql)\n";
    std::cout << "  -t, --threads COUNT         Number of threads to use (default: 6)\n";
    std::cout << "  -h, --help                  Show this help message\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << " --data-dir compressed_data --threads 8\n";
    std::cout << "  " << program_name << " -d test_data -t 4\n";
    std::cout << "  " << program_name << " --data-dir /path/to/data\n";
    std::cout << "  " << program_name << " -t 12\n";
}

int main(int argc, char* argv[]) {
    std::string compressed_data_dir = "postgresql";
    int num_threads = 6;
    
    // Parse command line arguments
    static struct option long_options[] = {
        {"data-dir", required_argument, 0, 'd'},
        {"threads", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "d:t:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'd':
                compressed_data_dir = optarg;
                break;
            case 't':
                try {
                    num_threads = std::stoi(optarg);
                    if (num_threads <= 0) {
                        std::cerr << "Error: Thread count must be positive\n";
                        return 1;
                    }
                } catch (const std::exception&) {
                    std::cerr << "Error: Invalid thread count: " << optarg << "\n";
                    return 1;
                }
                break;
            case 'h':
                printUsage(argv[0]);
                return 0;
            case '?':
                // getopt_long already printed an error message
                return 1;
            default:
                std::cerr << "Unknown option\n";
                return 1;
        }
    }
    
    std::cout << "并行查询测试" << std::endl;
    std::cout << "压缩数据目录: " << compressed_data_dir << std::endl;
    std::cout << "线程数: " << num_threads << std::endl;
    
    if (!std::filesystem::exists(compressed_data_dir)) {
        std::cerr << "错误: 压缩数据目录不存在: " << compressed_data_dir << std::endl;
        return 1;
    }
    
    ParallelQueryTest tester;
    
    // Set the data directory for the query engine
    // This is needed for granular data extraction from chunk directories
    tester.setDataDirectory(compressed_data_dir);
    
    // 加载压缩数据
    if (!tester.loadCompressedData(compressed_data_dir)) {
        std::cerr << "加载压缩数据失败" << std::endl;
        return 1;
    }
    
    // 运行测试
    // tester.testFieldExistenceQueries();
    // tester.testDictionaryQueries();
    // tester.testExactMatchQueries(num_threads);  // 添加精确匹配查询测试
    // tester.testRangeQueries(num_threads);       // 添加范围查询测试
    tester.testComplexQueries(num_threads);     // 添加复杂查询测试
    tester.testAggregateQueries(num_threads);   // 添加聚合查询测试
    tester.testDirectGroupedAggregateQuery(num_threads); // 添加直接分组聚合查询测试

    std::cout << "\n并行测试完成！" << std::endl;
    return 0;
}