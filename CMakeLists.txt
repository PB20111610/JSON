cmake_minimum_required(VERSION 3.16)
project(json2 CXX)
include_directories(/usr/include)
link_directories(/usr/lib)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找Zstandard库
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED libzstd)

# 定义ZSTD宏
add_definitions(-DUSE_ZSTD)

# Compression executable for granular type aware chunked compression
add_executable(json2
    test/test_granular_type_aware_chunked_cmp.cpp
    src/field_analyzer.cpp
    src/field_parser.cpp
    src/variable_dictionary.cpp
    src/timestamp_dictionary.cpp
    src/logtype_dictionary.cpp
    src/field_dictionary_manager.cpp 
    src/trie.cpp
    src/louds.cpp
    src/loudsTotrie.cpp
    src/reconstruct.cpp
    src/compress.cpp
    src/chunked_compressor.cpp
    src/compress_type_aware.cpp
    src/chunked_type_aware_compress.cpp
    vendor/simdjson/simdjson.cpp
    # New modular compression system files
    include/compression/core/compression_utils.cpp
    include/compression/algorithms/rle_compression.cpp
    include/compression/algorithms/varint_compression.cpp
    include/compression/algorithms/delta_compression.cpp
    include/compression/algorithms/bitpacking_compression.cpp
    include/compression/algorithms/dictionary_compression.cpp
    include/compression/backends/zstd_backend.cpp
    include/compression/backends/brotli_backend.cpp
    include/compression/backends/lzma_backend.cpp
    include/compression/backends/lz4_backend.cpp
    include/compression/backends/snappy_backend.cpp
    include/compression/type_aware/type_aware_compressor.cpp
    include/compression/type_aware/layer_compressor.cpp
    include/compression/type_aware/metadata_compressor.cpp
    include/compression/factory/compression_factory.cpp
)

# Compression executable for granular chunked compression
add_executable(chunked_compressor
    test/test_granular_chunked_cmp.cpp
    src/field_analyzer.cpp
    src/field_parser.cpp
    src/variable_dictionary.cpp
    src/timestamp_dictionary.cpp
    src/logtype_dictionary.cpp
    src/field_dictionary_manager.cpp 
    src/trie.cpp
    src/louds.cpp
    src/loudsTotrie.cpp
    src/reconstruct.cpp
    src/compress.cpp
    src/chunked_compressor.cpp
    src/compress_type_aware.cpp
    src/chunked_type_aware_compress.cpp
    vendor/simdjson/simdjson.cpp
    # New modular compression system files
    include/compression/core/compression_utils.cpp
    include/compression/algorithms/rle_compression.cpp
    include/compression/algorithms/varint_compression.cpp
    include/compression/algorithms/delta_compression.cpp
    include/compression/algorithms/bitpacking_compression.cpp
    include/compression/algorithms/dictionary_compression.cpp
    include/compression/backends/zstd_backend.cpp
    include/compression/backends/brotli_backend.cpp
    include/compression/backends/lzma_backend.cpp
    include/compression/backends/lz4_backend.cpp
    include/compression/backends/snappy_backend.cpp
    include/compression/type_aware/type_aware_compressor.cpp
    include/compression/type_aware/layer_compressor.cpp
    include/compression/type_aware/metadata_compressor.cpp
    include/compression/factory/compression_factory.cpp
)

# Parameterized query test executable
add_executable(parameterized_query
    test/test_parameterized_query.cpp
    src/query/query_engine.cpp
    src/query/field_analyzer.cpp
    src/query/query_parser.cpp
    src/query/chunk_selector.cpp
    src/query/selective_decompressor.cpp
    src/query/trie_traverser.cpp
    src/query/result_rebuilder.cpp
    src/query/path_cache.cpp
    src/field_analyzer.cpp
    src/field_parser.cpp
    src/variable_dictionary.cpp
    src/timestamp_dictionary.cpp
    src/logtype_dictionary.cpp
    src/field_dictionary_manager.cpp 
    src/trie.cpp
    src/louds.cpp
    src/loudsTotrie.cpp
    src/reconstruct.cpp
    src/compress.cpp
    src/compress_type_aware.cpp
    src/chunked_type_aware_compress.cpp
    vendor/simdjson/simdjson.cpp
    include/compression/core/compression_utils.cpp
    include/compression/algorithms/rle_compression.cpp
    include/compression/algorithms/varint_compression.cpp
    include/compression/algorithms/delta_compression.cpp
    include/compression/algorithms/bitpacking_compression.cpp
    include/compression/algorithms/dictionary_compression.cpp
    include/compression/backends/zstd_backend.cpp
    include/compression/backends/brotli_backend.cpp
    include/compression/backends/lzma_backend.cpp
    include/compression/backends/lz4_backend.cpp
    include/compression/backends/snappy_backend.cpp
    include/compression/type_aware/type_aware_compressor.cpp
    include/compression/type_aware/layer_compressor.cpp
    include/compression/type_aware/metadata_compressor.cpp
    include/compression/factory/compression_factory.cpp
)

