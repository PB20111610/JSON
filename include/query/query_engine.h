#pragma once

#include "../field_key.h"
#include "../compress_type_aware.h"
#include "../chunked_type_aware_compress.h"
#include "../field_dictionary_manager.h"
#include "query_parser.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace json2 {
namespace query {

// Forward declarations
class SelectiveDecompressor;
class QueryParser;
class QueryNode;

struct QueryConfig {
    bool enable_caching = true;
    size_t max_cache_entries = 1000;
    bool enable_parallel_processing = false;
};

struct FieldFilterResult {
    bool exists = false;
    bool type_matches = false;
    FieldType actual_type = FieldType::STRING;
    int field_index = -1;  // Add field index
    FieldKey field_key;    // Add field key
    std::string error_message;
};

struct ValueFilterResult {
    bool match_found = false;
    size_t count = 0;
    std::vector<std::string> matched_records;
    std::vector<size_t> matched_indices;
    std::string error_message;
};

// Record structure for query results
struct RecordQueryResult {
    std::vector<std::string> records;       // Matching records
    size_t count = 0;                       // Number of matching records
    size_t chunks_accessed = 0;             // Number of chunks accessed
    double decompression_ratio = 0.0;       // Decompression ratio
    double query_time_ms = 0.0;             // Query time (milliseconds)
    bool is_complete = false;               // Whether the query is complete
    std::string error_message;              // Error message
};

// Aggregate query result structure
struct AggregateQueryResult {
    double value = 0.0;                     // Aggregate value
    size_t count = 0;                       // Record count
    std::string aggregate_type;             // Aggregate type
    std::string field_name;                 // Field name
    size_t chunks_accessed = 0;             // Number of chunks accessed
    double query_time_ms = 0.0;             // Query time (milliseconds)
    bool is_complete = false;               // Whether the query is complete
    std::string error_message;              // Error message
};

// Group query result structure
struct GroupQueryResult {
    std::map<std::vector<std::string>, std::vector<std::string>> grouped_records;
    size_t count = 0;                       // Record count
    size_t chunks_accessed = 0;             // Number of chunks accessed
    double query_time_ms = 0.0;             // Query time (milliseconds)
    bool is_complete = false;               // Whether the query is complete
    std::string error_message;              // Error message
};

class QueryEngine {
public:
    // Constructor
    explicit QueryEngine(const QueryConfig& config = QueryConfig{});
    
    // Set the data directory for granular data extraction
    void setDataDirectory(const std::string& data_dir);
    
    virtual ~QueryEngine() = default;
    
    /**
     * Check if a field exists and matches the expected type
     * @param field_name Name of the field to check
     * @param expected_type Expected field type
     * @param granular_data Compressed block data to analyze
     * @param field_order Optional pre-decompressed field order metadata to avoid redundant decompression
     * @return FieldFilterResult containing existence and type information
     */
    FieldFilterResult checkFieldExistenceAndType(
        const std::string& field_name,
        FieldType expected_type,
        const GranularCompressedData& granular_data,
        const std::vector<FieldKey>* field_order = nullptr);
        
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
     * @param target_value Target value to search for (for range queries, use "min,max" format with comparison_op="range")
     * @param comparison_op Comparison operator for numeric types (=, !=, <, >, <=, >=, range)
     * @param granular_data Compressed block data to analyze
     * @param dict_manager Optional pre-decompressed dictionary manager to avoid redundant decompression
     * @return ValueFilterResult containing matching records
     */
    ValueFilterResult filterLayerValues(
        const std::string& field_name,
        FieldType field_type,
        const std::string& target_value,
        const std::string& comparison_op,
        const GranularCompressedData& granular_data,
        const FieldDictionaryManager* dict_manager = nullptr);

    /**
     * Combined filtering - first check field existence/type, then filter values
     * @param field_name Name of the field to filter
     * @param expected_type Expected field type
     * @param target_value Target value to search for
     * @param comparison_op Comparison operator for numeric types
     * @param granular_data Compressed block data to analyze
     * @return ValueFilterResult containing matching records
     */
    ValueFilterResult filterFieldAndValues(
        const std::string& field_name,
        FieldType expected_type,
        const std::string& target_value,
        const std::string& comparison_op,
        const GranularCompressedData& granular_data);
    
