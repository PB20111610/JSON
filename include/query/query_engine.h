#pragma once

#include "../field_key.h"
#include "../compress_type_aware.h"
#include "../chunked_type_aware_compress.h"
#include "../field_dictionary_manager.h"
#include <string>
#include <vector>
#include <optional>

namespace json2 {
namespace query {

// Forward declarations
class SelectiveDecompressor;

struct QueryConfig {
    bool enable_caching = true;
    size_t max_cache_entries = 1000;
    bool enable_parallel_processing = false;
};

struct FieldFilterResult {
    bool exists = false;
    bool type_matches = false;
    FieldType actual_type = FieldType::STRING;
    std::string error_message;
};

struct ValueFilterResult {
    bool match_found = false;
    size_t count = 0;
    std::vector<std::string> matched_records;
    std::vector<size_t> matched_indices;
    std::string error_message;
};

class QueryEngine {
public:
    virtual ~QueryEngine() = default;
    
    /**
     * Check if a field exists and matches the expected type
     * @param field_name Name of the field to check
     * @param expected_type Expected field type
     * @param granular_data Compressed block data to analyze
     * @return FieldFilterResult containing existence and type information
     */
    virtual FieldFilterResult checkFieldExistenceAndType(
        const std::string& field_name,
        FieldType expected_type,
        const GranularCompressedData& granular_data) = 0;
        
    // Helper function to encode target value using existing dictionary without modifying it
    /*static*/ std::optional<NodeValue> encodeTargetValueWithDictionary(
        const std::string& target_value,
        FieldType field_type,
        const FieldDictionaryManager& dict_manager);
        
    // Helper function to encode target value using dictionary from granular data
    std::optional<NodeValue> encodeTargetValueWithDictionary(
        const std::string& target_value,
        FieldType field_type,
        const GranularCompressedData& granular_data);
        
    /**
     * Filter values for numeric/boolean fields by decompressing layers
     * @param field_name Name of the field to filter
     * @param field_type Type of the field to filter
     * @param target_value Target value to search for
     * @param comparison_op Comparison operator for numeric types
     * @param granular_data Compressed block data to analyze
     * @return ValueFilterResult containing matching records
     */
    virtual ValueFilterResult filterLayerValues(
        const std::string& field_name,
        FieldType field_type,
        const std::string& target_value,
        const std::string& comparison_op,
        const GranularCompressedData& granular_data) = 0;

    /**
     * Combined filtering - first check field existence/type, then filter values
     * @param field_name Name of the field to filter
     * @param expected_type Expected field type
     * @param target_value Target value to search for
     * @param comparison_op Comparison operator for numeric types
     * @param granular_data Compressed block data to analyze
     * @return ValueFilterResult containing matching records
     */
    virtual ValueFilterResult filterFieldAndValues(
        const std::string& field_name,
        FieldType expected_type,
        const std::string& target_value,
        const std::string& comparison_op,
        const GranularCompressedData& granular_data) = 0;

protected:
    // Shared compression configuration for decompression operations
    static const compression::TypeAwareCompressionConfig DEFAULT_COMPRESSION_CONFIG;
    
};

} // namespace query
} // namespace json2