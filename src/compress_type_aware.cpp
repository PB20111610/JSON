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

// Helper function to map json2::FieldType to compression::FieldType
static compression::FieldType mapFieldType(json2::FieldType json_field_type) {
    switch (json_field_type) {
        case json2::FieldType::String:
        case json2::FieldType::UnstructuredArray:
            return compression::FieldType::STRING;
        case json2::FieldType::Int:
            return compression::FieldType::INT64;
        case json2::FieldType::Double:
            return compression::FieldType::DOUBLE;
        case json2::FieldType::Bool:
            return compression::FieldType::BOOL;
        default:
            return compression::FieldType::STRING; // Safe default
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
        return compressLouds(louds, manager, trie.getOrderedFields(), config);
        
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
            compression::FieldType field_type = compression::FieldType::STRING; // Default
            if (i < field_order.size()) {
                field_type = mapFieldType(field_order[i].type);
            }
            
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

        // 2. Enhanced layer data decompression with systematic validation
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
        
        // Validate layer count bounds
        if (layer_count > 1000) { // Reasonable upper limit
            throw std::runtime_error("TypeAware: Invalid layer count: " + std::to_string(layer_count));
        }
        
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
                FieldKey temp_fk{"temp_field_" + std::to_string(i), json2::FieldType::String};
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
            
            // Use field type-specific decompression (default to STRING for safety)
            compression::FieldType field_type = compression::FieldType::STRING;
            
            std::vector<uint8_t> layer_raw = decompressWithConfig(compressed_layer, field_type, config);
            if (layer_raw.empty()) {
                throw std::runtime_error("TypeAware: Empty decompressed layer " + std::to_string(i));
            }
            
            std::istringstream layer_stream(std::string(layer_raw.begin(), layer_raw.end()), std::ios::binary);
            louds->getLayeredStorage().deserializeLayer(i, layer_stream);
        }

        // 3. Enhanced dictionary decompression with validation
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

        // 4. Enhanced metadata decompression with validation
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

} // namespace json2