#pragma once

#include <string>
#include <vector>
#include <memory>
#include "field_dictionary_manager.h"
#include <tuple>

namespace json2 {

class JsonParser {
public:
    // 解析记录，返回字段、类型和值
    static std::vector<std::tuple<std::string, FieldType, Value>> parseFields(const std::string& record);
    // 类型敏感冗余度分析与排序（使用 FieldDictionaryManager）
    static void analyzeAndSortFields(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<std::string>& ordered_fields);
};

} // namespace json2
