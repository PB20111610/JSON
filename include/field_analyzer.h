#pragma once

#include <string>
#include <vector>
#include <memory>
#include "field_key.h"
#include <tuple>
#include "field_dictionary_manager.h"

namespace json2 {

class FieldAnalyzer {
public:
    // 解析记录，返回字段、类型和值
    static std::vector<std::tuple<std::string, FieldType, Value>> parseFields(const std::string& record);
    // 类型敏感冗余度分析与排序（使用 FieldDictionaryManager）
    static void analyzeAndSortFields(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<FieldKey>& ordered_fields);
    
    // 使用用户提供的字段排序，仅进行字段收集和类型验证
    static void analyzeWithCustomOrder(const std::vector<std::string>& records, FieldDictionaryManager& manager, 
                                       const std::vector<FieldKey>& custom_order, std::vector<FieldKey>& ordered_fields);
    
    // 验证用户提供的字段排序是否与实际数据匹配
    static bool validateCustomOrder(const std::vector<std::string>& records, const std::vector<FieldKey>& custom_order, 
                                    std::string& error_message);
};

} // namespace json2
