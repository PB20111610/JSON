#include "compress_type_aware.h"
#include "compress.h"
#include "loudsTotrie.h"
#include "compression/factory/compression_factory.h"
#include "compression/backends/zstd_backend.h"
#include "compression/algorithms/rle_compression.h"
#include "compression/core/compression_utils.h"
#include "compression/type_aware/type_aware_compressor.h"
#include <sstream>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace json2 {

// ========== Helper functions for reading/writing data (in anonymous namespace to avoid linkage issues) ==========

namespace {
    template<typename T>
    T readValue(const std::vector<uint8_t>& data, size_t& pos) {
        T value;
        std::memcpy(&value, &data[pos], sizeof(T));
        pos += sizeof(T);
        return value;
    }

    std::string readString(const std::vector<uint8_t>& data, size_t& pos) {
        uint32_t length = readValue<uint32_t>(data, pos);
        std::string str(reinterpret_cast<const char*>(&data[pos]), length);
        pos += length;
        return str;
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
}

// ========== Systematic Helper Functions Following compress.cpp Patterns ==========

// Helper function to validate configuration parameters
static void validateTypeAwareConfig(const compression::TypeAwareCompressionConfig& config) {
    // Validate that compression levels are within acceptable ranges
    if (config.compression_level < 0 || config.compression_level > 22) {
        throw std::runtime_error("TypeAware: Invalid compression level " + std::to_string(config.compression_level) + " (must be 0-22)");
    }
    
    // Basic validation - ensure backends are recognized
    auto louds_backend = static_cast<int>(config.louds_backend);
    auto dict_backend = static_cast<int>(config.dictionary_backend);
    auto meta_backend = static_cast<int>(config.metadata_backend);
    
    if (louds_backend < 0 || louds_backend > 20) {
        throw std::runtime_error("TypeAware: Invalid LOUDS backend in configuration");
    }
    if (dict_backend < 0 || dict_backend > 20) {
        throw std::runtime_error("TypeAware: Invalid dictionary backend in configuration");
    }
    if (meta_backend < 0 || meta_backend > 20) {
        throw std::runtime_error("TypeAware: Invalid metadata backend in configuration");
    }
}

// Helper function to validate compression effectiveness for type-aware compression
static bool validateTypeAwareCompressionEffectiveness(size_t original_size, size_t compressed_size, double threshold = 0.9) {
    if (original_size == 0) return true;
    double ratio = static_cast<double>(compressed_size) / original_size;
    return ratio < threshold;
}

// Helper function to safely create compressor from factory
static std::unique_ptr<compression::ICompression> createCompressorSafely(compression::CompressionBackend backend) {
    try {
        auto compressor = compression::factory::CompressionFactory::createCompressor(backend);
        if (!compressor) {
            throw std::runtime_error("Factory returned null compressor for backend " + std::to_string(static_cast<int>(backend)));
        }
        return compressor;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to create compressor: " + std::string(e.what()));
    }
}

// ========== Enhanced Configuration-Driven Compression Interface ==========

// Enhanced configuration-driven compression with validation
std::vector<uint8_t> compressWithConfig(const std::vector<uint8_t>& data, 
                                       compression::FieldType field_type,
                                       const compression::TypeAwareCompressionConfig& config) {
    if (data.empty()) return {};
    
    try {
        validateTypeAwareConfig(config);
        
        // Select backend based on field type and configuration
        compression::CompressionBackend backend;
        
        switch (field_type) {
            case compression::FieldType::BOOL:
                backend = config.layer_config.bool_backend;
                break;
            case compression::FieldType::STRING:
                backend = config.layer_config.string_backend;
                break;
            case compression::FieldType::TIMESTAMP:
                backend = config.layer_config.timestamp_backend;
                break;
            case compression::FieldType::LOGTYPE:
                backend = config.layer_config.logtype_backend;
                break;
            case compression::FieldType::ARRAY:
                backend = config.layer_config.array_backend;
                break;
            case compression::FieldType::NULL_TYPE:
                backend = config.layer_config.null_backend;
                break;
            case compression::FieldType::INT64:
            case compression::FieldType::UINT32:
                backend = config.layer_config.int_backend;
                break;
            case compression::FieldType::DOUBLE:
                backend = config.layer_config.double_backend;
                break;
            default:
                backend = config.fallback_backend;
        }
        
        auto compressor = createCompressorSafely(backend);
        std::vector<uint8_t> compressed = compressor->compress(data);
        
        // Add header: [compression_used_flag][backend_id][data]
        std::vector<uint8_t> result;
        
        // Validate compression effectiveness
        if (!validateTypeAwareCompressionEffectiveness(data.size(), compressed.size())) {
            // Header: 0 = uncompressed
            result.push_back(0);
            result.insert(result.end(), data.begin(), data.end());
        } else {
            // Header: 1 = compressed, followed by backend ID
            result.push_back(1);
            result.push_back(static_cast<uint8_t>(backend));
            result.insert(result.end(), compressed.begin(), compressed.end());
        }
        
        return result;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware compression failed: " + std::string(e.what()));
    }
}

// Enhanced configuration-driven decompression with validation
std::vector<uint8_t> decompressWithConfig(const std::vector<uint8_t>& compressed_data,
                                         compression::FieldType field_type,
                                         const compression::TypeAwareCompressionConfig& config) {
    if (compressed_data.empty()) return {};
    
    try {
        validateTypeAwareConfig(config);
        
        // Read header to determine if data was compressed
        if (compressed_data.size() < 1) {
            throw std::runtime_error("TypeAware: Insufficient data for header");
        }
        
        uint8_t compression_flag = compressed_data[0];
        
        if (compression_flag == 0) {
            // Data was not compressed, return as-is (skip header)
            return std::vector<uint8_t>(compressed_data.begin() + 1, compressed_data.end());
        } else if (compression_flag == 1) {
            // Data was compressed, decompress using stored backend
            if (compressed_data.size() < 2) {
                throw std::runtime_error("TypeAware: Insufficient data for compressed header");
            }
            
            compression::CompressionBackend backend = static_cast<compression::CompressionBackend>(compressed_data[1]);
            
            auto compressor = createCompressorSafely(backend);
            std::vector<uint8_t> compressed_payload(compressed_data.begin() + 2, compressed_data.end());
            std::vector<uint8_t> result = compressor->decompress(compressed_payload);
            
            return result;
        } else {
            throw std::runtime_error("TypeAware: Invalid compression flag: " + std::to_string(compression_flag));
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced Core Compression Methods Following compress.cpp Patterns ==========

CompressedData TypeAwareCompressor::compress(const Trie& trie, const FieldDictionaryManager& manager, 
                                           const compression::TypeAwareCompressionConfig& config) {
    try {
        validateTypeAwareConfig(config);
        
        // Build LOUDS trie and delegate to configuration-driven LOUDS compression
        LOUDSTrie louds(trie.getOrderedFields());
        louds.buildFromTrie(trie);
        
        // Use dynamically expanded field order from LOUDS
        auto expanded_field_order = louds.getFieldOrder();
        
        return compressLouds(louds, manager, expanded_field_order, config);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware trie compression failed: " + std::string(e.what()));
    }
}

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>>
TypeAwareCompressor::decompress(const CompressedData& compressed_data, 
                               const compression::TypeAwareCompressionConfig& config) {
    try {
        validateTypeAwareConfig(config);
        
        // Delegate to configuration-driven LOUDS decompression and convert to Trie
        auto [louds, manager] = decompressLouds(compressed_data, config);
        auto trie = std::make_unique<Trie>(louds->getFieldOrder());
        loudsToTrie(*louds, *trie);
        return {std::move(trie), std::move(manager)};
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware trie decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced LOUDS Compression with Systematic Error Handling ==========

CompressedData TypeAwareCompressor::compressLouds(const LOUDSTrie& louds, 
                                                 const FieldDictionaryManager& manager,
                                                 const std::vector<FieldKey>& field_order,
                                                 const compression::TypeAwareCompressionConfig& config) {
    try {
        // Starting LOUDS compression with type-aware configuration
        
        validateTypeAwareConfig(config);
        
        CompressedData result;
        
        // 1. LOUDS bitmap compression using configured BOOL algorithm
        // Step 1: Serialize LOUDS bitmap
        std::ostringstream bv_stream(std::ios::binary);
        louds.serializeBitmap(bv_stream);
        std::string bv_str = bv_stream.str();
        
        if (bv_str.empty()) {
            throw std::runtime_error("TypeAware: Empty LOUDS bitmap serialization");
        }
        
        // LOUDS bitmap serialization completed
        
        result.trie_data = compressWithConfig(std::vector<uint8_t>(bv_str.begin(), bv_str.end()), 
                                              compression::FieldType::BOOL, config);
        
        // LOUDS bitmap compression completed

        // 2. Layer data compression using type-specific algorithms
        size_t layer_count = louds.getLayeredStorage().getLayerCount();
        std::vector<std::vector<uint8_t>> compressed_layers;
        std::vector<uint32_t> layer_sizes;
        size_t layers_raw_total = 0;
        
        for (size_t i = 0; i < layer_count; ++i) {
            std::ostringstream layer_stream(std::ios::binary);
            louds.getLayeredStorage().serializeLayer(i, layer_stream);
            std::string layer_str = layer_stream.str();
            layers_raw_total += layer_str.size();
            
            // Map field type for configuration-driven compression
            compression::FieldType field_type = field_order[i].type;

            auto compressed = compressWithConfig(std::vector<uint8_t>(layer_str.begin(), layer_str.end()),
                                               field_type, config);
            layer_sizes.push_back(static_cast<uint32_t>(compressed.size()));
            compressed_layers.push_back(std::move(compressed));
        }
        
        // Serialize layer data following compress.cpp structure: [count][sizes][data]
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

        // 3. Dictionary compression using configured STRING algorithm
        std::vector<uint8_t> dict_data = Compressor::serializeDictionary(manager);
        if (dict_data.empty()) {
            throw std::runtime_error("TypeAware: Empty dictionary serialization");
        }
        result.dictionary_data = compressWithConfig(dict_data, compression::FieldType::STRING, config);

        // 4. Metadata compression using configured STRING algorithm
        std::vector<uint8_t> meta_data = Compressor::serializeMetadata(field_order, manager);
        if (meta_data.empty()) {
            throw std::runtime_error("TypeAware: Empty metadata serialization");
        }
        result.metadata_data = compressWithConfig(meta_data, compression::FieldType::STRING, config);

        // 5. Calculate sizes with validation
        result.original_size = bv_str.size() + layers_raw_total + dict_data.size() + meta_data.size();
        result.compressed_size = result.trie_data.size() + result.layer_data.size() + 
                                 result.dictionary_data.size() + result.metadata_data.size();
        
        if (result.original_size == 0) {
            throw std::runtime_error("TypeAware: Invalid original size calculation");
        }
        
        return result;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware LOUDS compression failed: " + std::string(e.what()));
    }
}

std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
TypeAwareCompressor::decompressLouds(const CompressedData& compressed_data, 
                                    const compression::TypeAwareCompressionConfig& config) {
    try {
        // Starting LOUDS decompression with type-aware configuration
        
        validateTypeAwareConfig(config);
        
        // 1. Enhanced LOUDS bitmap decompression with validation
        // Step 1: Decompress LOUDS bitmap
        
        if (compressed_data.trie_data.empty()) {
            throw std::runtime_error("TypeAware: Empty LOUDS trie data for decompression");
        }
        
        // Validating trie data size
        
        std::vector<uint8_t> trie_data = decompressWithConfig(compressed_data.trie_data, 
                                                             compression::FieldType::BOOL, config);
        
        // LOUDS bitmap decompression completed
        if (trie_data.empty()) {
            throw std::runtime_error("TypeAware: Decompressed LOUDS trie data is empty");
        }
        
        std::istringstream bv_stream(std::string(trie_data.begin(), trie_data.end()), std::ios::binary);
        auto louds = std::make_unique<LOUDSTrie>();
        louds->deserializeBitmap(bv_stream);

        // 2. Extract field order from metadata first, so we can use correct field types for layer decompression
        if (compressed_data.metadata_data.empty()) {
            throw std::runtime_error("TypeAware: Empty metadata for decompression");
        }
        
        std::vector<uint8_t> meta_data = decompressWithConfig(compressed_data.metadata_data, 
                                                             compression::FieldType::STRING, config);
        if (meta_data.empty()) {
            throw std::runtime_error("TypeAware: Decompressed metadata is empty");
        }
        
        std::vector<FieldKey> ordered_field_keys = Compressor::deserializeMetadata(meta_data);
        if (ordered_field_keys.empty()) {
            throw std::runtime_error("TypeAware: No field keys recovered from metadata");
        }
        
        louds->setFieldOrder(ordered_field_keys);

        // 3. Enhanced layer data decompression with systematic validation
        if (compressed_data.layer_data.empty()) {
            throw std::runtime_error("TypeAware: Empty layer data for decompression");
        }
        
        const std::vector<uint8_t>& layer_data = compressed_data.layer_data;
        size_t offset = 0;
        
        if (layer_data.size() < sizeof(uint32_t)) {
            throw std::runtime_error("TypeAware: Insufficient layer data for count");
        }
        
        uint32_t layer_count = 0;
        std::memcpy(&layer_count, &layer_data[offset], sizeof(layer_count));
        offset += sizeof(layer_count);
        
        if (layer_data.size() < offset + layer_count * sizeof(uint32_t)) {
            throw std::runtime_error("TypeAware: Insufficient data for layer sizes");
        }
        
        std::vector<uint32_t> layer_sizes(layer_count);
        for (uint32_t i = 0; i < layer_count; ++i) {
            std::memcpy(&layer_sizes[i], &layer_data[offset], sizeof(uint32_t));
            offset += sizeof(uint32_t);
        }
        
        // Initialize layered storage structure with validation
        for (uint32_t i = 0; i < layer_count; ++i) {
            if (i >= louds->getLayeredStorage().getLayerCount()) {
                // Use temporary field names and default types
                FieldKey temp_fk{"temp_field_" + std::to_string(i), json2::compression::FieldType::STRING};
                louds->getLayeredStorage().addLayer(temp_fk, 0);
            }
        }
        
        // Process each layer with enhanced error handling
        for (uint32_t i = 0; i < layer_count; ++i) {
            if (offset + layer_sizes[i] > layer_data.size()) {
                throw std::runtime_error("TypeAware: Layer " + std::to_string(i) + " size exceeds available data");
            }
            
            std::vector<uint8_t> compressed_layer(layer_data.begin() + offset, 
                                                 layer_data.begin() + offset + layer_sizes[i]);
            offset += layer_sizes[i];
            
            // Use field type-specific decompression based on field order
            compression::FieldType field_type = compression::FieldType::STRING; // Default
            if (i < ordered_field_keys.size()) {
                field_type = ordered_field_keys[i].type;
            }
            
            std::vector<uint8_t> layer_raw = decompressWithConfig(compressed_layer, field_type, config);
            if (layer_raw.empty()) {
                throw std::runtime_error("TypeAware: Empty decompressed layer " + std::to_string(i));
            }
            
            std::istringstream layer_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
            louds->getLayeredStorage().deserializeLayer(i, layer_stream);
        }

        // 4. Enhanced dictionary decompression with validation
        if (compressed_data.dictionary_data.empty()) {
            throw std::runtime_error("TypeAware: Empty dictionary data for decompression");
        }
        
        std::vector<uint8_t> dict_data = decompressWithConfig(compressed_data.dictionary_data, 
                                                             compression::FieldType::STRING, config);
        if (dict_data.empty()) {
            throw std::runtime_error("TypeAware: Decompressed dictionary data is empty");
        }
        
        auto manager = Compressor::deserializeDictionary(dict_data);
        if (!manager) {
            throw std::runtime_error("TypeAware: Failed to deserialize dictionary manager");
        }

        return {std::move(louds), std::move(manager)};
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware LOUDS decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced Statistics and Utility Methods Following compress.cpp Patterns ==========

void TypeAwareCompressor::printCompressionStats(const compression::TypeAwareCompressionConfig& config) {
    try {
        validateTypeAwareConfig(config);
        
        std::cout << "\n=== Type-Aware Compression Statistics ==="
                  << "\nConfiguration-based compression enabled"
                  << "\nLOUDS Backend: " << static_cast<int>(config.louds_backend)
                  << "\nDictionary Backend: " << static_cast<int>(config.dictionary_backend)
                  << "\nMetadata Backend: " << static_cast<int>(config.metadata_backend)
                  << "\nCompression Level: " << config.compression_level
                  << "\n=========================================" << std::endl;
                  
    } catch (const std::exception& e) {
        std::cerr << "Error printing compression stats: " << e.what() << std::endl;
    }
}

void TypeAwareCompressor::clearCompressionStats(const compression::TypeAwareCompressionConfig& config) {
    try {
        validateTypeAwareConfig(config);
        std::cout << "TypeAware compression statistics cleared successfully." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error clearing compression stats: " << e.what() << std::endl;
    }
}

// Enhanced compression ratio calculation with validation
double TypeAwareCompressor::getCompressionRatio(const CompressedData& compressed_data) {
    try {
        if (compressed_data.original_size == 0) {
            return 0.0;
        }
        
        if (compressed_data.compressed_size > compressed_data.original_size * 2) {
            // Suspicious compression ratio - might indicate an error
            std::cerr << "Warning: Suspicious compression ratio detected" << std::endl;
        }
        
        return static_cast<double>(compressed_data.compressed_size) / compressed_data.original_size;
        
    } catch (const std::exception& e) {
        std::cerr << "Error calculating compression ratio: " << e.what() << std::endl;
        return 1.0; // Conservative fallback
    }
}

// ========== 细粒度类型敏感压缩实现 ==========

GranularCompressedData TypeAwareCompressor::compressGranular(const Trie& trie, const FieldDictionaryManager& manager, 
                                                           const compression::TypeAwareCompressionConfig& config, 
                                                           bool use_layer_separation) {
    try {
        validateTypeAwareConfig(config);
        
        // Build LOUDS trie and delegate to configuration-driven LOUDS compression
        LOUDSTrie louds(trie.getOrderedFields());
        louds.buildFromTrie(trie);
        
        // Use dynamically expanded field order from LOUDS
        auto expanded_field_order = louds.getFieldOrder();
        
        return compressGranularLouds(louds, manager, expanded_field_order, config, use_layer_separation);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware granular trie compression failed: " + std::string(e.what()));
    }
}

GranularCompressedData TypeAwareCompressor::compressGranularLouds(const LOUDSTrie& louds, 
                                                                 const FieldDictionaryManager& manager,
                                                                 const std::vector<FieldKey>& field_order,
                                                                 const compression::TypeAwareCompressionConfig& config,
                                                                 bool use_layer_separation) {
    try {
        validateTypeAwareConfig(config);
        
        GranularCompressedData result;
        result.use_layer_separation = use_layer_separation;
        
        // 1. 分别序列化各类字典（使用类型敏感压缩）
        std::vector<uint8_t> string_dict_raw = Compressor::serializeStringDictionary(manager);
        std::vector<uint8_t> timestamp_dict_raw = Compressor::serializeTimestampDictionary(manager);
        std::vector<uint8_t> logtype_dict_raw = Compressor::serializeLogTypeDictionary(manager);
        
        // 2. 序列化LOUDS Trie位图（使用BOOL类型压缩）
        std::ostringstream bv_stream(std::ios::binary);
        louds.serializeBitmap(bv_stream);
        std::string bv_str = bv_stream.str();
        std::vector<uint8_t> trie_raw(bv_str.begin(), bv_str.end());
        
        if (trie_raw.empty()) {
            throw std::runtime_error("TypeAware Granular: Empty LOUDS bitmap serialization");
        }
        
        // 3. 序列化分层内容和层大小信息（使用类型敏感压缩）
        std::vector<uint8_t> layer_raw;
        std::vector<uint8_t> layer_sizes_raw;
        if (use_layer_separation) {
            // 按层分别序列化和压缩（使用字段类型特定的压缩算法）
            size_t layer_count = louds.getLayeredStorage().getLayerCount();
            result.layer_data_by_level.reserve(layer_count);
            
            // 序列化层大小信息
            std::vector<uint32_t> layer_sizes;
            std::vector<std::vector<uint8_t>> layer_data_by_level(layer_count);
            
            // 首先序列化所有层数据
            for (size_t i = 0; i < layer_count; ++i) {
                std::ostringstream layer_stream(std::ios::binary);
                louds.getLayeredStorage().serializeLayer(i, layer_stream);
                std::string layer_str = layer_stream.str();
                layer_data_by_level[i] = std::vector<uint8_t>(layer_str.begin(), layer_str.end());
                
                // 存储层中实际的元素数量，而不是序列化后的字节大小
                size_t layer_element_count = louds.getLayeredStorage().getLayer(i).size();
                layer_sizes.push_back(static_cast<uint32_t>(layer_element_count));
                
                // 根据字段类型选择压缩算法
                compression::FieldType field_type = field_order[i].type;

                std::vector<uint8_t> compressed_layer = compressWithConfig(layer_data_by_level[i], field_type, config);
                result.layer_data_by_level.push_back(std::move(compressed_layer));
                
                // 累计原始大小
                layer_raw.insert(layer_raw.end(), layer_data_by_level[i].begin(), layer_data_by_level[i].end());
            }
            
            // 序列化层大小信息
            layer_sizes_raw.resize(layer_sizes.size() * sizeof(uint32_t));
            std::memcpy(layer_sizes_raw.data(), layer_sizes.data(), layer_sizes.size() * sizeof(uint32_t));
        } else {
            // 整体序列化和压缩（使用STRING类型压缩）
            // 使用与传统版本相同的序列化方式
            std::ostringstream all_layers_stream(std::ios::binary);
            louds.getLayeredStorage().serialize(all_layers_stream);
            std::string all_layers_str = all_layers_stream.str();
            layer_raw.assign(all_layers_str.begin(), all_layers_str.end());
            result.layer_data_combined = compressWithConfig(layer_raw, compression::FieldType::STRING, config);
        }
        
        // 4. 序列化元数据（使用STRING类型压缩）
        std::vector<uint8_t> metadata_raw = Compressor::serializeMetadata(field_order, manager);
        
        // 5. 分别压缩各组件（使用类型敏感压缩）
        result.trie_bitmap = compressWithConfig(trie_raw, compression::FieldType::BOOL, config);
        result.string_dict = compressWithConfig(string_dict_raw, compression::FieldType::STRING, config);
        result.timestamp_dict = compressWithConfig(timestamp_dict_raw, compression::FieldType::TIMESTAMP, config);
        result.logtype_dict = compressWithConfig(logtype_dict_raw, compression::FieldType::LOGTYPE, config);
        result.metadata = compressWithConfig(metadata_raw, compression::FieldType::STRING, config);
        
        // 压缩层大小信息（如果使用分层压缩）
        if (use_layer_separation) {
            result.layer_sizes = compressWithConfig(layer_sizes_raw, compression::FieldType::STRING, config);
        }
        
        // 6. 记录原始大小
        result.trie_original_size = trie_raw.size();
        result.string_dict_original_size = string_dict_raw.size();
        result.timestamp_dict_original_size = timestamp_dict_raw.size();
        result.logtype_dict_original_size = logtype_dict_raw.size();
        result.layer_original_size = layer_raw.size();
        result.layer_sizes_original_size = layer_sizes_raw.size();
        result.metadata_original_size = metadata_raw.size();
        
        result.original_size = result.trie_original_size + result.string_dict_original_size + 
                              result.timestamp_dict_original_size + result.logtype_dict_original_size + 
                              result.layer_original_size + result.metadata_original_size + result.layer_sizes_original_size;
        
        // 7. 计算压缩后总大小
        result.compressed_size = result.trie_bitmap.size() + result.string_dict.size() + 
                                result.timestamp_dict.size() + result.logtype_dict.size() + 
                                result.metadata.size();
        
        if (use_layer_separation) {
            for (const auto& layer : result.layer_data_by_level) {
                result.compressed_size += layer.size();
            }
            result.compressed_size += result.layer_sizes.size(); // 添加层大小信息的压缩大小
        } else {
            result.compressed_size += result.layer_data_combined.size();
        }
        
        if (result.original_size == 0) {
            throw std::runtime_error("TypeAware Granular: Invalid original size calculation");
        }
        
        return result;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware granular LOUDS compression failed: " + std::string(e.what()));
    }
}

double TypeAwareCompressor::getGranularCompressionRatio(const GranularCompressedData& compressed_data) {
    try {
        if (compressed_data.compressed_size == 0) return 0.0;
        
        double ratio = static_cast<double>(compressed_data.compressed_size) / compressed_data.original_size;
        
        if (ratio > 2.0) {
            // Suspicious compression ratio - might indicate an error
            std::cerr << "Warning: Suspicious granular compression ratio detected: " << ratio << std::endl;
        }
        
        return static_cast<double>(compressed_data.original_size) / compressed_data.compressed_size;
        
    } catch (const std::exception& e) {
        std::cerr << "Error calculating granular compression ratio: " << e.what() << std::endl;
        return 1.0; // Conservative fallback
    }
}

// ========== 细粒度类型敏感解压缩实现 ==========

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
TypeAwareCompressor::decompressGranular(const GranularCompressedData& compressed_data, 
                                       const compression::TypeAwareCompressionConfig& config) {
    try {
        validateTypeAwareConfig(config);
        
        // Delegate to configuration-driven LOUDS decompression and convert to Trie
        auto [louds, manager] = decompressGranularLouds(compressed_data, config);
        auto trie = std::make_unique<Trie>(louds->getFieldOrder());
        loudsToTrie(*louds, *trie);
        return {std::move(trie), std::move(manager)};
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware granular trie decompression failed: " + std::string(e.what()));
    }
}

std::pair<std::unique_ptr<LOUDSTrie>, std::unique_ptr<FieldDictionaryManager>> 
TypeAwareCompressor::decompressGranularLouds(const GranularCompressedData& compressed_data, 
                                            const compression::TypeAwareCompressionConfig& config) {
    try {
        validateTypeAwareConfig(config);
        
        // 1. 解压缩元数据（使用STRING类型解压缩）
        if (compressed_data.metadata.empty()) {
            throw std::runtime_error("TypeAware Granular: Empty metadata for decompression");
        }
        
        std::vector<uint8_t> metadata_raw = decompressWithConfig(compressed_data.metadata, 
                                                               compression::FieldType::STRING, config);
        if (metadata_raw.empty()) {
            throw std::runtime_error("TypeAware Granular: Decompressed metadata is empty");
        }
        
        std::vector<FieldKey> field_order = Compressor::deserializeMetadata(metadata_raw);
        if (field_order.empty()) {
            throw std::runtime_error("TypeAware Granular: No field keys recovered from metadata");
        }
        
        // 2. 重建字典管理器（使用类型敏感解压缩）
        auto manager = std::make_unique<FieldDictionaryManager>();
        
        // 分别解压缩和反序列化各类字典
        if (!compressed_data.string_dict.empty()) {
            std::vector<uint8_t> string_dict_raw = decompressWithConfig(compressed_data.string_dict, 
                                                                       compression::FieldType::STRING, config);
            if (!string_dict_raw.empty()) {
                Compressor::deserializeStringDictionary(string_dict_raw, *manager);
            }
        }
        
        if (!compressed_data.timestamp_dict.empty()) {
            std::vector<uint8_t> timestamp_dict_raw = decompressWithConfig(compressed_data.timestamp_dict, 
                                                                          compression::FieldType::TIMESTAMP, config);
            if (!timestamp_dict_raw.empty()) {
                Compressor::deserializeTimestampDictionary(timestamp_dict_raw, *manager);
            }
        }
        
        if (!compressed_data.logtype_dict.empty()) {
            std::vector<uint8_t> logtype_dict_raw = decompressWithConfig(compressed_data.logtype_dict, 
                                                                        compression::FieldType::LOGTYPE, config);
            if (!logtype_dict_raw.empty()) {
                Compressor::deserializeLogTypeDictionary(logtype_dict_raw, *manager);
            }
        }
        
        // 3. 重建LOUDS Trie结构（使用BOOL类型解压缩）
        if (compressed_data.trie_bitmap.empty()) {
            throw std::runtime_error("TypeAware Granular: Empty LOUDS trie data for decompression");
        }
        
        std::vector<uint8_t> trie_raw = decompressWithConfig(compressed_data.trie_bitmap, 
                                                           compression::FieldType::BOOL, config);
        if (trie_raw.empty()) {
            throw std::runtime_error("TypeAware Granular: Decompressed LOUDS trie data is empty");
        }
        
        auto louds = Compressor::deserializeLoudsTrie(trie_raw, *manager, field_order);
        
        // 4. 重建分层内容（使用类型敏感解压缩）
        if (compressed_data.use_layer_separation) {
            // 确保layers_向量有足够的空间来容纳所有层
            auto& layered_storage = louds->getLayeredStorage();
            size_t required_layers = compressed_data.layer_data_by_level.size();
            while (layered_storage.getLayerCount() < required_layers) {
                layered_storage.addLayer(FieldKey{}, 0);
            }
            
            // 解压缩层大小信息（如果存在）
            std::vector<uint32_t> layer_sizes;
            if (!compressed_data.layer_sizes.empty()) {
                std::vector<uint8_t> layer_sizes_raw = decompressWithConfig(compressed_data.layer_sizes, 
                                                                           compression::FieldType::STRING, config);
                if (layer_sizes_raw.size() >= sizeof(uint32_t)) {
                    size_t layer_count = layer_sizes_raw.size() / sizeof(uint32_t);
                    layer_sizes.resize(layer_count);
                    std::memcpy(layer_sizes.data(), layer_sizes_raw.data(), layer_count * sizeof(uint32_t));
                }
            }
            
            // 按指定层解压缩（使用字段类型特定的解压缩算法）
            for (size_t i = 0; i < compressed_data.layer_data_by_level.size(); ++i) {
                // 根据字段类型选择解压缩算法
                compression::FieldType field_type = compression::FieldType::STRING; // Default
                if (i < field_order.size()) {
                    field_type = field_order[i].type;
                }

                std::vector<uint8_t> layer_data = decompressWithConfig(compressed_data.layer_data_by_level[i], 
                                                                      field_type, config);
                if (!layer_data.empty()) {
                    std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
                    layered_storage.deserializeLayer(i, layer_stream);
                }
            }
        } else {
            // 整体解压缩（使用STRING类型解压缩）
            if (!compressed_data.layer_data_combined.empty()) {
                std::vector<uint8_t> layer_raw = decompressWithConfig(compressed_data.layer_data_combined, 
                                                                     compression::FieldType::STRING, config);
                if (!layer_raw.empty()) {
                    // 使用与传统版本相同的反序列化方式
                    std::istringstream all_layers_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
                    louds->getLayeredStorage().deserialize(all_layers_stream);
                }
            }
        }
        
        louds->setFieldOrder(field_order);
        
        return {std::move(louds), std::move(manager)};
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware granular LOUDS decompression failed: " + std::string(e.what()));
    }
}

// ========== 部分解压缩实现（类型敏感版本） ==========

std::pair<std::unique_ptr<Trie>, std::unique_ptr<FieldDictionaryManager>> 
TypeAwareCompressor::decompressGranularPartial(const GranularCompressedData& compressed_data, 
                                              const Compressor::PartialDecompressionOptions& options,
                                              const compression::TypeAwareCompressionConfig& config) {
    try {
        validateTypeAwareConfig(config);
        
        std::unique_ptr<Trie> trie = nullptr;
        auto manager = std::make_unique<FieldDictionaryManager>();
        
        // 1. 解压缩元数据（通常需要）
        std::vector<FieldKey> field_order;
        if (options.load_metadata && !compressed_data.metadata.empty()) {
            std::vector<uint8_t> metadata_raw = decompressWithConfig(compressed_data.metadata, 
                                                                   compression::FieldType::STRING, config);
            if (!metadata_raw.empty()) {
                field_order = Compressor::deserializeMetadata(metadata_raw);
            }
        }
        
        // 2. 按需解压缩字典（使用类型敏感解压缩）
        if (options.load_string_dict && !compressed_data.string_dict.empty()) {
            std::vector<uint8_t> string_dict_raw = decompressWithConfig(compressed_data.string_dict, 
                                                                       compression::FieldType::STRING, config);
            if (!string_dict_raw.empty()) {
                Compressor::deserializeStringDictionary(string_dict_raw, *manager);
            }
        }
        
        if (options.load_timestamp_dict && !compressed_data.timestamp_dict.empty()) {
            std::vector<uint8_t> timestamp_dict_raw = decompressWithConfig(compressed_data.timestamp_dict, 
                                                                          compression::FieldType::TIMESTAMP, config);
            if (!timestamp_dict_raw.empty()) {
                Compressor::deserializeTimestampDictionary(timestamp_dict_raw, *manager);
            }
        }
        
        if (options.load_logtype_dict && !compressed_data.logtype_dict.empty()) {
            std::vector<uint8_t> logtype_dict_raw = decompressWithConfig(compressed_data.logtype_dict, 
                                                                        compression::FieldType::LOGTYPE, config);
            if (!logtype_dict_raw.empty()) {
                Compressor::deserializeLogTypeDictionary(logtype_dict_raw, *manager);
            }
        }
        
        // 3. 按需重建LOUDS Trie结构，然后转换为Trie
        std::unique_ptr<LOUDSTrie> louds = nullptr;
        if (options.load_trie && !field_order.empty() && !compressed_data.trie_bitmap.empty()) {
            std::vector<uint8_t> trie_raw = decompressWithConfig(compressed_data.trie_bitmap, 
                                                               compression::FieldType::BOOL, config);
            if (!trie_raw.empty()) {
                louds = Compressor::deserializeLoudsTrie(trie_raw, *manager, field_order);
            }
        }
        
        // 4. 按需重建分层内容（使用类型敏感解压缩）
        if (options.load_layers && louds) {
            if (compressed_data.use_layer_separation) {
                // 确保layers_向量有足够的空间来容纳所有层
                auto& layered_storage = louds->getLayeredStorage();
                size_t required_layers = compressed_data.layer_data_by_level.size();
                while (layered_storage.getLayerCount() < required_layers) {
                    layered_storage.addLayer(FieldKey{}, 0);
                }
                
                // 解压缩层大小信息（如果存在）
                std::vector<uint32_t> layer_sizes;
                if (!compressed_data.layer_sizes.empty()) {
                    std::vector<uint8_t> layer_sizes_raw = decompressWithConfig(compressed_data.layer_sizes, 
                                                                               compression::FieldType::STRING, config);
                    if (layer_sizes_raw.size() >= sizeof(uint32_t)) {
                        size_t layer_count = layer_sizes_raw.size() / sizeof(uint32_t);
                        layer_sizes.resize(layer_count);
                        std::memcpy(layer_sizes.data(), layer_sizes_raw.data(), layer_count * sizeof(uint32_t));
                    }
                }
                
                // 按指定层解压缩（使用字段类型特定的解压缩算法）
                if (!options.specific_layers.empty()) {
                    for (size_t layer_idx : options.specific_layers) {
                        if (layer_idx < compressed_data.layer_data_by_level.size()) {
                            // 根据字段类型选择解压缩算法
                            compression::FieldType field_type = compression::FieldType::STRING; // Default
                            if (layer_idx < field_order.size()) {
                                field_type = field_order[layer_idx].type;
                            }

                            std::vector<uint8_t> layer_data = decompressWithConfig(compressed_data.layer_data_by_level[layer_idx], 
                                                                                  field_type, config);
                            if (!layer_data.empty()) {
                                std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
                                layered_storage.deserializeLayer(layer_idx, layer_stream);
                            }
                        }
                    }
                } else {
                    // 解压缩所有层
                    for (size_t i = 0; i < compressed_data.layer_data_by_level.size(); ++i) {
                        compression::FieldType field_type = compression::FieldType::STRING; // Default
                        if (i < field_order.size()) {
                            field_type = field_order[i].type;
                        }
                        
                        std::vector<uint8_t> layer_data = decompressWithConfig(compressed_data.layer_data_by_level[i], 
                                                                              field_type, config);
                        if (!layer_data.empty()) {
                            std::istringstream layer_stream(std::string(layer_data.begin(), layer_data.end()), std::ios::binary);
                            layered_storage.deserializeLayer(i, layer_stream);
                        }
                    }
                }
            } else {
                // 整体解压缩（使用STRING类型解压缩）
                if (!compressed_data.layer_data_combined.empty()) {
                    std::vector<uint8_t> layer_raw = decompressWithConfig(compressed_data.layer_data_combined, 
                                                                         compression::FieldType::STRING, config);
                    if (!layer_raw.empty()) {
                        // 使用与传统版本相同的反序列化方式
                        std::istringstream all_layers_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
                        louds->getLayeredStorage().deserialize(all_layers_stream);
                    }
                }
            }
        }
        
        // 5. 如果需要Trie，将LOUDS Trie转换为普通Trie
        if (louds && options.load_trie) {
            trie = std::make_unique<Trie>(field_order);
            loudsToTrie(*louds, *trie);
        }
        
        return std::make_pair(std::move(trie), std::move(manager));
        
    } catch (const std::exception& e) {
        throw std::runtime_error("TypeAware granular partial decompression failed: " + std::string(e.what()));
    }
}

} // namespace json2