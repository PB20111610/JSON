#include "lzma_backend.h"
#include "../algorithms/rle_compression.h"
#include <stdexcept>
#include <cstring>

#ifdef HAVE_LZMA
#include <lzma.h>
#endif

namespace json2 {
namespace compression {
namespace backends {

// ========== Systematic Helper Functions ==========

// Helper function to validate LZMA compression parameters
static void validateLzmaParams(int level) {
    if (level < 0 || level > 9) {
        throw std::runtime_error("LZMA: Invalid compression level " + std::to_string(level) + " (must be 0-9)");
    }
}

#ifdef HAVE_LZMA
// Helper function to check LZMA operation results
static void validateLzmaResult(lzma_ret ret, const std::string& operation) {
    switch (ret) {
        case LZMA_OK:
        case LZMA_STREAM_END:
            return; // Success cases
        case LZMA_MEM_ERROR:
            throw std::runtime_error("LZMA " + operation + " failed: Out of memory");
        case LZMA_FORMAT_ERROR:
            throw std::runtime_error("LZMA " + operation + " failed: Invalid format");
        case LZMA_OPTIONS_ERROR:
            throw std::runtime_error("LZMA " + operation + " failed: Invalid options");
        case LZMA_DATA_ERROR:
            throw std::runtime_error("LZMA " + operation + " failed: Data corruption");
        case LZMA_BUF_ERROR:
            throw std::runtime_error("LZMA " + operation + " failed: Buffer error");
        case LZMA_PROG_ERROR:
            throw std::runtime_error("LZMA " + operation + " failed: Programming error");
        default:
            throw std::runtime_error("LZMA " + operation + " failed: Unknown error code " + std::to_string(ret));
    }
}

// Helper function to safely initialize LZMA encoder
static lzma_stream initializeLzmaEncoder(int level) {
    lzma_stream stream = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_easy_encoder(&stream, level, LZMA_CHECK_CRC64);
    validateLzmaResult(ret, "encoder initialization");
    return stream;
}

// Helper function to safely initialize LZMA decoder
static lzma_stream initializeLzmaDecoder() {
    lzma_stream stream = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_stream_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED);
    validateLzmaResult(ret, "decoder initialization");
    return stream;
}
#endif

// Helper function to check compression effectiveness
static bool validateCompressionEffectiveness(size_t original_size, size_t compressed_size, double threshold = 0.9) {
    if (original_size == 0) return true;
    double ratio = static_cast<double>(compressed_size) / original_size;
    return ratio < threshold;
}

// ========== Enhanced Core Compression Methods ==========

std::vector<uint8_t> LzmaBackend::compress(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return {};
    
    try {
        validateLzmaParams(level);
        
        if (isAvailable()) {
            std::vector<uint8_t> compressed = lzmaCompress(data, level);
            
            // Validate compression effectiveness
            if (!validateCompressionEffectiveness(data.size(), compressed.size())) {
                // If LZMA compression isn't effective, try fallback
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
        throw std::runtime_error("LZMA compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> LzmaBackend::decompress(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return {};
    
    try {
        if (isAvailable()) {
            return lzmaDecompress(compressed_data);
        } else {
            return fallbackDecompress(compressed_data);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LZMA decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced LZMA Compression Implementation ==========

std::vector<uint8_t> LzmaBackend::lzmaCompress(const std::vector<uint8_t>& data, int level) {
#ifdef HAVE_LZMA
    if (data.empty()) return {};
    
    try {
        validateLzmaParams(level);
        
        // Initialize encoder with error checking
        lzma_stream stream = initializeLzmaEncoder(level);
        
        // Set input data
        stream.next_in = data.data();
        stream.avail_in = data.size();
        
        std::vector<uint8_t> compressed;
        compressed.reserve(data.size() / 2); // Initial reasonable estimate
        
        std::vector<uint8_t> buffer(8192); // 8KB buffer for streaming
        
        lzma_action action = LZMA_FINISH;
        lzma_ret ret;
        
        do {
            stream.next_out = buffer.data();
            stream.avail_out = buffer.size();
            
            ret = lzma_code(&stream, action);
            validateLzmaResult(ret, "compression");
            
            // Copy compressed data from buffer
            size_t compressed_size = buffer.size() - stream.avail_out;
            if (compressed_size > 0) {
                compressed.insert(compressed.end(), buffer.begin(), buffer.begin() + compressed_size);
            }
            
        } while (ret != LZMA_STREAM_END);
        
        // Cleanup
        lzma_end(&stream);
        
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LZMA compression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("LZMA compression not available - library not compiled");
#endif
}

// ========== Enhanced LZMA Decompression Implementation ==========

std::vector<uint8_t> LzmaBackend::lzmaDecompress(const std::vector<uint8_t>& compressed) {
#ifdef HAVE_LZMA
    if (compressed.empty()) return {};
    
    try {
        // Initialize decoder with error checking
        lzma_stream stream = initializeLzmaDecoder();
        
        // Set input data
        stream.next_in = compressed.data();
        stream.avail_in = compressed.size();
        
        std::vector<uint8_t> decompressed;
        decompressed.reserve(compressed.size() * 3); // Reasonable initial estimate
        
        std::vector<uint8_t> buffer(8192); // 8KB buffer for streaming
        
        lzma_ret ret;
        
        do {
            stream.next_out = buffer.data();
            stream.avail_out = buffer.size();
            
            ret = lzma_code(&stream, LZMA_RUN);
            validateLzmaResult(ret, "decompression");
            
            // Copy decompressed data from buffer
            size_t decompressed_size = buffer.size() - stream.avail_out;
            if (decompressed_size > 0) {
                decompressed.insert(decompressed.end(), buffer.begin(), buffer.begin() + decompressed_size);
            }
            
        } while (ret != LZMA_STREAM_END && stream.avail_in > 0);
        
        // Cleanup
        lzma_end(&stream);
        
        if (ret != LZMA_STREAM_END) {
            throw std::runtime_error("LZMA decompression incomplete: expected STREAM_END");
        }
        
        return decompressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("LZMA decompression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("LZMA decompression not available - library not compiled");
#endif
}

// ========== Enhanced Analysis and Validation Functions ==========

bool LzmaBackend::isAvailable() {
#ifdef HAVE_LZMA
    return true;
#else
    return false;
#endif
}

double LzmaBackend::estimateCompressionRatio(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return 1.0;
    
    try {
        validateLzmaParams(level);
        
        // Enhanced empirical compression ratio estimation based on LZMA characteristics
        if (level <= 3) {
            return 0.55; // Fast mode
        } else if (level <= 6) {
            return 0.45; // Balanced mode
        } else {
            return 0.35; // High compression mode
        }
        
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

size_t LzmaBackend::estimateCompressionTime(size_t input_size, int level) {
    if (input_size == 0) return 0;
    
    try {
        validateLzmaParams(level);
        
        // Return estimated time in microseconds, LZMA is slower
        if (level <= 3) {
            return input_size / 100; // ~100KB/s
        } else if (level <= 6) {
            return input_size / 50;  // ~50KB/s
        } else {
            return input_size / 20;  // ~20KB/s
        }
        
    } catch (const std::exception&) {
        return input_size / 10; // Conservative estimate: 10KB/s
    }
}

// ========== Enhanced Fallback Methods ==========

std::vector<uint8_t> LzmaBackend::fallbackCompress(const std::vector<uint8_t>& data) {
    try {
        // Use RLE compression as fallback with error handling
        return algorithms::RLECompression::rleCompress(data);
    } catch (const std::exception& e) {
        throw std::runtime_error("LZMA fallback compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> LzmaBackend::fallbackDecompress(const std::vector<uint8_t>& compressed) {
    try {
        // Use RLE decompression as fallback with error handling
        return algorithms::RLECompression::rleDecompress(compressed);
    } catch (const std::exception& e) {
        throw std::runtime_error("LZMA fallback decompression failed: " + std::string(e.what()));
    }
}

} // namespace backends
} // namespace compression
} // namespace json2