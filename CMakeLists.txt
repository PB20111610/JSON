cmake_minimum_required(VERSION 3.16)
project(json2 CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找Zstandard库
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED libzstd)

# 定义ZSTD宏
add_definitions(-DUSE_ZSTD)

# Add our executable and the library sources
add_executable(json2
    src/main_trie.cpp
    src/parser.cpp
    src/variable_dictionary.cpp
    src/timestamp_dictionary.cpp
    src/logtype_dictionary.cpp
    src/field_dictionary_manager.cpp 
    src/trie.cpp
    src/reconstruct.cpp
    vendor/simdjson/simdjson.cpp
)

# 链接库
target_link_libraries(json2
    PRIVATE
        ${ZSTD_LIBRARIES}
)
target_include_directories(json2 PRIVATE ${LZ4_INCLUDE_DIRS} ${ZSTD_INCLUDE_DIRS} ${SNAPPY_INCLUDE_DIRS})

# Include directories
target_include_directories(json2 PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)

target_include_directories(json2 PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/vendor"
)