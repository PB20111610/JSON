#include "../include/query/field_analyzer.h"
#include <algorithm>
#include <regex>

namespace json2 {
namespace query {

FieldAnalysis FieldAnalyzer::analyze(const QueryNode& root) {
    FieldAnalysis analysis;
    analysis.has_nested_queries = false;
    analysis.has_logical_operations = false;
    analysis.estimated_complexity = 0;
    
    analyzeNode(root, analysis);
    
    // 计算预估复杂度
    analysis.estimated_complexity = estimateComplexity(analysis);
    
    return analysis;
}

FieldAnalysis FieldAnalyzer::analyze(const std::string& query_string) {
    QueryParser parser;
    auto root = parser.parse(query_string);
    return analyze(*root);
}

bool FieldAnalyzer::isFieldUsed(const std::string& field_name, const FieldAnalysis& analysis) const {
    return analysis.required_fields.find(field_name) != analysis.required_fields.end();
}

FieldType FieldAnalyzer::getFieldType(const std::string& field_name, const FieldAnalysis& analysis) const {
    auto it = analysis.field_types.find(field_name);
    return it != analysis.field_types.end() ? it->second : FieldType::String;
}

size_t FieldAnalyzer::estimateComplexity(const FieldAnalysis& analysis) const {
    size_t complexity = 0;
    
    // 基础复杂度：字段数量
    complexity += analysis.required_fields.size();
    
    // 逻辑操作增加复杂度
    if (analysis.has_logical_operations) {
        complexity *= 2;
    }
    
    // 嵌套查询增加复杂度
    if (analysis.has_nested_queries) {
        complexity *= 3;
    }
    
    // 模板查询增加复杂度
    if (!analysis.template_fields.empty()) {
        complexity += analysis.template_fields.size() * 2;
    }
    
    // 范围查询增加复杂度
    if (!analysis.range_fields.empty()) {
        complexity += analysis.range_fields.size() * 2;
    }
    
    return complexity;
}

void FieldAnalyzer::analyzeNode(const QueryNode& node, FieldAnalysis& analysis) {
    switch (node.getType()) {
        case QueryNodeType::FIELD:
            analyzeFieldNode(node, analysis);
            break;
        case QueryNodeType::VALUE:
            analyzeValueNode(node, analysis);
            break;
        case QueryNodeType::OPERATOR:
            analyzeOperatorNode(node, analysis);
            break;
        case QueryNodeType::LOGICAL:
            analyzeLogicalNode(node, analysis);
            break;
    }
}

void FieldAnalyzer::analyzeFieldNode(const QueryNode& node, FieldAnalysis& analysis) {
    std::string field_name = node.getContent();
    FieldType field_type = node.getFieldType();
    
    // 添加到必需字段
    analysis.required_fields.insert(field_name);
    analysis.field_types[field_name] = field_type;
    
    // 检查是否为时间戳字段
    if (isTimestampField(field_name)) {
        analysis.timestamp_fields.insert(field_name);
    }
    
    // 检查是否为模板字段
    if (isTemplateField(field_name)) {
        analysis.template_fields.insert(field_name);
    }
    
    // 检查是否为嵌套字段
    if (isNestedField(field_name)) {
        analysis.has_nested_queries = true;
    }
    
    // 分析子节点
    for (const auto& child : node.getChildren()) {
        analyzeNode(*child, analysis);
    }
}

void FieldAnalyzer::analyzeValueNode(const QueryNode& node, FieldAnalysis& analysis) {
    // 值节点本身不直接提供字段信息，但可能影响类型推断
    // 这里可以添加基于值的类型推断逻辑
}

void FieldAnalyzer::analyzeOperatorNode(const QueryNode& node, FieldAnalysis& analysis) {
    QueryOperator op = node.getOperator();
    
    // 检查范围查询
    if (op == QueryOperator::RANGE) {
        // 从父节点获取字段名
        // 这里需要从上下文推断字段名
        // 简化实现：假设最近的字段节点是目标字段
    }
    
    // 检查模板匹配
    if (op == QueryOperator::MATCHES) {
        // 从父节点获取字段名并标记为模板字段
        // 简化实现
    }
    
    // 分析子节点
    for (const auto& child : node.getChildren()) {
        analyzeNode(*child, analysis);
    }
}

void FieldAnalyzer::analyzeLogicalNode(const QueryNode& node, FieldAnalysis& analysis) {
    analysis.has_logical_operations = true;
    
    // 分析子节点
    for (const auto& child : node.getChildren()) {
        analyzeNode(*child, analysis);
    }
}

FieldType FieldAnalyzer::inferFieldType(const std::string& field_name, const std::string& value) const {
    // 基于字段名推断类型
    if (isTimestampField(field_name)) {
        return FieldType::Timestamp;
    }
    
    if (isTemplateField(field_name)) {
        return FieldType::LogType;
    }
    
    // 基于值推断类型
    if (value == "true" || value == "false") {
        return FieldType::Bool;
    }
    
    if (value == "null") {
        return FieldType::Null;
    }
    
    // 检查是否为数字
    try {
        std::stoll(value);
        return FieldType::Int;
    } catch (...) {
        try {
            std::stod(value);
            return FieldType::Double;
        } catch (...) {
            return FieldType::String;
        }
    }
}

bool FieldAnalyzer::isTimestampField(const std::string& field_name) const {
    // 常见的时间戳字段名
    static const std::vector<std::string> timestamp_fields = {
        "timestamp", "time", "date", "created_at", "updated_at", 
        "@timestamp", "log_time", "event_time"
    };
    
    std::string lower_name = field_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    return std::find(timestamp_fields.begin(), timestamp_fields.end(), lower_name) != timestamp_fields.end();
}

bool FieldAnalyzer::isTemplateField(const std::string& field_name) const {
    // 常见的模板字段名
    static const std::vector<std::string> template_fields = {
        "message", "log", "msg", "text", "content", "description"
    };
    
    std::string lower_name = field_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    return std::find(template_fields.begin(), template_fields.end(), lower_name) != template_fields.end();
}

bool FieldAnalyzer::isNestedField(const std::string& field_name) const {
    // 检查是否包含点号（嵌套字段）
    return field_name.find('.') != std::string::npos;
}

bool FieldAnalyzer::hasRangeQuery(const QueryNode& node) const {
    if (node.getType() == QueryNodeType::OPERATOR && 
        node.getOperator() == QueryOperator::RANGE) {
        return true;
    }
    
    for (const auto& child : node.getChildren()) {
        if (hasRangeQuery(*child)) {
            return true;
        }
    }
    
    return false;
}

bool FieldAnalyzer::hasTemplateQuery(const QueryNode& node) const {
    if (node.getType() == QueryNodeType::OPERATOR && 
        node.getOperator() == QueryOperator::MATCHES) {
        return true;
    }
    
    for (const auto& child : node.getChildren()) {
        if (hasTemplateQuery(*child)) {
            return true;
        }
    }
    
    return false;
}

bool FieldAnalyzer::hasNestedQuery(const QueryNode& node) const {
    if (node.getType() == QueryNodeType::FIELD && 
        isNestedField(node.getContent())) {
        return true;
    }
    
    for (const auto& child : node.getChildren()) {
        if (hasNestedQuery(*child)) {
            return true;
        }
    }
    
    return false;
}

size_t FieldAnalyzer::calculateNodeComplexity(const QueryNode& node) const {
    size_t complexity = 1;
    
    switch (node.getType()) {
        case QueryNodeType::FIELD:
            complexity = 1;
            break;
        case QueryNodeType::VALUE:
            complexity = 1;
            break;
        case QueryNodeType::OPERATOR:
            switch (node.getOperator()) {
                case QueryOperator::EQUALS:
                case QueryOperator::NOT_EQUALS:
                    complexity = 1;
                    break;
                case QueryOperator::GREATER:
                case QueryOperator::GREATER_EQUAL:
                case QueryOperator::LESS:
                case QueryOperator::LESS_EQUAL:
                    complexity = 2;
                    break;
                case QueryOperator::RANGE:
                    complexity = 3;
                    break;
                case QueryOperator::MATCHES:
                    complexity = 4;
                    break;
                case QueryOperator::EXISTS:
                    complexity = 1;
                    break;
                default:
                    complexity = 1;
                    break;
            }
            break;
        case QueryNodeType::LOGICAL:
            switch (node.getOperator()) {
                case QueryOperator::AND:
                    complexity = 2;
                    break;
                case QueryOperator::OR:
                    complexity = 3;
                    break;
                case QueryOperator::NOT:
                    complexity = 2;
                    break;
                default:
                    complexity = 1;
                    break;
            }
            break;
    }
    
    // 递归计算子节点复杂度
    for (const auto& child : node.getChildren()) {
        complexity += calculateNodeComplexity(*child);
    }
    
    return complexity;
}

} // namespace query
} // namespace json2