# Field queries test executable
add_executable(query
    test/test_query_performance.cpp
    src/query/query_engine.cpp
    src/query/field_analyzer.cpp
    src/query/query_parser.cpp
    src/query/chunk_selector.cpp
    src/query/selective_decompressor.cpp
    src/query/trie_traverser.cpp
    src/query/result_rebuilder.cpp
    src/query/path_cache.cpp
    src/field_analyzer.cpp
    src/field_parser.cpp
    src/variable_dictionary.cpp
    src/timestamp_dictionary.cpp
    src/logtype_dictionary.cpp
    src/field_dictionary_manager.cpp 
    src/trie.cpp
    src/louds.cpp
    src/loudsTotrie.cpp
    src/reconstruct.cpp
    src/compress.cpp
    src/compress_type_aware.cpp
    src/chunked_type_aware_compress.cpp
    vendor/simdjson/simdjson.cpp
    include/compression/core/compression_utils.cpp
    include/compression/algorithms/rle_compression.cpp
    include/compression/algorithms/varint_compression.cpp
    include/compression/algorithms/delta_compression.cpp
    include/compression/algorithms/bitpacking_compression.cpp
    include/compression/algorithms/dictionary_compression.cpp
    include/compression/backends/zstd_backend.cpp
    include/compression/backends/brotli_backend.cpp
    include/compression/backends/lzma_backend.cpp
    include/compression/backends/lz4_backend.cpp
    include/compression/backends/snappy_backend.cpp
    include/compression/type_aware/type_aware_compressor.cpp
    include/compression/type_aware/layer_compressor.cpp
    include/compression/type_aware/metadata_compressor.cpp
    include/compression/factory/compression_factory.cpp
)

# Parallel query test executable
add_executable(parallel_query
    test/test_parallel_query_engine.cpp
    src/query/query_engine.cpp
    src/query/field_analyzer.cpp
    src/query/query_parser.cpp
    src/query/chunk_selector.cpp
    src/query/selective_decompressor.cpp
    src/query/trie_traverser.cpp
    src/query/result_rebuilder.cpp
    src/query/path_cache.cpp
    src/field_analyzer.cpp
    src/field_parser.cpp
    src/variable_dictionary.cpp
    src/timestamp_dictionary.cpp
    src/logtype_dictionary.cpp
    src/field_dictionary_manager.cpp 
    src/trie.cpp
    src/louds.cpp
    src/loudsTotrie.cpp
    src/reconstruct.cpp
    src/compress.cpp
    src/compress_type_aware.cpp
    src/chunked_type_aware_compress.cpp
    vendor/simdjson/simdjson.cpp
    include/compression/core/compression_utils.cpp
    include/compression/algorithms/rle_compression.cpp
    include/compression/algorithms/varint_compression.cpp
    include/compression/algorithms/delta_compression.cpp
    include/compression/algorithms/bitpacking_compression.cpp
    include/compression/algorithms/dictionary_compression.cpp
    include/compression/backends/zstd_backend.cpp
    include/compression/backends/brotli_backend.cpp
    include/compression/backends/lzma_backend.cpp
    include/compression/backends/lz4_backend.cpp
    include/compression/backends/snappy_backend.cpp
    include/compression/type_aware/type_aware_compressor.cpp
    include/compression/type_aware/layer_compressor.cpp
    include/compression/type_aware/metadata_compressor.cpp
    include/compression/factory/compression_factory.cpp
)

# Raw JSON baseline query test executable
add_executable(baseline_query
    test/baseline_raw_json_query.cpp
    vendor/simdjson/simdjson.cpp
)

# Field type statistics test executable
add_executable(field_type_statistics
    test/test_field_type_statistics.cpp
    src/field_parser.cpp
    src/field_dictionary_manager.cpp
    src/variable_dictionary.cpp
    src/timestamp_dictionary.cpp
    src/logtype_dictionary.cpp
    vendor/simdjson/simdjson.cpp
)

# 链接库
target_link_libraries(json2
    PRIVATE
        ${ZSTD_LIBRARIES}
        sdsl
)
target_link_libraries(chunked_compressor
    PRIVATE
        ${ZSTD_LIBRARIES}
        sdsl
)
target_link_libraries(parameterized_query
    PRIVATE
        ${ZSTD_LIBRARIES}
        sdsl
)
target_link_libraries(query
    PRIVATE
        ${ZSTD_LIBRARIES}
        sdsl
)

target_link_libraries(parallel_query
    PRIVATE
        ${ZSTD_LIBRARIES}
        sdsl
)

# Link libraries for baseline query
target_link_libraries(baseline_query
    PRIVATE
        ${ZSTD_LIBRARIES}
        sdsl
)

# Link libraries for field type statistics
target_link_libraries(field_type_statistics
    PRIVATE
        ${ZSTD_LIBRARIES}
        sdsl
)

target_include_directories(json2 PRIVATE ${LZ4_INCLUDE_DIRS} ${ZSTD_INCLUDE_DIRS} ${SNAPPY_INCLUDE_DIRS})
target_include_directories(chunked_compressor PRIVATE ${LZ4_INCLUDE_DIRS} ${ZSTD_INCLUDE_DIRS} ${SNAPPY_INCLUDE_DIRS})
target_include_directories(parameterized_query PRIVATE ${LZ4_INCLUDE_DIRS} ${ZSTD_INCLUDE_DIRS} ${SNAPPY_INCLUDE_DIRS})
target_include_directories(query PRIVATE ${LZ4_INCLUDE_DIRS} ${ZSTD_INCLUDE_DIRS} ${SNAPPY_INCLUDE_DIRS})
target_include_directories(parallel_query PRIVATE ${LZ4_INCLUDE_DIRS} ${ZSTD_INCLUDE_DIRS} ${SNAPPY_INCLUDE_DIRS})

# Include directories
target_include_directories(json2 PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)
target_include_directories(chunked_compressor PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)
target_include_directories(parameterized_query PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)
target_include_directories(query PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)

target_include_directories(json2 PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)
target_include_directories(chunked_compressor PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)
target_include_directories(parameterized_query PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)
target_include_directories(query PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)
target_include_directories(parallel_query PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)

target_include_directories(parallel_query PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)

# Include directories for baseline query
target_include_directories(baseline_query PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)

target_include_directories(baseline_query PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)

# Include directories for field type statistics
target_include_directories(field_type_statistics PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)

target_include_directories(field_type_statistics PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)