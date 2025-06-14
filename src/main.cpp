#include "../include/parser.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

void printDictionaryStats(const json2::Dictionary& dict) {
    std::cout << "\nDictionary Statistics:\n";
    std::cout << "=====================\n";
    
    // 打印每种类型的字典大小
    std::cout << "Timestamp Dictionary Size: " << dict.size(json2::DictType::TIMESTAMP_DICT) << "\n";
    std::cout << "Log Dictionary Size: " << dict.size(json2::DictType::LOG_DICT) << "\n";
    std::cout << "Variable Dictionary Size: " << dict.size(json2::DictType::VARIABLE_DICT) << "\n\n";
    
    // 计算总记录数（使用最大出现次数作为估计）
    size_t total_records = 0;
    for (const auto& stats : dict.getFieldStats()) {
        total_records = std::max(total_records, stats.occurrence_count);
    }
    
    // 打印字段值域统计和冗余度因子
    std::cout << "Field Statistics and Redundancy Factors:\n";
    std::cout << "=====================================\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::setw(15) << "Field" 
              << std::setw(15) << "Type" 
              << std::setw(15) << "Value Count" 
              << std::setw(15) << "Occurrences" 
              << std::setw(15) << "Redundancy" 
              << "\n";
    std::cout << std::string(75, '-') << "\n";
    
    // 创建字段统计的副本用于排序
    std::vector<json2::FieldStats> sorted_stats = dict.getFieldStats();
    
    // 按冗余度因子排序
    std::sort(sorted_stats.begin(), sorted_stats.end(),
        [total_records](const json2::FieldStats& a, const json2::FieldStats& b) {
            double redundancy_a = static_cast<double>(a.occurrence_count) / 
                                 ((a.value_count + 1e-6) * total_records);
            double redundancy_b = static_cast<double>(b.occurrence_count) / 
                                 ((b.value_count + 1e-6) * total_records);
            return redundancy_a > redundancy_b;
        });
    
    // 打印排序后的字段统计
    for (const auto& stats : sorted_stats) {
        double redundancy_factor = static_cast<double>(stats.occurrence_count) / 
                                  ((stats.value_count + 1e-6) * total_records);
        
        std::cout << std::setw(15) << stats.name 
                  << std::setw(15) 
                  << (stats.type == json2::DictType::TIMESTAMP_DICT ? "TIMESTAMP" :
                      stats.type == json2::DictType::LOG_DICT ? "LOG" : "VARIABLE")
                  << std::setw(15) << stats.value_count
                  << std::setw(15) << stats.occurrence_count
                  << std::setw(15) << redundancy_factor
                  << "\n";
    }
    
    // 打印字段顺序（按冗余度因子）
    std::cout << "\nField Order (by redundancy factor):\n";
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
        const std::string test_log = "User login successful";
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