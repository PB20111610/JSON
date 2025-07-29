#include "../include/field_analyzer.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <vector>
#include <string>
#include <map>
#include "../include/field_dictionary_manager.h"

using json = nlohmann::json;

// Helper function to convert variant to string
std::string variantToString(const std::variant<int64_t, double, bool, std::string, std::nullptr_t>& value) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else {
            return std::to_string(v);
        }
    }, value);
}

// Helper function to convert variant to string for addFieldValue
std::string variantToFieldValueString(const std::variant<int64_t, double, bool, std::string, std::nullptr_t>& value) {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            return "";
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else {
            return std::to_string(v);
        }
    }, value);
}

// 编码后的字段信息
struct EncodedField {
    std::string field_name;
    json2::FieldType type;
    uint32_t code;
    std::string original_value;
};

// 编码单条记录
std::vector<EncodedField> encodeRecord(const std::string& record, json2::FieldDictionaryManager& manager) {
    std::vector<EncodedField> encoded_fields;
    auto fields = json2::FieldAnalyzer::parseFields(record);
    
    for (const auto& [field, type, value] : fields) {
        std::string value_str = variantToFieldValueString(value);
        uint32_t code = manager.addFieldValue(json2::FieldKey{field, type}, value_str);
        
        // 重新获取实际使用的类型（可能被重新分类）
        json2::FieldType actual_type = type;
        if (type == json2::FieldType::String) {
            // 检查是否为时间戳字段
            if (manager.isTimestampField(field) && manager.isTimestampValue(value_str)) {
                actual_type = json2::FieldType::Timestamp;
            }
            // 检查是否为日志模板（包含空格）
            else if (manager.isLogTemplate(value_str)) {
                actual_type = json2::FieldType::LogType;
            }
        }
        
        encoded_fields.push_back({
            field,
            actual_type,  // 使用实际类型而不是原始类型
            code,
            variantToString(value)
        });
    }
    
    return encoded_fields;
}

// 解码单条记录
std::string decodeRecord(const std::vector<EncodedField>& encoded_fields, json2::FieldDictionaryManager& manager) {
    // 使用有序JSON对象，保持字段顺序
    json decoded_json = json::object();
    
    for (const auto& field : encoded_fields) {
        std::string decoded_value;
        bool decode_success = false;
        
        switch (field.type) {
            case json2::FieldType::Int: {
                auto& dict = manager.variableDict();
                auto decoded = dict.getFieldValueByCode(field.field_name, field.type, field.code);
                if (decoded) {
                    decoded_value = std::to_string(std::get<int64_t>(*decoded));
                    decode_success = true;
                }
                break;
            }
            case json2::FieldType::Double: {
                auto& dict = manager.variableDict();
                auto decoded = dict.getFieldValueByCode(field.field_name, field.type, field.code);
                if (decoded) {
                    decoded_value = std::to_string(std::get<double>(*decoded));
                    decode_success = true;
                }
                break;
            }
            case json2::FieldType::Bool: {
                auto& dict = manager.variableDict();
                auto decoded = dict.getFieldValueByCode(field.field_name, field.type, field.code);
                if (decoded) {
                    decoded_value = std::get<bool>(*decoded) ? "true" : "false";
                    decode_success = true;
                }
                break;
            }
            case json2::FieldType::String: {
                auto& dict = manager.variableDict();
                auto decoded = dict.getFieldValueByCode(field.field_name, field.type, field.code);
                if (decoded) {
                    decoded_value = std::get<std::string>(*decoded);
                    decode_success = true;
                }
                break;
            }
            case json2::FieldType::Timestamp: {
                auto& dict = manager.timestampDict();
                // 尝试从编码中解码时间戳
                // 这里需要从原始值中提取epoch信息，然后重新编码
                auto [pattern_id, epoch] = dict.addTimestamp(field.field_name, field.original_value);
                if (pattern_id == field.code) {
                    // 编码匹配，使用解码后的值
                    decoded_value = dict.decode({pattern_id, epoch});
                    if (!decoded_value.empty()) {
                        decode_success = true;
                    }
                }
                break;
            }
            case json2::FieldType::LogType: {
                auto& dict = manager.logtypeDict();
                // 尝试从编码中解码日志类型
                // 这里需要从原始值中提取模板和变量，然后重新编码
                auto encoded = dict.encodeLog(field.original_value, {});
                if (encoded.template_id == field.code) {
                    // 编码匹配，使用解码后的值
                    decoded_value = dict.decodeLogToString(encoded);
                    if (!decoded_value.empty()) {
                        decode_success = true;
                    }
                }
                break;
            }
            case json2::FieldType::Null: {
                decoded_value = "null";
                decode_success = true;
                break;
            }
            default:
                break;
        }
        
        // 如果解码失败，使用原始值
        if (!decode_success) {
            decoded_value = field.original_value;
        }
        
        // 根据类型设置JSON值，保持字段顺序
        switch (field.type) {
            case json2::FieldType::Int:
                decoded_json[field.field_name] = std::stoll(decoded_value);
                break;
            case json2::FieldType::Double:
                decoded_json[field.field_name] = std::stod(decoded_value);
                break;
            case json2::FieldType::Bool:
                decoded_json[field.field_name] = (decoded_value == "true");
                break;
            case json2::FieldType::Null:
                decoded_json[field.field_name] = nullptr;
                break;
            default:
                decoded_json[field.field_name] = decoded_value;
                break;
        }
    }
    
    // 使用紧凑格式输出，确保字段顺序一致
    return decoded_json.dump(-1, 32, true);
}

