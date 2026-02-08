# JSON Compression and Query Engine

## Project Overview

This project implements an advanced JSON compression framework with selective query capabilities that enables efficient storage and querying of compressed JSON data with minimal decompression overhead. The system combines type-aware compression techniques with granular data access patterns to achieve high compression ratios while maintaining query performance.

## Core Features

### 1. Type-Aware Compression
- **Granular Compression**: Separates data by field types (INT64, DOUBLE, BOOL, STRING, TIMESTAMP, LOGTYPE, ARRAY, NULL)
- **Layer Separation**: Stores different data types in separate compressed layers
- **Adaptive Algorithms**: Uses optimal compression algorithms for each data type:
  - Bit-packing for boolean and null values
  - Delta-varint for numeric and timestamp data
  - RLE for array data
  - ZSTD for dictionary compression

### 2. Selective Query Engine
- **Metadata-Only Queries**: Field existence and type checking without data decompression
- **Dictionary-Level Queries**: Value lookup in compressed dictionaries
- **Selective Decompression**: Only decompress required data components
- **Complex Query Support**: AND/OR/NOT operations with parentheses grouping

### 3. Efficient Data Structures
- **Trie-based Storage**: Path-compressed trie structure for JSON field paths
- **LOUDS Bitmap**: Level-Order Unary Degree Sequence for efficient trie navigation
- **Multi-Dictionary Management**: Separate dictionaries for strings, timestamps, and log templates
- **Chunked Storage**: Data organized in blocks and chunks for scalable processing

## Project Structure

```
.
├── include/                    # Header files
│   ├── compression/           # Compression algorithms and backends
│   ├── query/                # Query engine components
│   └── core data structures  # Trie, dictionaries, etc.
├── src/                      # Implementation files
├── test/                     # Test suite and experimental code
│   ├── test_granular_type_aware_chunked_cmp.cpp  # Core compression test
│   ├── test_parameterized_query.cpp              # Query testing
│   └── other test files
└── docs/                     # Documentation
```

## Core Test Files

### 1. test_granular_type_aware_chunked_cmp.cpp
This is the **primary compression test** that demonstrates the core functionality:

**Performance Characteristics:**
- Focuses on correctness rather than speed
- May be slower than optimized production implementations
- Designed for research and validation purposes

**Key Features Tested:**
- Granular type-aware compression with layer separation
- Configurable compression backends (ZSTD, Bit-packing, Delta-varint, etc.)
- Block-based processing for memory efficiency
- Detailed compression statistics and ratio analysis
- Data reconstruction and verification

**Usage Example:**
```bash
./test_granular_type_aware_chunked_cmp --test-file-path data.json --block_size 20000 --chunk_size 1000
```

**Configuration Options:**
- `--test-file-path`: Input JSON file path
- `--block_size`: Number of records per block (default: 20000)
- `--chunk_size`: Records per chunk (default: 1000)
- `--num_limit`: Limit number of records to process (0 = unlimited)

### 2. test_parameterized_query.cpp
This implements the **query testing framework** that demonstrates selective querying capabilities:

**Supported Query Types:**
- **Field Existence**: `--field field_name`
- **Dictionary Queries**: `--dict field_name`
- **Exact Match**: `--point field_name value`
- **Range Queries**: `--range field_name min max`
- **Aggregate Functions**: `--aggregate COUNT/SUM/AVG/MAX/MIN [field]`
- **Complex Queries**: `--complex "field1:value1 AND field2:value2"`
- **Group By**: `--group field1 field2`

**Usage Example:**
```bash
./test_parameterized_query --data-dir compressed_type_aware_data --field user --point user postgres --aggregate COUNT
```

## Query Documentation

See [Query.md](Query.md) for comprehensive documentation on:
- Query syntax and examples
- Performance optimization strategies
- Implementation details
- Query execution architecture
- Performance benchmarks and metrics

## Dataset

The experimental dataset used for testing is available at:
**https://zenodo.org/records/18522101**

This dataset contains real-world JSON log data suitable for evaluating compression ratios and query performance.

## Key Components

### Compression System
- **ChunkedTypeAwareCompressor**: Main compression engine with type-aware strategies
- **FieldDictionaryManager**: Manages string, timestamp, and logtype dictionaries
- **Trie Structure**: Stores JSON field paths with path compression
- **LOUDS Bitmap**: Efficient trie navigation structure

### Query Engine
- **QueryEngine**: Main query processing class
- **Selective Decompressor**: Decompresses only required data components
- **Field Analyzer**: Analyzes query fields and types
- **Result Rebuilder**: Reconstructs JSON from compressed data

## Build Instructions

```bash
mkdir build
cd build
cmake ..
make
```

## Usage Examples

### 1. Compression Workflow
```bash
# Compress JSON data
./test_granular_type_aware_chunked_cmp --test-file-path input.json

# Output files:
# - compressed_type_aware_data/ (compressed data directory)
# - result_type_aware_chunked.json (compression statistics)
# - chunked_type_aware_reconstructed.json (verification output)
```

### 2. Query Workflow
```bash
# Run various queries on compressed data
./test_parameterized_query --data-dir compressed_type_aware_data \
    --field user \
    --point user postgres \
    --range pid 7880 7890 \
    --aggregate COUNT \
    --complex "user:postgres AND dbname:example"
```

## Architecture Highlights

### 1. Type-Aware Compression Strategy
The system automatically identifies field types and applies optimal compression:
- **Strings**: Dictionary compression with ZSTD
- **Integers**: Delta encoding with variable-length integers
- **Timestamps**: Template-based compression
- **Booleans**: Bit-packing for maximum density
- **Arrays**: Run-length encoding for repeated values

### 2. Selective Access Patterns
Queries execute with minimal data movement:
- **Field existence**: Only metadata is accessed
- **Value queries**: Only relevant dictionaries are decompressed
- **Record retrieval**: Only required layers are processed
- **Complex queries**: Efficient pruning of search space

### 3. Scalable Design
- **Block-based processing**: Handles large files without memory issues
- **Parallel processing**: Multi-core support for compression and queries
- **Incremental processing**: Process data in chunks for streaming scenarios

## Research Applications

This framework is particularly useful for:
- **Log analysis**: Efficient storage and querying of log data
- **Time-series data**: Optimized handling of timestamp-based records
- **Large-scale analytics**: Processing massive JSON datasets with minimal resources
- **Real-time systems**: Fast query responses for interactive applications

## Future Enhancements

- **Distributed processing**: Multi-node compression and querying
- **Advanced indexing**: More sophisticated query optimization
- **Streaming support**: Real-time compression and query processing
- **Machine learning integration**: Adaptive compression based on data patterns

## License

This project is for research and educational purposes. See individual files for specific licensing information.