    /**
     * Filter values within a range by decompressing layers in a single pass
     * @param field_name Name of the field to filter
     * @param field_type Type of the field to filter
     * @param min_value Minimum value for the range (inclusive)
     * @param max_value Maximum value for the range (inclusive)
     * @param granular_data Compressed block data to analyze
     * @param dict_manager Optional pre-decompressed dictionary manager to avoid redundant decompression
     * @return ValueFilterResult containing matching records
     */
    ValueFilterResult filterLayerValuesInRange(
        const std::string& field_name,
        FieldType field_type,
        const std::string& min_value,
        const std::string& max_value,
        const GranularCompressedData& granular_data,
        const FieldDictionaryManager* dict_manager = nullptr);
    
    /**
     * Execute exact match query by leveraging existing filtering methods in the query engine
     * This function calls the query engine's filtering methods and then reconstructs full records
     * for the matching indices.
     * 
     * @param field_name Name of the field to match
     * @param exact_value Exact value to search for
     * @param expected_type Expected field type
     * @param granular_data Compressed block data
     * @return RecordQueryResult containing matching records
     */
    RecordQueryResult executeExactMatchQuery(
        const std::string& field_name,
        const std::string& exact_value,
        FieldType expected_type,
        const GranularCompressedData& granular_data);
    
