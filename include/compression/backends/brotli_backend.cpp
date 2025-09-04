#include "brotli_backend.h"
#include "../algorithms/rle_compression.h"
#include <stdexcept>
#include <cstring>

#ifdef HAVE_BROTLI
#include <brotli/encode.h>
#include <brotli/decode.h>
#endif

namespace json2 {
namespace compression {
namespace backends {

// ========== Systematic Helper Functions ==========

// Helper function to validate Brotli compression parameters
static void validateBrotliParams(int quality) {
    if (quality < 0 || quality > 11) {
        throw std::runtime_error("Brotli: Invalid quality level " + std::to_string(quality) + " (must be 0-11)");
    }
}

// Helper function to validate input data size
static void validateInputSize(const std::vector<uint8_t>& data) {
#ifdef HAVE_BROTLI
    if (!data.empty()) {
        size_t max_size = BrotliEncoderMaxCompressedSize(data.size());
        if (max_size == 0) {
            throw std::runtime_error("Brotli: Input data too large for compression");
        }
    }
#endif
}

// Helper function to check compression effectiveness
static bool validateCompressionEffectiveness(size_t original_size, size_t compressed_size, double threshold = 0.9) {
    if (original_size == 0) return true;
    double ratio = static_cast<double>(compressed_size) / original_size;
    return ratio < threshold;
}

// Helper function to safely handle Brotli decoder output
#ifdef HAVE_BROTLI
static void extractBrotliOutput(BrotliDecoderState* state, std::vector<uint8_t>& output) {
    size_t decoded_size = 0;
    const uint8_t* decoded_data = BrotliDecoderTakeOutput(state, &decoded_size);
    if (decoded_size > 0 && decoded_data != nullptr) {
        output.insert(output.end(), decoded_data, decoded_data + decoded_size);
    }
}
#endif

// ========== Enhanced Core Compression Methods ==========

std::vector<uint8_t> BrotliBackend::compress(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return {};
    
    try {
        validateBrotliParams(level);
        
        if (isAvailable()) {
            validateInputSize(data);
            std::vector<uint8_t> compressed = brotliCompress(data, level);
            
            // Validate compression effectiveness
            if (!validateCompressionEffectiveness(data.size(), compressed.size())) {
                // If Brotli compression isn't effective, try fallback
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
        throw std::runtime_error("Brotli compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> BrotliBackend::decompress(const std::vector<uint8_t>& compressed_data) {
    if (compressed_data.empty()) return {};
    
    try {
        if (isAvailable()) {
            return brotliDecompress(compressed_data);
        } else {
            return fallbackDecompress(compressed_data);
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Brotli decompression failed: " + std::string(e.what()));
    }
}

// ========== Enhanced Brotli Implementation with Robust Error Handling ==========

std::vector<uint8_t> BrotliBackend::brotliCompress(const std::vector<uint8_t>& data, int quality) {
#ifdef HAVE_BROTLI
    if (data.empty()) return {};
    
    try {
        // Validate quality parameter
        validateBrotliParams(quality);
        
        // Estimate output buffer size with safety margin
        size_t max_output_size = BrotliEncoderMaxCompressedSize(data.size());
        if (max_output_size == 0) {
            throw std::runtime_error("Brotli: Input too large for compression");
        }
        
        std::vector<uint8_t> compressed(max_output_size);
        size_t encoded_size = compressed.size();
        
        // Execute compression with validated parameters
        BROTLI_BOOL result = BrotliEncoderCompress(
            quality,                    // quality (0-11)
            BROTLI_DEFAULT_WINDOW,      // window size
            BROTLI_DEFAULT_MODE,        // mode
            data.size(),                // input size
            data.data(),                // input data
            &encoded_size,              // output size
            compressed.data()           // output buffer
        );
        
        if (result != BROTLI_TRUE) {
            throw std::runtime_error("Brotli compression operation failed");
        }
        
        // Validate output size
        if (encoded_size > max_output_size) {
            throw std::runtime_error("Brotli: Output size exceeds expected bounds");
        }
        
        compressed.resize(encoded_size);
        return compressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Brotli compression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("Brotli compression not available - library not compiled");
#endif
}

std::vector<uint8_t> BrotliBackend::brotliDecompress(const std::vector<uint8_t>& compressed) {
#ifdef HAVE_BROTLI
    if (compressed.empty()) return {};
    
    try {
        // Create decoder state with error checking
        BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        if (!state) {
            throw std::runtime_error("Brotli: Failed to create decoder instance");
        }
        
        // Initialize input parameters
        size_t available_in = compressed.size();
        const uint8_t* next_in = compressed.data();
        
        std::vector<uint8_t> decompressed;
        decompressed.reserve(compressed.size() * 3); // Reasonable initial capacity
        
        BrotliDecoderResult result;
        
        do {
            size_t available_out = 0;
            uint8_t* next_out = nullptr;
            
            result = BrotliDecoderDecompressStream(
                state, &available_in, &next_in, &available_out, &next_out, nullptr);
            
            // Handle different result states
            if (result == BROTLI_DECODER_RESULT_ERROR) {
                BrotliDecoderDestroyInstance(state);
                BrotliDecoderErrorCode error_code = BrotliDecoderGetErrorCode(state);
                throw std::runtime_error("Brotli decompression error: code " + std::to_string(error_code));
            }
            
            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
                // Extract output data safely
                extractBrotliOutput(state, decompressed);
            }
            
        } while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
        
        // Extract any remaining output
        extractBrotliOutput(state, decompressed);
        
        // Cleanup
        BrotliDecoderDestroyInstance(state);
        
        if (result != BROTLI_DECODER_RESULT_SUCCESS) {
            throw std::runtime_error("Brotli decompression incomplete: result code " + std::to_string(result));
        }
        
        return decompressed;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Brotli decompression internal error: " + std::string(e.what()));
    }
#else
    throw std::runtime_error("Brotli decompression not available - library not compiled");
#endif
}

// ========== Enhanced Analysis and Validation Functions ==========

bool BrotliBackend::isAvailable() {
#ifdef HAVE_BROTLI
    return true;
#else
    return false;
#endif
}

double BrotliBackend::estimateCompressionRatio(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return 1.0;
    
    try {
        validateBrotliParams(level);
        
        // Enhanced empirical compression ratio estimation based on Brotli characteristics
        if (level <= 3) {
            return 0.65; // Fast mode
        } else if (level <= 6) {
            return 0.55; // Balanced mode  
        } else {
            return 0.45; // High compression mode
        }
        
    } catch (const std::exception&) {
        return 1.0; // Conservative estimate on error
    }
}

size_t BrotliBackend::estimateCompressionTime(size_t input_size, int level) {
    if (input_size == 0) return 0;
    
    try {
        validateBrotliParams(level);
        
        // Return estimated time in microseconds
        if (level <= 3) {
            return input_size / 1000;  // ~1MB/s
        } else if (level <= 6) {
            return input_size / 500;   // ~500KB/s
        } else {
            return input_size / 100;   // ~100KB/s
        }
        
    } catch (const std::exception&) {
        return input_size; // Conservative estimate: 1 microsecond per byte
    }
}

// ========== Enhanced Fallback Methods ==========

std::vector<uint8_t> BrotliBackend::fallbackCompress(const std::vector<uint8_t>& data) {
    try {
        // Use RLE compression as fallback with error handling
        return algorithms::RLECompression::rleCompress(data);
    } catch (const std::exception& e) {
        throw std::runtime_error("Brotli fallback compression failed: " + std::string(e.what()));
    }
}

std::vector<uint8_t> BrotliBackend::fallbackDecompress(const std::vector<uint8_t>& compressed) {
    try {
        // Use RLE decompression as fallback with error handling
        return algorithms::RLECompression::rleDecompress(compressed);
    } catch (const std::exception& e) {
        throw std::runtime_error("Brotli fallback decompression failed: " + std::string(e.what()));
    }
}

} // namespace backends
} // namespace compression
} // namespace json2