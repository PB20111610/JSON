#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <limits>
#include <utility>
#include <ostream>
#include <iostream>
#include "field_key.h"
#include "variable_dictionary.h"
#include <variant>

namespace json2 {

// 模板化时间戳编码结构
struct TemplateEncodedTimestamp {
    uint32_t template_id;      // 时间戳模板ID
    std::vector<uint32_t> var_codes;  // 变量编码（年、月、日、时、分、秒等）
    
    bool operator==(const TemplateEncodedTimestamp& other) const {
        return template_id == other.template_id && var_codes == other.var_codes;
    }
    bool operator!=(const TemplateEncodedTimestamp& other) const {
        return !(*this == other);
    }
};

// 添加operator<<定义
inline std::ostream& operator<<(std::ostream& os, const TemplateEncodedTimestamp& ts) {
    os << "TemplateTimestamp{template=" << ts.template_id << ",vars=[";
    for (size_t i = 0; i < ts.var_codes.size(); ++i) {
        if (i > 0) os << ",";
        os << ts.var_codes[i];
    }
    return os << "]}";
}

class TimestampDictionary {
public:
    // 编码/解码接口
    TemplateEncodedTimestamp encodeTemplate(const FieldKey& key, const std::string& value);
    std::string decodeTemplate(const FieldKey& key, const TemplateEncodedTimestamp& encoded) const;
    
    // 获取模板ID对应的模板字符串
    std::string getTemplateById(uint32_t id) const;
    // 获取变量编码对应的变量字符串
    std::string getVariableByCode(uint32_t code) const;
    // 获取模板数量
    size_t getTemplateCount() const;
    // 获取变量数量
    size_t getVariableCount() const;
    
    // 预注册模板（用于反序列化）
    void registerTemplate(const std::string& template_str);
    // 预注册变量（用于反序列化）
    void registerVariable(const std::string& variable);
    
    void clear();
    // 兼容 Trie 类型分发接口
    uint32_t getOrAddFieldValue(const FieldKey& key, const Value& value);

private:
    // 模板化编码相关
    std::unordered_map<std::string, uint32_t> template_to_id_;
    std::vector<std::string> id_to_template_;
    uint32_t next_template_id_ = 1;
    std::unordered_map<std::string, uint32_t> variable_to_code_;
    std::vector<std::string> code_to_variable_;
    uint32_t next_var_code_ = 1;
    
    // 辅助方法
    uint32_t addTemplate(const std::string& template_str);
    uint32_t encodeVariable(const std::string& var);
    std::string decodeVariable(uint32_t code) const;
    std::pair<std::string, std::vector<std::string>> extractTemplateAndVars(const std::string& timestamp) const;
};

} // namespace json2 