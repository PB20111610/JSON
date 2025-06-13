cmake_minimum_required(VERSION 3.10)
project(JSON2)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加头文件目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# 添加源文件
set(SOURCES
    src/main.cpp
    src/parser.cpp
    src/dictionary.cpp
)

# 创建可执行文件
add_executable(json2 ${SOURCES})

# 复制测试数据到构建目录
configure_file(${CMAKE_SOURCE_DIR}/test_data.json
               ${CMAKE_BINARY_DIR}/test_data.json COPYONLY)