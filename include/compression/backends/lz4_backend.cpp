#include "lz4_backend.h"
#include "../algorithms/rle_compression.h"
#include <stdexcept>
#include <cstring>

#ifdef HAVE_LZ4
#include <lz4.h>
#include <lz4hc.h>
#endif

namespace json2 {
namespace compression {
namespace backends {

// ========== Systematic Helper Functions ==========

// Helper function to validate LZ4 compression parameters
static void validateLz4Params(int level) {
    if (level < 1 || level > 12) {
        throw std::runtime_error("LZ4: Invalid compression level " + std::to_string(level) + " (must be 1-12)");
    }
}

// Helper function to validate and calculate LZ4 bounds
static int calculateLz4Bounds(size_t input_size) {
#ifdef HAVE_LZ4
    if (input_size > INT_MAX) {
        throw std::runtime_error("LZ4: Input size too large for compression");
    }
    
    int max_compressed_size = LZ4_compressBound(static_cast<int>(input_size));
    if (max_compressed_size <= 0) {
        throw std::runtime_error("LZ4: Failed to calculate compression bounds");
    }
    
    return max_compressed_size;
#else
    return 0;
#endif
}

// Helper function to write size header with validation
static void writeOriginalSizeHeader(std::vector<uint8_t>& output, uint32_t original_size) {
    if (output.size() < sizeof(uint32_t)) {
        output.resize(sizeof(uint32_t));
    }
    std::memcpy(output.data(), &original_size, sizeof(uint32_t));
}

// Helper function to read size header with validation
static uint32_t readOriginalSizeHeader(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(uint32_t)) {
        throw std::runtime_error("LZ4: Invalid compressed data - missing size header");
    }
    
    uint32_t original_size;
    std::memcpy(&original_size, data.data(), sizeof(uint32_t));
    
    if (original_size == 0 && data.size() > sizeof(uint32_t)) {
        throw std::runtime_error("LZ4: Invalid original size in header");
    }
    
