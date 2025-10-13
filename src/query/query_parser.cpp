#include "../include/query/query_parser.h"
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace json2 {
namespace query {

// QueryNode 实现
QueryNode::QueryNode(QueryNodeType type, const std::string& content)
    : type_(type), content_(content), operator_type_(QueryOperator::EQUALS), field_type_(FieldType::String) {
}

std::unique_ptr<QueryNode> QueryNode::field(const std::string& field_name, FieldType field_type) {
    auto node = std::make_unique<QueryNode>(QueryNodeType::FIELD, field_name);
    node->field_type_ = field_type;
    return node;
}

std::unique_ptr<QueryNode> QueryNode::value(const std::string& value) {
    return std::make_unique<QueryNode>(QueryNodeType::VALUE, value);
}

std::unique_ptr<QueryNode> QueryNode::operator_(QueryOperator op) {
    auto node = std::make_unique<QueryNode>(QueryNodeType::OPERATOR);
    node->operator_type_ = op;
    return node;
}

std::unique_ptr<QueryNode> QueryNode::logical(QueryOperator op) {
    auto node = std::make_unique<QueryNode>(QueryNodeType::LOGICAL);
    node->operator_type_ = op;
    return node;
}

std::unique_ptr<QueryNode> QueryNode::range(const std::string& min_val, const std::string& max_val) {
    auto node = std::make_unique<QueryNode>(QueryNodeType::OPERATOR);
    node->operator_type_ = QueryOperator::RANGE;
    node->content_ = min_val + " TO " + max_val;
    return node;
}

void QueryNode::addChild(std::unique_ptr<QueryNode> child) {
    children_.push_back(std::move(child));
}

void QueryNode::addFieldValue(const std::string& field_name, const std::string& value, FieldType field_type) {
    auto field_node = field(field_name, field_type);
    auto value_node = QueryNode::value(value);
    auto op_node = std::make_unique<QueryNode>(QueryNodeType::OPERATOR);
    op_node->setOperator(QueryOperator::EQUALS);
    
    field_node->addChild(std::move(op_node));
    field_node->addChild(std::move(value_node));
    addChild(std::move(field_node));
}

void QueryNode::addLogicalOperator(QueryOperator op) {
    auto op_node = logical(op);
    addChild(std::move(op_node));
}

std::string QueryNode::toString(int indent) const {
    std::string indent_str(indent * 2, ' ');
    std::stringstream ss;
    
    switch (type_) {
        case QueryNodeType::FIELD:
            ss << indent_str << "FIELD: " << content_ << " (type: " << static_cast<int>(field_type_) << ")";
            break;
        case QueryNodeType::VALUE:
            ss << indent_str << "VALUE: " << content_;
            break;
        case QueryNodeType::OPERATOR:
            ss << indent_str << "OPERATOR: " << static_cast<int>(operator_type_);
            break;
        case QueryNodeType::LOGICAL:
            ss << indent_str << "LOGICAL: " << static_cast<int>(operator_type_);
            break;
    }
    
    for (const auto& child : children_) {
        ss << "\n" << child->toString(indent + 1);
    }
    
    return ss.str();
}

// QueryParser 实现
std::unique_ptr<QueryNode> QueryParser::parse(const std::string& query_string) {
    if (query_string.empty()) {
        throw std::invalid_argument("Query string cannot be empty");
    }
    
    auto tokens = tokenize(query_string);
    if (tokens.empty()) {
        throw std::invalid_argument("No valid tokens found in query string");
    }
    
    size_t pos = 0;
    return parseExpression(tokens, pos);
}

bool QueryParser::validate(const std::string& query_string) {
    try {
        parse(query_string);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<std::string> QueryParser::tokenize(const std::string& query_string) {
    std::vector<std::string> tokens;
    std::string current_token;
    bool in_quotes = false;
    char quote_char = '\0';
    
    for (size_t i = 0; i < query_string.length(); ++i) {
        char c = query_string[i];
        
        if (in_quotes) {
            if (c == quote_char) {
                in_quotes = false;
                quote_char = '\0';
                if (!current_token.empty()) {
                    tokens.push_back(current_token);
                    current_token.clear();
                }
            } else {
                current_token += c;
            }
        } else {
            if (c == '"' || c == '\'') {
                in_quotes = true;
                quote_char = c;
                if (!current_token.empty()) {
                    tokens.push_back(current_token);
                    current_token.clear();
                }
            } else if (std::isspace(c)) {
                if (!current_token.empty()) {
                    tokens.push_back(current_token);
                    current_token.clear();
                }
            } else if (c == '(' || c == ')' || c == '[' || c == ']' || c == ':' || c == ',') {
                if (!current_token.empty()) {
                    tokens.push_back(current_token);
                    current_token.clear();
                }
                tokens.push_back(std::string(1, c));
            } else {
                current_token += c;
            }
        }
    }
    
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }
    
    return tokens;
}

std::unique_ptr<QueryNode> QueryParser::parseExpression(const std::vector<std::string>& tokens, size_t& pos) {
    return parseLogicalExpression(tokens, pos);
}

std::unique_ptr<QueryNode> QueryParser::parseLogicalExpression(const std::vector<std::string>& tokens, size_t& pos) {
    auto left = parseComparisonExpression(tokens, pos);
    
    while (pos < tokens.size()) {
        if (tokens[pos] == "AND" || tokens[pos] == "OR") {
            auto op = tokens[pos] == "AND" ? QueryOperator::AND : QueryOperator::OR;
            pos++; // 跳过操作符
            
            auto right = parseComparisonExpression(tokens, pos);
            
            auto logical_node = std::make_unique<QueryNode>(QueryNodeType::LOGICAL);
            logical_node->setOperator(op);
            logical_node->addChild(std::move(left));
            logical_node->addChild(std::move(right));
            
            left = std::move(logical_node);
        } else {
            break;
        }
    }
    
    return left;
}

// 辅助函数：判断是否为已知字段类型
bool QueryParser::isKnownFieldType(const std::string& token) {
    static const std::unordered_set<std::string> types = {
        "string", "int", "double", "bool", "timestamp", "logtype", "null", "array"
    };
    return types.count(token) > 0;
}

std::unique_ptr<QueryNode> QueryParser::parseComparisonExpression(const std::vector<std::string>& tokens, size_t& pos) {
    if (pos >= tokens.size()) {
        throw std::invalid_argument("Unexpected end of tokens");
    }
    
    // 处理括号
    if (tokens[pos] == "(") {
        pos++; // 跳过 '('
        auto result = parseLogicalExpression(tokens, pos);
        if (pos >= tokens.size() || tokens[pos] != ")") {
            throw std::invalid_argument("Missing closing parenthesis");
        }
        pos++; // 跳过 ')'
        return result;
    }
    
    // 处理 NOT
    if (tokens[pos] == "NOT") {
        pos++; // 跳过 'NOT'
        auto operand = parseComparisonExpression(tokens, pos);
        
        auto not_node = std::make_unique<QueryNode>(QueryNodeType::LOGICAL);
        not_node->setOperator(QueryOperator::NOT);
        not_node->addChild(std::move(operand));
        
        return not_node;
    }
    
    return parseFieldExpression(tokens, pos);
}

// 主体函数：支持 field: value, field: "value", field:type: value, field > value 等
std::unique_ptr<QueryNode> QueryParser::parseFieldExpression(const std::vector<std::string>& tokens, size_t& pos) {
    if (pos >= tokens.size()) {
        throw std::invalid_argument("Expected field name");
    }
    std::string field_name = tokens[pos++];
    FieldType field_type = FieldType::String;

    // 支持类型声明 field:type: value
    if (pos < tokens.size() && tokens[pos] == ":") {
        pos++;
        if (pos < tokens.size() && isKnownFieldType(tokens[pos])) {
            field_type = parseFieldType(tokens[pos++]);
            if (pos < tokens.size() && tokens[pos] == ":") pos++;
        }
    }

    if (pos >= tokens.size()) {
        throw std::invalid_argument("Expected operator or value");
    }

    // 检查操作符
    QueryOperator op = QueryOperator::EQUALS;
    if (pos < tokens.size() && isOperator(tokens[pos])) {
        op = parseOperator(tokens[pos++]);
    } else if (pos < tokens.size() && tokens[pos] == ":") {
        pos++;
    }

    if (pos >= tokens.size()) {
        throw std::invalid_argument("Expected value");
    }
    std::string value = tokens[pos++];

    // 处理范围查询 [min TO max]
    if (value == "[" && pos < tokens.size()) {
        std::string min_val = tokens[pos++];
        if (pos >= tokens.size() || tokens[pos] != "TO") {
            throw std::invalid_argument("Expected 'TO' in range query");
        }
        pos++;
        if (pos >= tokens.size()) {
            throw std::invalid_argument("Expected max value in range query");
        }
        std::string max_val = tokens[pos++];
        if (pos >= tokens.size() || tokens[pos] != "]") {
            throw std::invalid_argument("Expected ']' in range query");
        }
        pos++;
        auto field_node = QueryNode::field(field_name, field_type);
        auto range_node = QueryNode::range(min_val, max_val);
        field_node->addChild(std::move(range_node));
        return field_node;
    }

    // 字段存在性查询
    if (value == "*") {
        auto field_node = QueryNode::field(field_name, field_type);
        auto exists_node = std::make_unique<QueryNode>(QueryNodeType::OPERATOR);
        exists_node->setOperator(QueryOperator::EXISTS);
        field_node->addChild(std::move(exists_node));
        return field_node;
    }

    // 模板/通配符查询
    if (value.find("*") != std::string::npos) {
        auto field_node = QueryNode::field(field_name, field_type);
        auto template_node = std::make_unique<QueryNode>(QueryNodeType::OPERATOR);
        template_node->setOperator(QueryOperator::MATCHES);
        template_node->setContent(value);
        field_node->addChild(std::move(template_node));
        return field_node;
    }

    // 普通字段值查询
    auto field_node = QueryNode::field(field_name, field_type);
    auto value_node = QueryNode::value(unquote(value));
    auto op_node = std::make_unique<QueryNode>(QueryNodeType::OPERATOR);
    op_node->setOperator(op);
    field_node->addChild(std::move(op_node));
    field_node->addChild(std::move(value_node));
    return field_node;
}

bool QueryParser::isOperator(const std::string& token) {
    return token == "=" || token == "!=" || token == ">" || token == ">=" || 
           token == "<" || token == "<=" || token == "~" || token == "!~";
}

bool QueryParser::isLogicalOperator(const std::string& token) {
    return token == "AND" || token == "OR" || token == "NOT";
}

QueryOperator QueryParser::parseOperator(const std::string& token) {
    if (token == "=") return QueryOperator::EQUALS;
    if (token == "!=") return QueryOperator::NOT_EQUALS;
    if (token == ">") return QueryOperator::GREATER;
    if (token == ">=") return QueryOperator::GREATER_EQUAL;
    if (token == "<") return QueryOperator::LESS;
    if (token == "<=") return QueryOperator::LESS_EQUAL;
    if (token == "~") return QueryOperator::CONTAINS;
    if (token == "!~") return QueryOperator::NOT;
    
    throw std::invalid_argument("Unknown operator: " + token);
}

FieldType QueryParser::parseFieldType(const std::string& type_str) {
    if (type_str == "string") return FieldType::String;
    if (type_str == "int") return FieldType::Int;
    if (type_str == "double") return FieldType::Double;
    if (type_str == "bool") return FieldType::Bool;
    if (type_str == "timestamp") return FieldType::Timestamp;
    if (type_str == "logtype") return FieldType::LogType;
    if (type_str == "null") return FieldType::Null;
    if (type_str == "array") return FieldType::UnstructuredArray;
    
    // 默认返回String类型
    return FieldType::String;
}

std::string QueryParser::unquote(const std::string& str) {
    if (str.length() >= 2 && ((str[0] == '"' && str.back() == '"') || 
                              (str[0] == '\'' && str.back() == '\''))) {
        return str.substr(1, str.length() - 2);
    }
    return str;
}

void QueryParser::throwParseError(const std::string& message, size_t position) {
    throw std::invalid_argument("Parse error at position " + std::to_string(position) + ": " + message);
}

} // namespace query
} // namespace json2
