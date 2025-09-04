#include "snappy_backend.h"
#include "../algorithms/rle_compression.h"
#include <stdexcept>
#include <cstring>

#ifdef HAVE_SNAPPY
#include <snappy.h>
#endif

namespace json2 {
namespace compression {
namespace backends {

// ========== Systematic Helper Functions ==========

// Helper function to validate Snappy operations
static void validateSnappyOperation(bool result, const std::string& operation) {
    if (!result) {
        throw std::runtime_error("Snappy " + operation + " operation failed");
    }
}

// Helper function to validate input data for Snappy
static void validateSnappyInput(const std::vector<uint8_t>& data) {
    // Snappy has no specific input size limits, but validate basic constraints
    if (data.size() > SIZE_MAX / 2) {
        throw std::runtime_error("Snappy: Input data too large for processing");
    }
}

// Helper function to check compression effectiveness
static bool validateCompressionEffectiveness(size_t original_size, size_t compressed_size, double threshold = 0.95) {
    if (original_size == 0) return true;
    double ratio = static_cast<double>(compressed_size) / original_size;
    return ratio < threshold;
}

// Helper function to safely convert between string and vector<uint8_t>
static std::string vectorToString(const std::vector<uint8_t>& data) {
    if (data.empty()) return std::string();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

static std::vector<uint8_t> stringToVector(const std::string& str) {
    if (str.empty()) return std::vector<uint8_t>();
    return std::vector<uint8_t>(str.begin(), str.end());
}

// ========== Enhanced Core Compression Methods ==========

std::vector<uint8_t> SnappyBackend::compress(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return {};
    
    try {
        if (isAvailable()) {
            validateSnappyInput(data);
            std::vector<uint8_t> compressed = snappyCompress(data);
            
            // Validate compression effectiveness
            if (!validateCompressionEffectiveness(data.size(), compressed.size())) {
                // If Snappy compression isn't effective, try fallback
                std::vector<uint8_t> fallback_compressed = fallbackCompress(data);
                if (fallback_compressed.size() < compressed.size()) {
                    return fallback_compressed;
                }
            }
            
            return compressed;
        } else {
            return fallbackCompress(data);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Snappy compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> SnappyBackend::decompress(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return {};
    
    try {
        if (isAvailable()) {
            return snappyDecompress(compressed_data);
        } else {
            return fallbackDecompress(compressed_data);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Snappy decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced Snappy Implementation with Robust Error Handling ==========

std::vector<uint8_t> SnappyBackend::snappyCompress(const std::vector<uint8_t>& data) {
#ifdef HAVE_SNAPPY
    if (data.empty()) return {};
    
    try {
        validateSnappyInput(data);
        
        std::string input = vectorToString(data);
        std::string compressed;
        
        // Execute Snappy compression with error checking
        snappy::Compress(input.data(), input.size(), &compressed);
        
        // Validate compression result
        if (compressed.empty() && !data.empty()) {
            throw std::runtime_error("Snappy compression produced empty result for non-empty input");
        }
        
        return stringToVector(compressed);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Snappy compression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("Snappy compression not available - library not compiled");
#endif
}

std::vector<uint8_t> SnappyBackend::snappyDecompress(const std::vector<uint8_t>& compressed) {
#ifdef HAVE_SNAPPY
    if (compressed.empty()) return {};
    
    try {
        // Validate compressed data first
        if (!isValidCompressedData(compressed)) {
            throw std::runtime_error("Snappy: Invalid compressed data format");
        }
        
        std::string compressed_str = vectorToString(compressed);
        std::string decompressed;
        
        // Execute Snappy decompression with error checking
        bool result = snappy::Uncompress(compressed_str.data(), compressed_str.size(), &decompressed);
        validateSnappyOperation(result, "decompression");
        
        return stringToVector(decompressed);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Snappy decompression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("Snappy decompression not available - library not compiled");
#endif
}

// ========== Enhanced Analysis and Validation Functions ==========

bool SnappyBackend::isAvailable() {
#ifdef HAVE_SNAPPY
    return true;
#else
    return false;
#endif
}

double SnappyBackend::estimateCompressionRatio(const std::vector<uint8_t>& data) {
    if (data.empty()) return 1.0;
    
    try {
        // Enhanced empirical compression ratio estimation based on Snappy characteristics
        // Snappy focuses on speed over compression ratio
        return 0.75; // Snappy typically achieves ~75% of original size
        
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

size_t SnappyBackend::estimateCompressionTime(size_t input_size) {
    if (input_size == 0) return 0;
    
    try {
        // Return estimated time in microseconds, Snappy is the fastest
        return input_size / 50000; // ~50MB/s
        
    } catch (const std::exception&) {
        return input_size / 10000; // Conservative estimate: 10MB/s
    }
}

bool SnappyBackend::isValidCompressedData(const std::vector<uint8_t>& compressed) {
#ifdef HAVE_SNAPPY
    if (compressed.empty()) return false;
    
    try {
        std::string compressed_str = vectorToString(compressed);
        return snappy::IsValidCompressedBuffer(compressed_str.data(), compressed_str.size());
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
#else
    return false;
#endif
}

size_t SnappyBackend::getUncompressedLength(const std::vector<uint8_t>& compressed) {
#ifdef HAVE_SNAPPY
    if (compressed.empty()) return 0;
    
    try {
        std::string compressed_str = vectorToString(compressed);
        size_t length;
        
        if (snappy::GetUncompressedLength(compressed_str.data(), compressed_str.size(), &length)) {
            return length;
        }
        
        return 0;
        
    } catch (const std::exception&) {
        return 0; // Conservative approach on error
    }
#else
    throw std::runtime_error("Snappy not available - library not compiled");
#endif
}

// ========== Enhanced Fallback Methods ==========

std::vector<uint8_t> SnappyBackend::fallbackCompress(const std::vector<uint8_t>& data) {
    try {
        // Use RLE compression as fallback with error handling
        return algorithms::RLECompression::rleCompress(data);
    } catch (const std::exception& e) {
        throw std::runtime_error("Snappy fallback compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> SnappyBackend::fallbackDecompress(const std::vector<uint8_t>& compressed) {
    try {
        // Use RLE decompression as fallback with error handling
        return algorithms::RLECompression::rleDecompress(compressed);
    } catch (const std::exception& e) {
        throw std::runtime_error("Snappy fallback decompression failed: " + std::string(e.what()));
    }
}

} // namespace backends
} // namespace compression
} // namespace json2