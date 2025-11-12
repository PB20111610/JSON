#pragma once

#include "../include/compression/type_aware/type_aware_compressor.h"
#include "../include/chunked_type_aware_compress.h"

namespace json2 {
namespace test {

/**
 * Utility function to create a standard TypeAwareCompressionConfig for testing
 * This ensures consistency across all test files
 */
inline compression::TypeAwareCompressionConfig createStandardTestConfig() {
    compression::TypeAwareCompressionConfig config;
    
    // Core backends
    config.louds_backend = compression::CompressionBackend::BIT_PACKING;
    config.dictionary_backend = compression::CompressionBackend::ZSTD;
    config.metadata_backend = compression::CompressionBackend::ZSTD;
    
    // Field type compression backends
    config.layer_config.int_backend = compression::CompressionBackend::DELTA_VARINT;
    config.layer_config.double_backend = compression::CompressionBackend::DELTA_VARINT;
    config.layer_config.bool_backend = compression::CompressionBackend::BIT_PACKING;
    config.layer_config.string_backend = compression::CompressionBackend::DELTA_VARINT;
    config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_VARINT;
    config.layer_config.logtype_backend = compression::CompressionBackend::DELTA_VARINT;
    config.layer_config.array_backend = compression::CompressionBackend::RLE;
    config.layer_config.null_backend = compression::CompressionBackend::BIT_PACKING;
    
    // Compression level
    config.compression_level = 3;
    
    return config;
}

} // namespace test

// Function to expose the test configuration
inline ChunkedTypeAwareConfig getTestConfig() {
    ChunkedTypeAwareConfig config;
    config.type_aware_config = json2::test::createStandardTestConfig();
    // Change timestamp compression method to DELTA_VARINT for our testing
    config.type_aware_config.layer_config.timestamp_backend = compression::CompressionBackend::DELTA_VARINT;
    return config;
}

} // namespace json2