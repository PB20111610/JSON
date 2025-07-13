#include "../include/logtype_dictionary.h"
#include <regex>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace json2 {

uint32_t LogTypeDictionary::addLogType(const std::string& log_template) {
    auto it = template_to_id_.find(log_template);
    if (it != template_to_id_.end()) {
        template_count_[log_template]++;
        return it->second;
    }
    uint32_t id = next_id_++;
    template_to_id_[log_template] = id;
    if (id_to_template_.size() <= id) {
        id_to_template_.resize(id + 1);
    }
    id_to_template_[id] = log_template;
    template_count_[log_template] = 1;
    return id;
}

std::string LogTypeDictionary::getLogTypeById(uint32_t id) const {
    if (id < id_to_template_.size()) {
        return id_to_template_[id];
    }
    return "";
}

size_t LogTypeDictionary::getLogTypeCount(const std::string& log_template) const {
    auto it = template_count_.find(log_template);
    if (it != template_count_.end()) {
        return it->second;
    }
    return 0;
}

void LogTypeDictionary::clear() {
    template_to_id_.clear();
    id_to_template_.clear();
    template_count_.clear();
    next_id_ = 1;
    variable_to_code_.clear();
    code_to_variable_.clear();
    next_var_code_ = 1;
}

size_t LogTypeDictionary::getLogTypeCount() const {
    return template_to_id_.size();
}

uint32_t LogTypeDictionary::encodeVariable(const std::string& var) {
    auto it = variable_to_code_.find(var);
    if (it != variable_to_code_.end()) {
        return it->second;
    }
    uint32_t code = next_var_code_++;
    variable_to_code_[var] = code;
    if (code_to_variable_.size() <= code) {
        code_to_variable_.resize(code + 1);
    }
    code_to_variable_[code] = var;
    return code;
}

std::string LogTypeDictionary::decodeVariable(uint32_t code) const {
    if (code < code_to_variable_.size()) {
        return code_to_variable_[code];
    }
    return "";
}



EncodedLog LogTypeDictionary::encodeLog(const std::string& log_template, const std::vector<std::string>& variables) {
    EncodedLog encoded;
    encoded.template_id = addLogType(log_template);
    for (const auto& var : variables) {
        encoded.var_codes.push_back(encodeVariable(var));
    }
    return encoded;
}

std::pair<std::string, std::vector<std::string>> LogTypeDictionary::decodeLog(const EncodedLog& encoded) const {
    std::string tmpl = getLogTypeById(encoded.template_id);
    std::vector<std::string> vars;
    for (auto code : encoded.var_codes) {
        vars.push_back(decodeVariable(code));
    }
    return {tmpl, vars};
}

std::pair<std::string, std::vector<std::string>> LogTypeDictionary::extractTemplateAndVars(const std::string& raw_log) const {
    std::vector<std::string> vars;
    std::string tmpl = raw_log;
    // 支持的变量正则：数字、IP、邮箱
    std::regex var_regex(R"((\b\d+\b)|(\b\d{1,3}(?:\.\d{1,3}){3}\b)|([a-zA-Z0-9_.+-]+@[a-zA-Z0-9-]+\.[a-zA-Z0-9-.]+))");
    std::smatch match;
    std::string::const_iterator searchStart(tmpl.cbegin());
    size_t offset = 0;
    while (std::regex_search(searchStart, tmpl.cend(), match, var_regex)) {
        vars.push_back(match.str());
        // 替换为 *
        size_t pos = match.position(0) + offset;
        tmpl.replace(pos, match.length(0), "*");
        offset = pos + 1; // * 长度为1
        searchStart = tmpl.cbegin() + offset;
    }
    return {tmpl, vars};
}



EncodedLog LogTypeDictionary::encodeLog(const std::string& raw_log) {
    auto [tmpl, vars] = extractTemplateAndVars(raw_log);
    return encodeLog(tmpl, vars);
}

std::string LogTypeDictionary::decodeLogToString(const EncodedLog& encoded) const {
    auto [tmpl, vars] = decodeLog(encoded);
    std::string result = tmpl;
    size_t var_idx = 0;
    size_t pos = 0;
    while ((pos = result.find("*", pos)) != std::string::npos && var_idx < vars.size()) {
        result.replace(pos, 1, vars[var_idx++]);
        pos += vars[var_idx-1].size();
    }
    return result;
}

} // namespace json2
