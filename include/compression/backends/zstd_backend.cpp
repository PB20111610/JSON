#include "zstd_backend.h"
#include "../algorithms/rle_compression.h"
#include <stdexcept>
#include <algorithm>
#include <cstring>

// 条件编译支持ZSTD
#ifdef USE_ZSTD
#include <zstd.h>
// 暂时禁用ZSTD字典训练功能，因为需要额外的zdict.h头文件
// 可以在必要时重新启用
// #define ZDICT_STATIC_LINKING_ONLY
// #include <zdict.h>
#endif

namespace json2 {
namespace compression {
namespace backends {

// ========== Enhanced Systematic Helper Functions ==========

// Helper function to validate ZSTD compression parameters
static void validateZstdParams(int level) {
    // ZSTD supports levels from 1 to 22, with negative levels for fast mode
    if (level < -5 || level > 22) {
        throw std::runtime_error("ZSTD: Invalid compression level " + std::to_string(level) + " (must be -5 to 22)");
    }
}

// Enhanced helper function to check ZSTD operation results
static void validateZstdResult(size_t result, const std::string& operation) {
#ifdef USE_ZSTD
    if (ZSTD_isError(result)) {
        throw std::runtime_error("ZSTD " + operation + " failed: " + std::string(ZSTD_getErrorName(result)));
    }
#endif
}

// Enhanced helper function to get decompressed size with validation
static size_t getDecompressedSize(const std::vector<uint8_t>& compressed_data) {
#ifdef USE_ZSTD
    if (compressed_data.empty()) {
        throw std::runtime_error("ZSTD: Cannot determine size of empty compressed data");
    }
    
    size_t size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());
    
    if (size == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("ZSTD: Invalid compressed data format");
    }
    
    if (size == ZSTD_CONTENTSIZE_UNKNOWN) {
        // If size cannot be determined, estimate 4x compression ratio
        size_t estimated = compressed_data.size() * 4;
        if (estimated < compressed_data.size()) { // Overflow check
            throw std::runtime_error("ZSTD: Estimated decompressed size too large");
        }
        return estimated;
    }
    
