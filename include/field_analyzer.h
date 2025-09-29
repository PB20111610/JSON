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
    
    // 分层混合排序策略 (Hierarchical Hybrid Ordering Strategy) - 主要排序方法
    static void analyzeAndSortFields(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<FieldKey>& ordered_fields);
    
    // 使用用户提供的字段排序作为调试方式
    static void analyzeWithCustomOrder(const std::vector<FieldKey>& custom_order, std::vector<FieldKey>& ordered_fields);
    
private:
    // 内部实现：分层混合排序策略的详细实现
    static void analyzeAndSortFieldsHierarchical(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<FieldKey>& ordered_fields);
};

} // namespace json2
