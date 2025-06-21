cmake_minimum_required(VERSION 3.16)
project(json2 CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add our executable and the library sources
add_executable(json2
    src/main.cpp
    src/parser.cpp
    src/dictionary.cpp
    src/trie.cpp
    src/reconstruct.cpp
    vendor/simdjson/simdjson.cpp
)

# Include directories
target_include_directories(json2 PRIVATE 
    include
    vendor/nlohmann
    vendor/simdjson
)

# 复制测试数据到构建目录
add_custom_command(TARGET json2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_SOURCE_DIR}/test_data.json
    ${CMAKE_BINARY_DIR}/test_data.json
)