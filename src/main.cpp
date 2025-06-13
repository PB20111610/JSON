#include "../include/parser.h"
#include <iostream>
#include <iomanip>

void printDictionaryStats(const json2::Dictionary& dict) {
    std::cout << "\nDictionary Statistics:\n";
    std::cout << "=====================\n";
    
    // 打印每种类型的字典大小
    std::cout << "Timestamp Dictionary Size: " << dict.size(json2::DictType::TIMESTAMP_DICT) << "\n";
    std::cout << "Log Dictionary Size: " << dict.size(json2::DictType::LOG_DICT) << "\n";
    std::cout << "Variable Dictionary Size: " << dict.size(json2::DictType::VARIABLE_DICT) << "\n\n";
    
    // 打印字段值域统计
    std::cout << "Field Value Domain Statistics:\n";
    std::cout << "============================\n";
    for (const auto& stats : dict.getFieldStats()) {
        std::cout << "Field: " << std::setw(15) << stats.name 
                  << " | Type: " << std::setw(15) 
                  << (stats.type == json2::DictType::TIMESTAMP_DICT ? "TIMESTAMP" :
                      stats.type == json2::DictType::LOG_DICT ? "LOG" : "VARIABLE")
                  << " | Value Count: " << stats.value_count << "\n";
    }
    
    // 打印字段顺序（按值域大小）
    std::cout << "\nField Order (by value domain size):\n";
    std::cout << "================================\n";
    auto ordered_fields = dict.getOrderedFields();
    for (size_t i = 0; i < ordered_fields.size(); ++i) {
        std::cout << (i + 1) << ". " << ordered_fields[i] << "\n";
    }
}

int main() {
    try {
        json2::Dictionary dict;
        std::vector<json2::ParsedField> fieldOrder;
        
        // 第一遍扫描：收集字典和字段统计
        std::cout << "First Pass: Collecting dictionary and field statistics...\n";
        json2::JsonParser::parseAndCollect("test_data.json", dict, fieldOrder);
        
        // 打印统计信息
        printDictionaryStats(dict);
        
        // 测试字典编码
        std::cout << "\nTesting Dictionary Encoding:\n";
        std::cout << "==========================\n";
        
        // 测试一些已知的值
        const std::string test_timestamp = "2024-03-20 10:30:45.123 EDT";
        const std::string test_log = "User login failed";
        const std::string test_var = "user123";
        
        uint32_t timestamp_code = dict.getCode(test_timestamp, json2::DictType::TIMESTAMP_DICT);
        uint32_t log_code = dict.getCode(test_log, json2::DictType::LOG_DICT);
        uint32_t var_code = dict.getCode(test_var, json2::DictType::VARIABLE_DICT);
        
        std::cout << "Test Timestamp: " << test_timestamp << " -> Code: " << timestamp_code << "\n";
        std::cout << "Test Log: " << test_log << " -> Code: " << log_code << "\n";
        std::cout << "Test Variable: " << test_var << " -> Code: " << var_code << "\n";
        
        // 验证编码-解码
        std::cout << "\nVerifying Encoding-Decoding:\n";
        std::cout << "==========================\n";
        std::cout << "Code " << timestamp_code << " -> " << dict.getString(timestamp_code, json2::DictType::TIMESTAMP_DICT) << "\n";
        std::cout << "Code " << log_code << " -> " << dict.getString(log_code, json2::DictType::LOG_DICT) << "\n";
        std::cout << "Code " << var_code << " -> " << dict.getString(var_code, json2::DictType::VARIABLE_DICT) << "\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}