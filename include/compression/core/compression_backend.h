#pragma once

#include <cstdint>

namespace json2 {
namespace compression {

/**
 * 压缩专用字段类型枚举
 */
enum class FieldType : uint8_t {
    INT64 = 0,        // 64位整数
    UINT32 = 1,       // 32位无符号整数
    DOUBLE = 2,       // 双精度浮点数
    BOOL = 3,         // 布尔值
    STRING = 4,       // 字符串
    TIMESTAMP = 5,    // 时间戳
    LOGTYPE = 6,      // 日志类型
    ARRAY = 7,        // 数组
    OBJECT = 8,       // 对象
    NULL_TYPE = 9     // 空值
};

/**
 * 压缩方法枚举
 */
enum class CompressionMethod : uint8_t {
    NONE = 0,         // 不压缩
    RLE = 1,          // 游程编码
    VARINT = 2,       // 可变长整数编码
    DELTA = 3,        // Delta压缩
    BITPACKING = 4,   // 位打包
    DICTIONARY = 5,   // 字典压缩
    BACKEND = 6       // 外部压缩库
};

/**
 * 压缩后端类型 - 支持用户指定不同压缩库
 */
enum class CompressionBackend : uint8_t {
    AUTO = 0,         // 自动根据数据类型选择最优算法
    RLE = 1,          // 游程编码
    BIT_PACKING = 2,  // 位打包
    DICTIONARY = 3,   // 字典压缩
    DELTA_VARINT = 4, // Delta+Varint编码
    DELTA_DELTA = 5,  // Delta-of-delta编码
    ZSTD = 6,         // Zstandard压缩库
    BROTLI = 7,       // Brotli压缩库
    LZMA = 8,         // LZMA压缩库
    LZ4 = 9,          // LZ4压缩库（快速）
    SNAPPY = 10,      // Snappy压缩库（快速）
    NONE = 255        // 不压缩
};

/**
 * 类型感知压缩配置 - 支持针对不同数据类型特点的压缩策略
 */
struct TypeAwareCompressionConfig {
    // LOUDS位图压缩 (0/1位序列，推荐RLE)
    CompressionBackend louds_backend = CompressionBackend::RLE;
    
    // FieldKey序列压缩 (二元组字符串列表，推荐字典+ZSTD)
    CompressionBackend fieldkey_backend = CompressionBackend::DICTIONARY;
    
    // 字典数据压缩 (String/Timestamp/Logtype模板+字典，推荐ZSTD/Brotli)
    CompressionBackend dictionary_backend = CompressionBackend::ZSTD;
    
    // 元数据压缩 (结构化元信息，推荐ZSTD)
    CompressionBackend metadata_backend = CompressionBackend::ZSTD;
    
    // 分层数据压缩 - 根据FieldType分别配置
    struct LayerCompressionConfig {
        CompressionBackend int_backend = CompressionBackend::DELTA_VARINT;     // 整数: Delta+Varint
        CompressionBackend double_backend = CompressionBackend::DELTA_VARINT;  // 浮点数: Delta+Varint
        CompressionBackend bool_backend = CompressionBackend::BIT_PACKING;     // 布尔: 位打包
        CompressionBackend string_backend = CompressionBackend::DELTA_VARINT;  // 字符串编码值: Delta+Varint
        CompressionBackend timestamp_backend = CompressionBackend::DELTA_DELTA; // 时间戳: Delta-of-delta
        CompressionBackend logtype_backend = CompressionBackend::DELTA_VARINT;  // 日志类型编码值: Delta+Varint
        CompressionBackend array_backend = CompressionBackend::RLE;             // 数组编码值: RLE
        CompressionBackend null_backend = CompressionBackend::BIT_PACKING;      // Null掩码: 位打包
    } layer_config;
    
    // 全局备用策略
    CompressionBackend fallback_backend = CompressionBackend::ZSTD;
    
    // 是否启用Null感知压缩（处理稀疏数据）
    bool enable_null_aware = true;
    
    // 压缩级别（对支持的算法）
    int compression_level = 6;  // 1-22 for ZSTD, 0-11 for Brotli
};

} // namespace compression
} // namespace json2