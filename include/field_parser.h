#pragma once

#include <string>
#include <vector>
#include <tuple>
#include <simdjson.h>
#include "field_dictionary_manager.h"

namespace json2 {

class FieldParser {
public:
    // 统一的字段类型推断（从simdjson元素）
    static FieldType inferFieldType(const std::string& field_name, 
                                   simdjson::dom::element value_node, 
                                   FieldDictionaryManager& manager,
                                   bool is_serialized_array = false);
    
    // 统一的字段值提取
    static Value extractValue(simdjson::dom::element value_node);
    
    // 统一的嵌套字段访问
    static simdjson::dom::element getNestedField(simdjson::dom::element node, const std::string& field);
    
    // 统一的字段解析（返回字段名、类型、值）
    static std::vector<std::tuple<std::string, FieldType, Value>> parseFields(
        const std::string& record, FieldDictionaryManager& manager);
    
    // 递归收集所有字段
    static void collectAllFields(simdjson::dom::element node, 
                                const std::string& prefix, 
                                int depth,
                                std::set<FieldKey>& all_fields,
                                std::unordered_map<FieldKey, size_t>& value_counts,
                                FieldDictionaryManager& manager);

    // 新增：结构化数组处理函数
    static void parseStructuredArray(simdjson::dom::element array_node,
                                   const std::string& prefix,
                                   int depth,
                                   std::set<FieldKey>& all_fields,
                                   std::unordered_map<FieldKey, size_t>& value_counts,
                                   FieldDictionaryManager& manager);

    // 新增：提取所有字段的值到映射中
    static void extractAllFieldValues(simdjson::dom::element node,
                                    const std::string& prefix,
                                    std::unordered_map<FieldKey, std::string>& field_values);
};

} // namespace json2 