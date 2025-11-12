#include <iostream>
#include <vector>
#include <cassert>
#include "../vendor/simdjson/simdjson.h"
#include "../include/trie.h"
#include "../include/timestamp_dictionary.h"
#include "../include/logtype_dictionary.h"
#include "../include/compression/algorithms/delta_compression.h"
#include "../include/compression/core/compression_utils.h"

using namespace json2;
using namespace json2::compression;
using namespace json2::compression::algorithms;
using namespace json2::compression::utils;  // Add this line for SerializationUtils

void testStructuredRandomAccess() {
    std::cout << "Testing structured data random access...\n";
    
    // Create test data with very small values that should stay within uint32_t range
    // Even with delta compression
    TemplateEncodedTimestamp ts1{1, {1, 2, 3}};
    TemplateEncodedTimestamp ts2{2, {4, 5}};
    TemplateEncodedTimestamp ts3{3, {6, 7, 8, 9}};
    
    EncodedLog log1{1, {10, 20}};
    EncodedLog log2{2, {30, 40, 50}};
    EncodedLog log3{3, {60}};
    
    // Serialize to bytes correctly
    std::vector<uint8_t> timestamp_data;
    
    // Serialize first timestamp
    timestamp_data.insert(timestamp_data.end(), 
                         reinterpret_cast<const uint8_t*>(&ts1.template_id), 
                         reinterpret_cast<const uint8_t*>(&ts1.template_id) + sizeof(ts1.template_id));
    uint32_t var_count1 = static_cast<uint32_t>(ts1.var_codes.size());
    timestamp_data.insert(timestamp_data.end(), 
                         reinterpret_cast<const uint8_t*>(&var_count1), 
                         reinterpret_cast<const uint8_t*>(&var_count1) + sizeof(var_count1));
    for (uint32_t code : ts1.var_codes) {
        timestamp_data.insert(timestamp_data.end(), 
                             reinterpret_cast<const uint8_t*>(&code), 
                             reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
    }
    
    // Serialize second timestamp
    timestamp_data.insert(timestamp_data.end(), 
                         reinterpret_cast<const uint8_t*>(&ts2.template_id), 
                         reinterpret_cast<const uint8_t*>(&ts2.template_id) + sizeof(ts2.template_id));
    uint32_t var_count2 = static_cast<uint32_t>(ts2.var_codes.size());
    timestamp_data.insert(timestamp_data.end(), 
                         reinterpret_cast<const uint8_t*>(&var_count2), 
                         reinterpret_cast<const uint8_t*>(&var_count2) + sizeof(var_count2));
    for (uint32_t code : ts2.var_codes) {
        timestamp_data.insert(timestamp_data.end(), 
                             reinterpret_cast<const uint8_t*>(&code), 
                             reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
    }
    
    // Serialize third timestamp
    timestamp_data.insert(timestamp_data.end(), 
                         reinterpret_cast<const uint8_t*>(&ts3.template_id), 
                         reinterpret_cast<const uint8_t*>(&ts3.template_id) + sizeof(ts3.template_id));
    uint32_t var_count3 = static_cast<uint32_t>(ts3.var_codes.size());
    timestamp_data.insert(timestamp_data.end(), 
                         reinterpret_cast<const uint8_t*>(&var_count3), 
                         reinterpret_cast<const uint8_t*>(&var_count3) + sizeof(var_count3));
    for (uint32_t code : ts3.var_codes) {
        timestamp_data.insert(timestamp_data.end(), 
                             reinterpret_cast<const uint8_t*>(&code), 
                             reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
    }
    
    // Convert to uint32_t sequence and compress
    std::vector<uint32_t> timestamp_uint32s = SerializationUtils::bytesToUint32s(timestamp_data);
    
    // Print the uint32_t values for debugging
    std::cout << "Timestamp uint32 values: [";
    for (size_t i = 0; i < timestamp_uint32s.size(); ++i) {
        if (i > 0) std::cout << ",";
        std::cout << timestamp_uint32s[i];
    }
    std::cout << "]" << std::endl;
    
    std::vector<int64_t> timestamp_int64s(timestamp_uint32s.begin(), timestamp_uint32s.end());
    std::vector<uint8_t> compressed_timestamps = DeltaCompression::deltaVarintCompress(timestamp_int64s);
    
    // Test random access for timestamps
    TemplateEncodedTimestamp retrieved_ts1 = DeltaCompression::deltaVarintDecompressTimestampAt(compressed_timestamps, 0);
    TemplateEncodedTimestamp retrieved_ts2 = DeltaCompression::deltaVarintDecompressTimestampAt(compressed_timestamps, 1);
    TemplateEncodedTimestamp retrieved_ts3 = DeltaCompression::deltaVarintDecompressTimestampAt(compressed_timestamps, 2);
    
    assert(retrieved_ts1.template_id == ts1.template_id);
    assert(retrieved_ts1.var_codes == ts1.var_codes);
    assert(retrieved_ts2.template_id == ts2.template_id);
    assert(retrieved_ts2.var_codes == ts2.var_codes);
    assert(retrieved_ts3.template_id == ts3.template_id);
    assert(retrieved_ts3.var_codes == ts3.var_codes);
    
    std::cout << "Timestamp random access test passed!\n";
    
    // Test logtype random access
    std::vector<uint8_t> log_data;
    
    // Serialize first log
    log_data.insert(log_data.end(), 
                   reinterpret_cast<const uint8_t*>(&log1.template_id), 
                   reinterpret_cast<const uint8_t*>(&log1.template_id) + sizeof(log1.template_id));
    uint32_t log_var_count1 = static_cast<uint32_t>(log1.var_codes.size());
    log_data.insert(log_data.end(), 
                   reinterpret_cast<const uint8_t*>(&log_var_count1), 
                   reinterpret_cast<const uint8_t*>(&log_var_count1) + sizeof(log_var_count1));
    for (uint32_t code : log1.var_codes) {
        log_data.insert(log_data.end(), 
                       reinterpret_cast<const uint8_t*>(&code), 
                       reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
    }
    
    // Serialize second log
    log_data.insert(log_data.end(), 
                   reinterpret_cast<const uint8_t*>(&log2.template_id), 
                   reinterpret_cast<const uint8_t*>(&log2.template_id) + sizeof(log2.template_id));
    uint32_t log_var_count2 = static_cast<uint32_t>(log2.var_codes.size());
    log_data.insert(log_data.end(), 
                   reinterpret_cast<const uint8_t*>(&log_var_count2), 
                   reinterpret_cast<const uint8_t*>(&log_var_count2) + sizeof(log_var_count2));
    for (uint32_t code : log2.var_codes) {
        log_data.insert(log_data.end(), 
                       reinterpret_cast<const uint8_t*>(&code), 
                       reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
    }
    
    // Serialize third log
    log_data.insert(log_data.end(), 
                   reinterpret_cast<const uint8_t*>(&log3.template_id), 
                   reinterpret_cast<const uint8_t*>(&log3.template_id) + sizeof(log3.template_id));
    uint32_t log_var_count3 = static_cast<uint32_t>(log3.var_codes.size());
    log_data.insert(log_data.end(), 
                   reinterpret_cast<const uint8_t*>(&log_var_count3), 
                   reinterpret_cast<const uint8_t*>(&log_var_count3) + sizeof(log_var_count3));
    for (uint32_t code : log3.var_codes) {
        log_data.insert(log_data.end(), 
                       reinterpret_cast<const uint8_t*>(&code), 
                       reinterpret_cast<const uint8_t*>(&code) + sizeof(code));
    }
    
    // Convert to uint32_t sequence and compress
    std::vector<uint32_t> log_uint32s = SerializationUtils::bytesToUint32s(log_data);
    
    // Print the uint32_t values for debugging
    std::cout << "Log uint32 values: [";
    for (size_t i = 0; i < log_uint32s.size(); ++i) {
        if (i > 0) std::cout << ",";
        std::cout << log_uint32s[i];
    }
    std::cout << "]" << std::endl;
    
    std::vector<int64_t> log_int64s(log_uint32s.begin(), log_uint32s.end());
    std::vector<uint8_t> compressed_logs = DeltaCompression::deltaVarintCompress(log_int64s);
    
    // Test random access for logs
    EncodedLog retrieved_log1 = DeltaCompression::deltaVarintDecompressLogtypeAt(compressed_logs, 0);
    EncodedLog retrieved_log2 = DeltaCompression::deltaVarintDecompressLogtypeAt(compressed_logs, 1);
    EncodedLog retrieved_log3 = DeltaCompression::deltaVarintDecompressLogtypeAt(compressed_logs, 2);
    
    assert(retrieved_log1.template_id == log1.template_id);
    assert(retrieved_log1.var_codes == log1.var_codes);
    assert(retrieved_log2.template_id == log2.template_id);
    assert(retrieved_log2.var_codes == log2.var_codes);
    assert(retrieved_log3.template_id == log3.template_id);
    assert(retrieved_log3.var_codes == log3.var_codes);
    
    std::cout << "Logtype random access test passed!\n";
    std::cout << "All structured random access tests passed!\n";
}

int main() {
    try {
        testStructuredRandomAccess();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
}