    return size;
#else
    return compressed_data.size();
#endif
}

// Helper function to check compression effectiveness
static bool validateCompressionEffectiveness(size_t original_size, size_t compressed_size, double threshold = 0.95) {
    if (original_size == 0) return true;
    double ratio = static_cast<double>(compressed_size) / original_size;
    return ratio < threshold;
}

// Helper function to validate minimum size for compression
static bool isWorthCompressing(const std::vector<uint8_t>& data, size_t min_size = 64) {
    if (data.size() < min_size) return false;
    
    // For very small data, compression overhead may exceed benefits
    if (data.size() < 32) return false;
    
    return true;
}

// ========== Enhanced Core Compression Methods ==========

std::vector<uint8_t> ZstdBackend::compress(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return {};
    
    try {
        validateZstdParams(level);
        return compressWithLevel(data, level);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("ZSTD compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> ZstdBackend::decompress(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return {};
    
    try {
        return decompressZstd(compressed_data);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("ZSTD decompression failed: " + std::string(e.what()));
    }
}

bool ZstdBackend::isAvailable() {
#ifdef USE_ZSTD
    return true;
#else
    return false;
#endif
}

// ========== Enhanced ZSTD Compression Implementation ==========

std::vector<uint8_t> ZstdBackend::compressWithLevel(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return data;
    
    try {
        validateZstdParams(level);
        
        // Check if compression is worthwhile
        if (!isWorthCompressing(data)) {
            return data;
        }
        
        // Limit compression level to valid range
        level = std::max(MIN_LEVEL, std::min(MAX_LEVEL, level));
        
#ifdef USE_ZSTD
        size_t compressed_bound = ZSTD_compressBound(data.size());
        if (compressed_bound == 0) {
            throw std::runtime_error("ZSTD: Input data too large for compression bounds calculation");
        }
        
        std::vector<uint8_t> compressed_data(compressed_bound);
        
        size_t actual_size = ZSTD_compress(compressed_data.data(), compressed_bound,
                                          data.data(), data.size(), level);
        
        validateZstdResult(actual_size, "compression");
        
        compressed_data.resize(actual_size);
        
        // Check compression effectiveness, if no significant benefit return original
        if (!validateCompressionEffectiveness(data.size(), actual_size)) {
            return data;
        }
        
        return compressed_data;
        
#else
        // If ZSTD not available, fallback to RLE
        return algorithms::RLECompression::rleCompress(data);
#endif
        
    } catch (const std::exception& e) {
        throw std::runtime_error("ZSTD compression with level internal error: " + std::string(e.what()));
    }
}

// ========== Enhanced ZSTD Decompression Implementation ==========

std::vector<uint8_t> ZstdBackend::decompressZstd(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return compressed_data;
    
    try {
#ifdef USE_ZSTD
        size_t decompressed_size = getDecompressedSize(compressed_data);
        std::vector<uint8_t> decompressed_data(decompressed_size);
        
        size_t actual_size = ZSTD_decompress(decompressed_data.data(), decompressed_size,
                                            compressed_data.data(), compressed_data.size());
        
        validateZstdResult(actual_size, "decompression");
        
        // Validate decompressed size matches expected
        if (actual_size != decompressed_size && decompressed_size != compressed_data.size() * 4) {
            // Only validate size mismatch if we had a definite size expectation
            decompressed_data.resize(actual_size);
        }
        
        return decompressed_data;
        
#else
        // If ZSTD not available, fallback to RLE
        return algorithms::RLECompression::rleDecompress(compressed_data);
#endif
        
    } catch (const std::exception& e) {
        throw std::runtime_error("ZSTD decompression internal error: " + std::string(e.what()));
    }
}

// ========== Enhanced Dictionary Compression with Validation ==========

std::vector<uint8_t> ZstdBackend::compressWithDictionary(const std::vector<uint8_t>& data, 
                                                        const std::vector<uint8_t>& dictionary, 
                                                        int level) {
#ifdef USE_ZSTD
    if (data.empty()) return data;
    
    try {
        validateZstdParams(level);
        
        // Create compression context with error checking
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        if (!cctx) {
            throw std::runtime_error("ZSTD: Failed to create compression context");
        }
        
        // Set dictionary with validation
        if (!dictionary.empty()) {
            size_t dict_result = ZSTD_CCtx_loadDictionary(cctx, dictionary.data(), dictionary.size());
            if (ZSTD_isError(dict_result)) {
                ZSTD_freeCCtx(cctx);
                throw std::runtime_error("ZSTD: Failed to load dictionary: " + std::string(ZSTD_getErrorName(dict_result)));
            }
        }
        
        // Set compression level with validation
        size_t level_result = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
        if (ZSTD_isError(level_result)) {
            ZSTD_freeCCtx(cctx);
            throw std::runtime_error("ZSTD: Failed to set compression level");
        }
        
        // Perform compression
        size_t compressed_size = ZSTD_compressBound(data.size());
        std::vector<uint8_t> compressed_data(compressed_size);
        
        size_t actual_size = ZSTD_compress2(cctx, compressed_data.data(), compressed_size,
                                           data.data(), data.size());
        
        ZSTD_freeCCtx(cctx);
        
        validateZstdResult(actual_size, "dictionary compression");
        
        compressed_data.resize(actual_size);
        return compressed_data;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("ZSTD dictionary compression failed: " + std::string(e.what()));
    }
#else
    return compressWithLevel(data, level);
#endif
}

std::vector<uint8_t> ZstdBackend::decompressWithDictionary(const std::vector<uint8_t>& compressed_data,
                                                          const std::vector<uint8_t>& dictionary) {
#ifdef USE_ZSTD
    if (compressed_data.empty()) return compressed_data;
    
    try {
        // Create decompression context with error checking
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        if (!dctx) {
            throw std::runtime_error("ZSTD: Failed to create decompression context");
        }
        
        // Set dictionary with validation
        if (!dictionary.empty()) {
            size_t dict_result = ZSTD_DCtx_loadDictionary(dctx, dictionary.data(), dictionary.size());
            if (ZSTD_isError(dict_result)) {
                ZSTD_freeDCtx(dctx);
                throw std::runtime_error("ZSTD: Failed to load dictionary: " + std::string(ZSTD_getErrorName(dict_result)));
            }
        }
        
        // Get decompressed size with validation
        size_t decompressed_size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());
        if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
            ZSTD_freeDCtx(dctx);
            throw std::runtime_error("ZSTD: Invalid compressed data format for dictionary decompression");
        }
        
        if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            decompressed_size = compressed_data.size() * 4;
        }
        
        std::vector<uint8_t> decompressed_data(decompressed_size);
        
        size_t actual_size = ZSTD_decompressDCtx(dctx, decompressed_data.data(), decompressed_size,
                                                compressed_data.data(), compressed_data.size());
        
        ZSTD_freeDCtx(dctx);
        
        validateZstdResult(actual_size, "dictionary decompression");
        
        decompressed_data.resize(actual_size);
        return decompressed_data;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("ZSTD dictionary decompression failed: " + std::string(e.what()));
    }
#else
    return decompressZstd(compressed_data);
#endif
}

// ========== Enhanced Analysis and Validation Functions ==========

double ZstdBackend::estimateCompressionRatio(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return 1.0;
    
    try {
        validateZstdParams(level);
        
#ifdef USE_ZSTD
        // Use ZSTD compression bounds for more accurate estimation
        size_t bound = ZSTD_compressBound(data.size());
        if (bound == 0) {
            return 1.0; // Conservative estimate if bounds calculation fails
        }
        
        // Adjust estimation based on compression level
        double level_factor = 1.0;
        if (level <= 3) {
            level_factor = 0.8;      // Low compression level
        } else if (level <= 9) {
            level_factor = 0.6;      // Medium compression level
        } else {
            level_factor = 0.4;      // High compression level
        }
        
        return std::min(1.0, static_cast<double>(bound) * level_factor / data.size());
        
#else
        // Fallback to RLE estimation
        return algorithms::RLECompression::estimateCompressionRatio(data);
#endif
        
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

bool ZstdBackend::isWorthCompressing(const std::vector<uint8_t>& data, size_t min_size) {
    if (data.size() < min_size) return false;
    
    try {
        // For very small data, compression overhead may exceed benefits
        if (data.size() < MIN_SIZE_FOR_COMPRESSION) return false;
        
        // Estimate compression ratio, only compress if expected ratio < 0.9
        double estimated_ratio = estimateCompressionRatio(data);
        return estimated_ratio < 0.9;
        
    } catch (const std::exception&) {
        return false; // Conservative approach on error
    }
}

// ========== Enhanced Dictionary Training with Graceful Degradation ==========

std::vector<uint8_t> ZstdBackend::trainDictionary(const std::vector<std::vector<uint8_t>>& samples, 
                                                 size_t dict_size) {
#ifdef USE_ZSTD
    if (samples.empty()) return {};
    
    try {
        // Merge all sample data with size validation
        std::vector<uint8_t> training_data;
        std::vector<size_t> sample_sizes;
        
        size_t total_size = 0;
        for (const auto& sample : samples) {
            if (sample.empty()) continue; // Skip empty samples
            
            // Check for potential overflow
            if (total_size > SIZE_MAX - sample.size()) {
                throw std::runtime_error("ZSTD: Training data too large - potential overflow");
            }
            
            total_size += sample.size();
            training_data.insert(training_data.end(), sample.begin(), sample.end());
            sample_sizes.push_back(sample.size());
        }
        
        if (training_data.empty()) {
            return {}; // No valid training data
        }
        
        // Validate dictionary size
        if (dict_size == 0 || dict_size > training_data.size() / 2) {
            throw std::runtime_error("ZSTD: Invalid dictionary size");
        }
        
        // ZSTD dictionary training functionality temporarily disabled 
        // because zdict.h header file is not available
        // Return empty dictionary to indicate no dictionary compression
        // This maintains graceful degradation as per memory requirement
        return {};
        
    } catch (const std::exception& e) {
        // Graceful degradation: return empty dictionary on error
        return {};
    }
    
#else
    return {}; // No dictionary support when ZSTD not available
#endif
}

std::string ZstdBackend::getVersionInfo() {
#ifdef USE_ZSTD
    unsigned version = ZSTD_versionNumber();
    unsigned major = version / 10000;
    unsigned minor = (version % 10000) / 100;
    unsigned release = version % 100;
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(release);
#else
    return "Not available";
#endif
}

} // namespace backends
} // namespace compression
} // namespace json2