    return original_size;
}

// Helper function to check compression effectiveness
static bool validateCompressionEffectiveness(size_t original_size, size_t compressed_size, double threshold = 0.9) {
    if (original_size == 0) return true;
    double ratio = static_cast<double>(compressed_size) / original_size;
    return ratio < threshold;
}

// ========== Enhanced Core Compression Methods ==========

std::vector<uint8_t> Lz4Backend::compress(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return {};
    
    try {
        validateLz4Params(level);
        
        if (isAvailable()) {
            std::vector<uint8_t> compressed;
            
            if (level <= 6) {
                compressed = lz4Compress(data, level);
            } else {
                compressed = lz4HcCompress(data, level);
            }
            
            // Validate compression effectiveness
            if (!validateCompressionEffectiveness(data.size(), compressed.size())) {
                // If LZ4 compression isn't effective, try fallback
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
        throw std::runtime_error("LZ4 compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> Lz4Backend::decompress(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return {};
    
    try {
        if (isAvailable()) {
            return lz4Decompress(compressed_data);
        } else {
            return fallbackDecompress(compressed_data);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LZ4 decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced LZ4 Standard Compression ==========

std::vector<uint8_t> Lz4Backend::lz4Compress(const std::vector<uint8_t>& data, int level) {
#ifdef HAVE_LZ4
    if (data.empty()) return {};
    
    try {
        validateLz4Params(level);
        
        // Calculate maximum compressed size with validation
        int max_compressed_size = calculateLz4Bounds(data.size());
        
        std::vector<uint8_t> compressed(max_compressed_size + 4); // +4 for size header
        
        // Write original size header
        uint32_t original_size = static_cast<uint32_t>(data.size());
        writeOriginalSizeHeader(compressed, original_size);
        
        // Execute compression with bounds checking
        int compressed_size = LZ4_compress_default(
            reinterpret_cast<const char*>(data.data()),
            reinterpret_cast<char*>(compressed.data() + 4),
            static_cast<int>(data.size()),
            max_compressed_size
        );
        
        if (compressed_size <= 0) {
            throw std::runtime_error("LZ4 standard compression operation failed");
        }
        
        // Validate compressed size bounds
        if (compressed_size > max_compressed_size) {
            throw std::runtime_error("LZ4: Compressed size exceeds calculated bounds");
        }
        
        compressed.resize(compressed_size + 4);
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LZ4 standard compression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("LZ4 compression not available - library not compiled");
#endif
}

// ========== Enhanced LZ4 High Compression ==========

std::vector<uint8_t> Lz4Backend::lz4HcCompress(const std::vector<uint8_t>& data, int level) {
#ifdef HAVE_LZ4
    if (data.empty()) return {};
    
    try {
        validateLz4Params(level);
        
        // Calculate maximum compressed size with validation
        int max_compressed_size = calculateLz4Bounds(data.size());
        
        std::vector<uint8_t> compressed(max_compressed_size + 4); // +4 for size header
        
        // Write original size header
        uint32_t original_size = static_cast<uint32_t>(data.size());
        writeOriginalSizeHeader(compressed, original_size);
        
        // Execute high compression with bounds checking
        int compressed_size = LZ4_compress_HC(
            reinterpret_cast<const char*>(data.data()),
            reinterpret_cast<char*>(compressed.data() + 4),
            static_cast<int>(data.size()),
            max_compressed_size,
            level
        );
        
        if (compressed_size <= 0) {
            throw std::runtime_error("LZ4HC compression operation failed");
        }
        
        // Validate compressed size bounds
        if (compressed_size > max_compressed_size) {
            throw std::runtime_error("LZ4HC: Compressed size exceeds calculated bounds");
        }
        
        compressed.resize(compressed_size + 4);
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LZ4HC compression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("LZ4HC compression not available - library not compiled");
#endif
}

// ========== Enhanced LZ4 Decompression ==========

std::vector<uint8_t> Lz4Backend::lz4Decompress(const std::vector<uint8_t>& compressed) {
#ifdef HAVE_LZ4
    if (compressed.empty()) return {};
    
    try {
        // Read and validate original size header
        uint32_t original_size = readOriginalSizeHeader(compressed);
        
        if (original_size == 0) {
            return {};
        }
        
        // Validate compressed data size
        if (compressed.size() < sizeof(uint32_t) + 1) {
            throw std::runtime_error("LZ4: Insufficient compressed data");
        }
        
        std::vector<uint8_t> decompressed(original_size);
        
        // Execute safe decompression
        int result = LZ4_decompress_safe(
            reinterpret_cast<const char*>(compressed.data() + 4),
            reinterpret_cast<char*>(decompressed.data()),
            static_cast<int>(compressed.size() - 4),
            static_cast<int>(original_size)
        );
        
        if (result < 0) {
            throw std::runtime_error("LZ4 decompression operation failed: result code " + std::to_string(result));
        }
        
        // Validate decompressed size matches expected
        if (static_cast<uint32_t>(result) != original_size) {
            throw std::runtime_error("LZ4: Decompressed size mismatch - expected " + 
                                   std::to_string(original_size) + ", got " + std::to_string(result));
        }
        
        return decompressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LZ4 decompression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("LZ4 decompression not available - library not compiled");
#endif
}

// ========== Enhanced Analysis and Validation Functions ==========

bool Lz4Backend::isAvailable() {
#ifdef HAVE_LZ4
    return true;
#else
    return false;
#endif
}

double Lz4Backend::estimateCompressionRatio(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return 1.0;
    
    try {
        validateLz4Params(level);
        
        // Enhanced empirical compression ratio estimation based on LZ4 characteristics
        if (level <= 3) {
            return 0.70; // Fast mode
        } else if (level <= 6) {
            return 0.65; // Standard mode
        } else {
            return 0.60; // High compression mode
        }
        
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

size_t Lz4Backend::estimateCompressionTime(size_t input_size, int level) {
    if (input_size == 0) return 0;
    
    try {
        validateLz4Params(level);
        
        // Return estimated time in microseconds, LZ4 is very fast
        if (level <= 6) {
            return input_size / 10000; // ~10MB/s
        } else {
            return input_size / 2000;  // ~2MB/s (HC mode slower)
        }
        
    } catch (const std::exception&) {
        return input_size / 1000; // Conservative estimate: 1KB/s
    }
}

// ========== Enhanced Fallback Methods ==========

std::vector<uint8_t> Lz4Backend::fallbackCompress(const std::vector<uint8_t>& data) {
    try {
        // Use RLE compression as fallback with error handling
        return algorithms::RLECompression::rleCompress(data);
    } catch (const std::exception& e) {
        throw std::runtime_error("LZ4 fallback compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> Lz4Backend::fallbackDecompress(const std::vector<uint8_t>& compressed) {
    try {
        // Use RLE decompression as fallback with error handling
        return algorithms::RLECompression::rleDecompress(compressed);
    } catch (const std::exception& e) {
        throw std::runtime_error("LZ4 fallback decompression failed: " + std::string(e.what()));
    }
}

} // namespace backends
} // namespace compression
} // namespace json2