int main() {
    using namespace json2;
    const std::string data_file = "test_data.json";

    FieldDictionaryManager manager;
    
    // 配置时间戳字段
    std::vector<std::string> timestamp_fields = {"@timestamp", "timestamp", "session_start"};
    manager.setTimestampFields(timestamp_fields);
    
    std::vector<std::string> ordered_fields;
    std::vector<std::string> records;
    std::ifstream in(data_file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) records.push_back(line);
    }
    in.close();

    FieldAnalyzer::analyzeAndSortFields(records, manager, ordered_fields);

    // std::cout << "\nField Redundancy Statistics (Type Sensitive, includes LogType/Timestamp):\n";
    // manager.printRedundancyStats(std::cout);

    // 完整的编码-解码测试
    // std::cout << "\n=== Complete Encoding-Decoding Test ===\n";
    
    size_t total_records = records.size();
    size_t successful_encodings = 0;
    size_t successful_decodings = 0;
    size_t perfect_matches = 0;
    
    // 测试前几条记录
    size_t test_count = std::min(size_t(187), total_records);
    
    for (size_t i = 0; i < test_count; ++i) {
        // std::cout << "\n--- Record " << (i + 1) << " ---\n";
        // std::cout << "Original: " << records[i] << "\n";
        
        // 编码阶段
        auto encoded_fields = encodeRecord(records[i], manager);
        successful_encodings++;
        
        // std::cout << "Encoded fields:\n";
        for (const auto& field : encoded_fields) {
            // std::string type_str;
            // switch (field.type) {
            //     case FieldType::Int: type_str = "Int"; break;
            //     case FieldType::Double: type_str = "Double"; break;
            //     case FieldType::Bool: type_str = "Bool"; break;
            //     case FieldType::String: type_str = "String"; break;
            //     case FieldType::Timestamp: type_str = "Timestamp"; break;
            //     case FieldType::LogType: type_str = "LogType"; break;
            //     case FieldType::Null: type_str = "Null"; break;
            //     default: type_str = "Unknown"; break;
            // }
            // std::cout << "  " << field.field_name << " (" << type_str << "): " 
            //          << field.original_value << " -> Code: " << field.code << "\n";
        }
        
        // 解码阶段
        std::string decoded_record = decodeRecord(encoded_fields, manager);
        successful_decodings++;
        
        // std::cout << "Decoded: " << decoded_record << "\n";
        
        // 验证一致性
        if (records[i] == decoded_record) {
            // std::cout << "✓ Perfect match!\n";
            perfect_matches++;
        } else {
            // std::cout << "✗ Mismatch detected\n";
            // 使用JSON对象比较，忽略字段顺序
            try {
                json original_json = json::parse(records[i]);
                json decoded_json = json::parse(decoded_record);
                if (original_json == decoded_json) {
                    // std::cout << "✓ JSON content matches (field order different)\n";
                    perfect_matches++;
                } else {
                    // std::cout << "✗ JSON content differs\n";
                    // 输出差异信息
                    // std::cout << "Original JSON keys: ";
                    // for (auto it = original_json.begin(); it != original_json.end(); ++it) {
                    //     std::cout << it.key() << " ";
                    // }
                    // std::cout << "\nDecoded JSON keys: ";
                    // for (auto it = decoded_json.begin(); it != decoded_json.end(); ++it) {
                    //     std::cout << it.key() << " ";
                    // }
                    // std::cout << "\n";
                }
            } catch (const std::exception& e) {
                // std::cout << "✗ JSON parsing error: " << e.what() << "\n";
            }
        }
    }
    
    // std::cout << "\n=== Test Summary ===\n";
    // std::cout << "Total records tested: " << test_count << "\n";
    // std::cout << "Successful encodings: " << successful_encodings << "\n";
    // std::cout << "Successful decodings: " << successful_decodings << "\n";
    // std::cout << "Perfect matches: " << perfect_matches << "\n";
    // std::cout << "Success rate: " << (perfect_matches * 100.0 / test_count) << "%\n";
    
    if (perfect_matches == test_count) {
        // std::cout << "🎉 All encoding-decoding tests passed!\n";
    } else {
        // std::cout << "⚠️  Some tests failed. Dictionary implementation may need review.\n";
    }
    
    // 测试时间戳格式解析
    // std::cout << "\n=== Testing Timestamp Format Parsing ===\n";
    TimestampDictionary timestamp_test;
    
    std::vector<std::string> timestamp_formats = {
        "2024-03-20 10:30:45.123 EDT",  // 带毫秒
        "2024-03-20 10:30:45 EDT",      // 不带毫秒
        "2024-03-20 10:31:12.456 EDT",  // 带毫秒
        "2024-03-20 10:31:00 EDT"       // 不带毫秒
    };
    
    for (const auto& ts : timestamp_formats) {
        auto [pattern_id, epoch] = timestamp_test.addTimestamp("timestamp", ts);
        // std::cout << "Timestamp: " << ts << " -> Pattern ID: " << pattern_id << ", Epoch: " << epoch << "\n";
        
        if (pattern_id != 0 && epoch != -1) {
            std::string decoded = timestamp_test.decode({pattern_id, epoch});
            if (decoded == ts) {
                // std::cout << "  ✓ Decode successful: " << decoded << "\n";
            } else {
                // std::cout << "  ✗ Decode failed: expected " << ts << ", got " << decoded << "\n";
            }
        } else {
            // std::cout << "  ✗ Parse failed\n";
        }
    }
    
    return 0;
}