    /**
     * Execute exact match query on multiple blocks
     * @param field_name Name of the field to match
     * @param exact_value Exact value to search for
     * @param expected_type Expected field type
     * @param chunks Vector of compressed blocks to query
     * @return RecordQueryResult containing matching records from all blocks
     */
    RecordQueryResult executeExactMatchQueryMultiBlock(
        const std::string& field_name,
        const std::string& exact_value,
        FieldType expected_type,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    /**
     * Execute exact match query on multiple blocks in parallel
     * @param field_name Name of the field to match
     * @param exact_value Exact value to search for
     * @param expected_type Expected field type
     * @param chunks Vector of compressed blocks to query
     * @param thread_count Number of threads to use for parallel processing
     * @return RecordQueryResult containing matching records from all blocks
     */
    RecordQueryResult executeExactMatchQueryMultiBlockParallel(
        const std::string& field_name,
        const std::string& exact_value,
        FieldType expected_type,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = 0);
    
    /**
     * Execute range query by leveraging existing filtering methods in the query engine
     * This function calls the query engine's filtering methods and then reconstructs full records
     * for the matching indices.
     * 
     * @param field_name Name of the field to match
     * @param min_value Minimum value for the range (inclusive)
     * @param max_value Maximum value for the range (inclusive)
     * @param expected_type Expected field type
     * @param granular_data Compressed block data
     * @return RecordQueryResult containing matching records
     */
    RecordQueryResult executeRangeQuery(
        const std::string& field_name,
        const std::string& min_value,
        const std::string& max_value,
        FieldType expected_type,
        const GranularCompressedData& granular_data);
    
    /**
     * Execute range query on multiple blocks
     * @param field_name Name of the field to match
     * @param min_value Minimum value for the range (inclusive)
     * @param max_value Maximum value for the range (inclusive)
     * @param expected_type Expected field type
     * @param chunks Vector of compressed blocks to query
     * @return RecordQueryResult containing matching records from all blocks
     */
    RecordQueryResult executeRangeQueryMultiBlock(
        const std::string& field_name,
        const std::string& min_value,
        const std::string& max_value,
        FieldType expected_type,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    /**
     * Execute range query on multiple blocks in parallel
     * @param field_name Name of the field to match
     * @param min_value Minimum value for the range (inclusive)
     * @param max_value Maximum value for the range (inclusive)
     * @param expected_type Expected field type
     * @param chunks Vector of compressed blocks to query
     * @param thread_count Number of threads to use for parallel processing
     * @return RecordQueryResult containing matching records from all blocks
     */
    RecordQueryResult executeRangeQueryMultiBlockParallel(
        const std::string& field_name,
        const std::string& min_value,
        const std::string& max_value,
        FieldType expected_type,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = 0);

    /**
     * Execute aggregate query by leveraging existing filtering methods in the query engine
     * This function calls the query engine's filtering methods and then computes aggregate values
     * without decompressing full records.
     * 
     * @param aggregate_func Aggregate function to apply (COUNT, SUM, AVG, MAX, MIN)
     * @param field_name Name of the field to aggregate (empty for COUNT(*))
     * @param expected_type Expected field type
     * @param granular_data Compressed block data
     * @return AggregateQueryResult containing the aggregate value
     */
    AggregateQueryResult executeAggregateQuery(
        AggregateFunction aggregate_func,
        const std::string& field_name,
        FieldType expected_type,
        const GranularCompressedData& granular_data);
    
    /**
     * Execute aggregate query on multiple blocks
     * @param aggregate_func Aggregate function to apply (COUNT, SUM, AVG, MAX, MIN)
     * @param field_name Name of the field to aggregate (empty for COUNT(*))
     * @param expected_type Expected field type
     * @param chunks Vector of compressed blocks to query
     * @return AggregateQueryResult containing the aggregate value from all blocks
     */
    AggregateQueryResult executeAggregateQueryMultiBlock(
        AggregateFunction aggregate_func,
        const std::string& field_name,
        FieldType expected_type,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    /**
     * Execute aggregate query on multiple blocks in parallel
     * @param aggregate_func Aggregate function to apply (COUNT, SUM, AVG, MAX, MIN)
     * @param field_name Name of the field to aggregate (empty for COUNT(*))
     * @param expected_type Expected field type
     * @param chunks Vector of compressed blocks to query
     * @param thread_count Number of threads to use for parallel processing
     * @return AggregateQueryResult containing the aggregate value from all blocks
     */
    AggregateQueryResult executeAggregateQueryMultiBlockParallel(
        AggregateFunction aggregate_func,
        const std::string& field_name,
        FieldType expected_type,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = 0);

    /**
     * Execute group query by leveraging existing filtering methods in the query engine
     * This function calls the query engine's filtering methods and then groups records
     * without decompressing full records.
     * 
     * @param group_fields Names of the fields to group by
     * @param group_field_types Expected field types for group fields
     * @param granular_data Compressed block data
     * @return GroupQueryResult containing grouped records
     */
    GroupQueryResult executeGroupQuery(
        const std::vector<std::string>& group_fields,
        const std::vector<FieldType>& group_field_types,
        const GranularCompressedData& granular_data);
    
    /**
     * Execute group query on multiple blocks
     * @param group_fields Names of the fields to group by
     * @param group_field_types Expected field types for group fields
     * @param chunks Vector of compressed blocks to query
     * @return GroupQueryResult containing grouped records from all blocks
     */
    GroupQueryResult executeGroupQueryMultiBlock(
        const std::vector<std::string>& group_fields,
        const std::vector<FieldType>& group_field_types,
        const std::vector<ChunkedTypeAwareBlock>& chunks);
        
    /**
     * Execute group query on multiple blocks in parallel
     * @param group_fields Names of the fields to group by
     * @param group_field_types Expected field types for group fields
     * @param chunks Vector of compressed blocks to query
     * @param thread_count Number of threads to use for parallel processing
     * @return GroupQueryResult containing grouped records from all blocks
     */
    GroupQueryResult executeGroupQueryMultiBlockParallel(
        const std::vector<std::string>& group_fields,
        const std::vector<FieldType>& group_field_types,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = 0);

    // ========== Complex Query Methods ==========

    /**
     * Query result structure for complex queries
     */
    struct QueryResult {
        std::vector<std::string> records;
        size_t count = 0;
        std::string error_message;
    };

    /**
     * Execute complex query with logical operations (AND, OR, NOT)
     * @param complex_query Complex query string with logical operators
     * @param granular_data Compressed block data
     * @return QueryResult containing matching records
     */
    QueryResult executeComplexQuery(
        const std::string& complex_query,
        const GranularCompressedData& granular_data);

    /**
     * Execute complex query on multiple blocks
     * @param complex_query Complex query string with logical operators
     * @param chunks Vector of compressed blocks to query
     * @return QueryResult containing matching records from all blocks
     */
    QueryResult executeComplexQueryMultiBlock(
        const std::string& complex_query,
        const std::vector<ChunkedTypeAwareBlock>& chunks);

    /**
     * Execute complex query on multiple blocks in parallel
     * @param complex_query Complex query string with logical operators
     * @param chunks Vector of compressed blocks to query
     * @param thread_count Number of threads to use for parallel processing
     * @return QueryResult containing matching records from all blocks
     */
    QueryResult executeComplexQueryMultiBlockParallel(
        const std::string& complex_query,
        const std::vector<ChunkedTypeAwareBlock>& chunks,
        size_t thread_count = 0);

protected:
    // Query configuration
    QueryConfig config_;
    
    // Data directory for granular data extraction
    std::string data_dir_;
    
    // Shared compression configuration for decompression operations
    static const compression::TypeAwareCompressionConfig DEFAULT_COMPRESSION_CONFIG;
    
    // Helper function to convert a path to a JSON string representation
    std::string pathToJSONString(const std::vector<std::string>& path, 
                                const std::vector<FieldKey>& field_order) const;

    // Helper function to reconstruct records for specific indices
    QueryResult reconstructRecordsForIndices(
        const std::string& field_name,
        FieldType field_type,
        const std::vector<size_t>& indices,
        const GranularCompressedData& granular_data);

    // Helper functions to merge RecordQueryResult objects from multiple blocks
    void mergeRecordQueryResults(RecordQueryResult& target, const RecordQueryResult& source) const;
    
    // Helper function to extract granular data from a chunk
    GranularCompressedData extractGranularDataFromChunk(const ChunkedTypeAwareBlock& chunk, size_t chunk_index) const;
    
    // Helper function to map json2::FieldType to compression::FieldType
    compression::FieldType mapFieldTypeToCompressionType(FieldType json_field_type) const;
    
    // Helper function to get field index by name
    int getFieldIndex(const std::string& field_name, const std::vector<FieldKey>& field_order) const;
    
    // Helper function to decode node value to string
    std::string decodeNodeValueToString(
        const NodeValue& node_value,
        const FieldKey& field_key,
        const FieldDictionaryManager& dict_manager) const;
        
    // Helper function to decode node value to double
    double decodeNodeValueToDouble(
        const NodeValue& node_value,
        const FieldKey& field_key,
        const FieldDictionaryManager& dict_manager) const;
    
    // ========== Complex Query Helper Methods ==========
    
    // Evaluate a query node (generic entry point)
    QueryResult evaluateQueryNode(const QueryNode* node, const GranularCompressedData& granular_data);
    
    // Evaluate an aggregate node
    QueryResult evaluateAggregateNode(const QueryNode* node, const GranularCompressedData& granular_data);
    
    // Evaluate a group by node
    QueryResult evaluateGroupByNode(const QueryNode* node, const GranularCompressedData& granular_data);
    
    // Evaluate a field node (leaf node in the AST)
    QueryResult evaluateFieldNode(const QueryNode* node, const GranularCompressedData& granular_data);
    
    // Evaluate a logical node (AND, OR, NOT)
    QueryResult evaluateLogicalNode(const QueryNode* node, const GranularCompressedData& granular_data);
    
    // Merge two query results with AND logic (intersection)
    QueryResult mergeResultsAND(const QueryResult& left, const QueryResult& right) const;
    
    // Merge two query results with OR logic (union)
    QueryResult mergeResultsOR(const QueryResult& left, const QueryResult& right) const;
    
    // Negate a query result
    QueryResult negateResult(const QueryResult& result) const;
};
} // namespace query
} // namespace json2