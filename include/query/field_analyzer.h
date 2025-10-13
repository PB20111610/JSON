#pragma once

#include "query_parser.h"
#include "field_key.h"
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace json2 {
namespace query {

/**
 * 字段分析结果
 */
struct FieldAnalysis {
    std::unordered_set<std::string> required_fields;           // 查询需要的字段
    std::unordered_map<std::string, FieldType> field_types;    // 字段类型映射
    std::unordered_set<std::string> timestamp_fields;          // 时间戳字段
    std::unordered_set<std::string> template_fields;           // 模板字段
    std::unordered_set<std::string> range_fields;              // 范围查询字段
    bool has_nested_queries;                                   // 是否有嵌套查询
    bool has_logical_operations;                               // 是否有逻辑操作
    size_t estimated_complexity;                               // 预估复杂度
};

/**
 * 字段分析器
 * 分析查询AST，提取字段信息和查询特征
 */
class FieldAnalyzer {
public:
    FieldAnalyzer() = default;
    
    /**
     * 分析查询AST，提取字段信息
     * @param root 查询AST根节点
     * @return 字段分析结果
     */
    FieldAnalysis analyze(const QueryNode& root);
    
    /**
     * 分析查询字符串
     * @param query_string 查询字符串
     * @return 字段分析结果
     */
    FieldAnalysis analyze(const std::string& query_string);
    
    /**
     * 检查字段是否在查询中被使用
     * @param field_name 字段名
     * @param analysis 字段分析结果
     * @return 是否被使用
     */
    bool isFieldUsed(const std::string& field_name, const FieldAnalysis& analysis) const;
    
    /**
     * 获取字段的查询类型
     * @param field_name 字段名
     * @param analysis 字段分析结果
     * @return 字段类型
     */
    FieldType getFieldType(const std::string& field_name, const FieldAnalysis& analysis) const;
    
    /**
     * 估算查询复杂度
     * @param analysis 字段分析结果
     * @return 复杂度分数
     */
    size_t estimateComplexity(const FieldAnalysis& analysis) const;

private:
    // 递归分析AST节点
    void analyzeNode(const QueryNode& node, FieldAnalysis& analysis);
    
    // 分析不同类型的节点
    void analyzeFieldNode(const QueryNode& node, FieldAnalysis& analysis);
    void analyzeValueNode(const QueryNode& node, FieldAnalysis& analysis);
    void analyzeOperatorNode(const QueryNode& node, FieldAnalysis& analysis);
    void analyzeLogicalNode(const QueryNode& node, FieldAnalysis& analysis);
    
    // 字段类型推断
    FieldType inferFieldType(const std::string& field_name, const std::string& value) const;
    bool isTimestampField(const std::string& field_name) const;
    bool isTemplateField(const std::string& field_name) const;
    bool isNestedField(const std::string& field_name) const;
    
    // 查询特征检测
    bool hasRangeQuery(const QueryNode& node) const;
    bool hasTemplateQuery(const QueryNode& node) const;
    bool hasNestedQuery(const QueryNode& node) const;
    
    // 复杂度计算
    size_t calculateNodeComplexity(const QueryNode& node) const;
};

} // namespace query
} // namespace json2
