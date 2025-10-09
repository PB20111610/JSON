#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <chrono>

// Include the compression algorithms
#include "../include/compression/algorithms/rle_compression.h"
#include "../include/compression/algorithms/bitpacking_compression.h"
#include "../include/compression/algorithms/dictionary_compression.h"
#include "../include/compression/algorithms/delta_compression.h"
#include "../include/compression/algorithms/varint_compression.h"

using namespace json2::compression::algorithms;

void testRLEPartialDecompression() {
    std::cout << "Testing RLE Partial Decompression...\n";
    
    // Create test data with runs
    std::vector<uint8_t> data = {1, 1, 1, 2, 2, 3, 3, 3, 3, 4};
    
    // Compress the data
    auto compressed = RLECompression::rleCompress(data);
    
    // Test partial decompression
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t value = RLECompression::rleDecompressAt(compressed, i);
        assert(value == data[i]);
        std::cout << "Index " << i << ": " << static_cast<int>(value) << " (expected: " << static_cast<int>(data[i]) << ")\n";
    }
    
    std::cout << "RLE Partial Decompression test passed!\n\n";
}

void testBitPackingPartialDecompression() {
    std::cout << "Testing Bit Packing Partial Decompression...\n";
    
    // Create test boolean data
    std::vector<bool> data = {true, false, true, true, false, false, true, false, true, true};
    
    // Compress the data
    auto compressed = BitPackingCompression::bitPackingCompress(data);
    
    // Test partial decompression
    for (size_t i = 0; i < data.size(); ++i) {
        bool value = BitPackingCompression::bitPackingDecompressAt(compressed, i, data.size());
        assert(value == data[i]);
        std::cout << "Index " << i << ": " << (value ? "true" : "false") << " (expected: " << (data[i] ? "true" : "false") << ")\n";
    }
    
    std::cout << "Bit Packing Partial Decompression test passed!\n\n";
}

void testDictionaryPartialDecompression() {
    std::cout << "Testing Dictionary Partial Decompression...\n";
    
    // Create test string data with repetition
    std::vector<std::string> data = {"apple", "banana", "apple", "cherry", "banana", "apple"};
    
    // Compress the data using an instance
    DictionaryCompression dictCompressor;
    auto compressed = dictCompressor.compressStrings(data);
    
    // Test partial decompression
    for (size_t i = 0; i < data.size(); ++i) {
        try {
            std::string value = dictCompressor.decompressStringAt(compressed, i);
            assert(value == data[i]);
            std::cout << "Index " << i << ": " << value << " (expected: " << data[i] << ")\n";
        } catch (const std::exception& e) {
            std::cout << "Index " << i << ": Error - " << e.what() << "\n";
        }
    }
    
    std::cout << "Dictionary Partial Decompression test completed!\n\n";
}

void testDeltaPartialDecompression() {
    std::cout << "Testing Delta Partial Decompression...\n";
    
    // Create test numeric data
    std::vector<int64_t> data = {100, 105, 110, 108, 115, 120, 118, 125};
    
    // Compress the data using an instance
    DeltaCompression deltaCompressor;
    auto compressed = deltaCompressor.compressInt64(data);
    
    // Test partial decompression
    for (size_t i = 0; i < data.size(); ++i) {
        try {
            int64_t value = deltaCompressor.decompressInt64At(compressed, i);
            assert(value == data[i]);
            std::cout << "Index " << i << ": " << value << " (expected: " << data[i] << ")\n";
        } catch (const std::exception& e) {
            std::cout << "Index " << i << ": Error - " << e.what() << "\n";
        }
    }
    
    std::cout << "Delta Partial Decompression test passed!\n\n";
}

void testDeltaDeltaPartialDecompression() {
    std::cout << "Testing Delta-Delta Partial Decompression...\n";
    
    // Create test timestamp data with regular intervals
    std::vector<int64_t> data = {1000, 1010, 1020, 1030, 1040, 1050, 1060, 1070};
    
    // Compress the data
    auto compressed = DeltaDeltaCompression::compressTimestamps(data);
    
    // Test partial decompression
    for (size_t i = 0; i < data.size(); ++i) {
        try {
            int64_t value = DeltaDeltaCompression::decompressTimestampAt(compressed, i);
            assert(value == data[i]);
            std::cout << "Index " << i << ": " << value << " (expected: " << data[i] << ")\n";
        } catch (const std::exception& e) {
            std::cout << "Index " << i << ": Error - " << e.what() << "\n";
        }
    }
    
    std::cout << "Delta-Delta Partial Decompression test passed!\n\n";
}

void testVarintPartialDecompression() {
    std::cout << "Testing Varint Partial Decompression...\n";
    
    // Create test numeric data
    std::vector<int64_t> data = {100, 1000, 10000, 100000, 1000000, 10000000};
    
    // Compress the data using an instance
    VarintCompression varintCompressor;
    auto compressed = varintCompressor.compressInt64(data);
    
    // Test partial decompression
    for (size_t i = 0; i < data.size(); ++i) {
        try {
            int64_t value = varintCompressor.decompressInt64At(compressed, i);
            assert(value == data[i]);
            std::cout << "Index " << i << ": " << value << " (expected: " << data[i] << ")\n";
        } catch (const std::exception& e) {
            std::cout << "Index " << i << ": Error - " << e.what() << "\n";
        }
    }
    
    std::cout << "Varint Partial Decompression test passed!\n\n";
}

void performanceComparison() {
    std::cout << "Performance Comparison...\n";
    
    // Create large test data
    std::vector<uint8_t> data(10000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = i % 100; // Create repeating pattern
    }
    
    // Compress the data
    auto compressed = RLECompression::rleCompress(data);
    
    // Time full decompression
    auto start = std::chrono::high_resolution_clock::now();
    auto full_result = RLECompression::rleDecompress(compressed);
    auto end = std::chrono::high_resolution_clock::now();
    auto full_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // Time partial decompression for a specific index
    start = std::chrono::high_resolution_clock::now();
    uint8_t partial_result = RLECompression::rleDecompressAt(compressed, 5000);
    end = std::chrono::high_resolution_clock::now();
    auto partial_duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Full decompression time: " << full_duration.count() << " microseconds\n";
    std::cout << "Partial decompression time: " << partial_duration.count() << " microseconds\n";
    std::cout << "Speedup: " << (double)full_duration.count() / partial_duration.count() << "x\n";
    std::cout << "Value at index 5000: " << (int)partial_result << "\n";
    
    std::cout << "Performance comparison completed!\n\n";
}

int main() {
    std::cout << "Partial Decompression Functionality Tests\n";
    std::cout << "========================================\n\n";
    
    try {
        testRLEPartialDecompression();
        testBitPackingPartialDecompression();
        testDictionaryPartialDecompression();
        testDeltaPartialDecompression();
        testDeltaDeltaPartialDecompression();
        testVarintPartialDecompression();
        performanceComparison();
        
        std::cout << "All tests passed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "Test failed with error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}