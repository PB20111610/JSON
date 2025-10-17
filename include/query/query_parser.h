#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include "field_key.h"

namespace json2 {
namespace query {

/**
 * 查询AST节点类型
 */
enum class QueryNodeType {
    FIELD,      // 字段节点
    VALUE,      // 值节点
    OPERATOR,   // 操作符节点
    LOGICAL,    // 逻辑操作符节点
    AGGREGATE,  // 聚合函数节点
    GROUP_BY    // GROUP BY节点
};

/**
 * 查询操作符
 */
enum class QueryOperator {
    EQUALS,         // =
    NOT_EQUALS,     // !=
    GREATER,        // >
    GREATER_EQUAL,  // >=
    LESS,           // <
    LESS_EQUAL,     // <=
    RANGE,          // [min TO max]
    EXISTS,         // 字段存在
    CONTAINS,       // 包含
    MATCHES,        // 模式匹配
    AND,            // 逻辑AND
    OR,             // 逻辑OR
    NOT,            // 逻辑NOT
    COUNT,          // 聚合函数COUNT
    SUM,            // 聚合函数SUM
    AVG,            // 聚合函数AVG
    MAX,            // 聚合函数MAX
    MIN             // 聚合函数MIN
};

// 聚合函数类型
enum class AggregateFunction {
    COUNT,
    SUM,
    AVG,
    MAX,
    MIN
};

/**
 * 查询AST节点
 */
class QueryNode {
public:
    QueryNode(QueryNodeType type, const std::string& content = "");
    
    // 静态工厂方法
    static std::unique_ptr<QueryNode> field(const std::string& field_name, 
                                           FieldType field_type = FieldType::String);
    static std::unique_ptr<QueryNode> value(const std::string& value);
    static std::unique_ptr<QueryNode> operator_(QueryOperator op);
    static std::unique_ptr<QueryNode> logical(QueryOperator op);
    static std::unique_ptr<QueryNode> range(const std::string& min_val, const std::string& max_val);
    static std::unique_ptr<QueryNode> aggregate(AggregateFunction func, const std::string& field_name = "");
    static std::unique_ptr<QueryNode> groupBy(const std::vector<std::string>& group_fields);
    
    // 访问器
    QueryNodeType getType() const { return type_; }
    const std::string& getContent() const { return content_; }
    QueryOperator getOperator() const { return operator_type_; }
    FieldType getFieldType() const { return field_type_; }
    const std::vector<std::unique_ptr<QueryNode>>& getChildren() const { return children_; }
    AggregateFunction getAggregateFunction() const { return aggregate_function_; }
    const std::vector<std::string>& getGroupFields() const { return group_fields_; }
    
    // 修改器
    void addChild(std::unique_ptr<QueryNode> child);
    void setFieldType(FieldType type) { field_type_ = type; }
    void setOperator(QueryOperator op) { operator_type_ = op; }
    void setContent(const std::string& content) { content_ = content; }
    void setAggregateFunction(AggregateFunction func) { aggregate_function_ = func; }
    void setGroupFields(const std::vector<std::string>& group_fields) { group_fields_ = group_fields; }
    
    // 查询构建辅助方法
    void addFieldValue(const std::string& field_name, const std::string& value, 
                      FieldType field_type = FieldType::String);
    void addLogicalOperator(QueryOperator op);
    
    // 调试输出
    std::string toString(int indent = 0) const;

private:
    QueryNodeType type_;
    std::string content_;
    QueryOperator operator_type_;
    FieldType field_type_;
    std::vector<std::unique_ptr<QueryNode>> children_;
    AggregateFunction aggregate_function_;
    std::vector<std::string> group_fields_;
};

/**
 * 查询解析器
 */
class QueryParser {
public:
    QueryParser() = default;
    
    /**
     * 解析查询字符串为AST
     * @param query_string 查询字符串
     * @return 查询AST根节点
     */
    std::unique_ptr<QueryNode> parse(const std::string& query_string);
    
    /**
     * 验证查询语法
     * @param query_string 查询字符串
     * @return 是否有效
     */
    bool validate(const std::string& query_string);

private:
    // 词法分析
    std::vector<std::string> tokenize(const std::string& query_string);
    
    // 语法分析
    std::unique_ptr<QueryNode> parseExpression(const std::vector<std::string>& tokens, 
                                              size_t& pos);
    std::unique_ptr<QueryNode> parseLogicalExpression(const std::vector<std::string>& tokens, 
                                                     size_t& pos);
    std::unique_ptr<QueryNode> parseComparisonExpression(const std::vector<std::string>& tokens, 
                                                        size_t& pos);
    std::unique_ptr<QueryNode> parseFieldExpression(const std::vector<std::string>& tokens, 
                                                   size_t& pos);
    
    // 辅助方法
    bool isOperator(const std::string& token);
    bool isLogicalOperator(const std::string& token);
    bool isKnownFieldType(const std::string& token);
    QueryOperator parseOperator(const std::string& token);
    FieldType parseFieldType(const std::string& type_str);
    std::string unquote(const std::string& str);
    
    // 错误处理
    void throwParseError(const std::string& message, size_t position);
};

} // namespace query
} // namespace json2
