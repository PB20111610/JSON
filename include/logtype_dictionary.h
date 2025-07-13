#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ostream>
#include <iostream>
#include <cstdint>

namespace json2 {

struct EncodedLog {
    uint32_t template_id;
    std::vector<uint32_t> var_codes;
};

class LogTypeDictionary {
public:
    // 添加日志模板，返回模板id
    uint32_t addLogType(const std::string& log_template);
    // 通过id获取模板字符串
    std::string getLogTypeById(uint32_t id) const;
    // 获取模板出现次数
    size_t getLogTypeCount(const std::string& log_template) const;
    void clear();
    size_t getLogTypeCount() const;

    // 变量编码
    uint32_t encodeVariable(const std::string& var);
    std::string decodeVariable(uint32_t code) const;

    // 日志整体编码/解码
    EncodedLog encodeLog(const std::string& log_template, const std::vector<std::string>& variables);
    std::pair<std::string, std::vector<std::string>> decodeLog(const EncodedLog& encoded) const;
    // 自动解码为原始日志字符串
    std::string decodeLogToString(const EncodedLog& encoded) const;

private:
    std::unordered_map<std::string, uint32_t> template_to_id_;
    std::vector<std::string> id_to_template_;
    std::unordered_map<std::string, size_t> template_count_;
    uint32_t next_id_ = 1;

    // 变量字典
    std::unordered_map<std::string, uint32_t> variable_to_code_;
    std::vector<std::string> code_to_variable_;
    uint32_t next_var_code_ = 1;

    // 自动模板化和变量提取编码
    EncodedLog encodeLog(const std::string& raw_log);
    // 辅助函数，提取模板和变量
    std::pair<std::string, std::vector<std::string>> extractTemplateAndVars(const std::string& raw_log) const;
};

} // namespace json2
