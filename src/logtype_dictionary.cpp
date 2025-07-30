#include "../include/logtype_dictionary.h"
#include "../include/variable_dictionary.h"
#include <variant>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <iostream> // Added for debug output

namespace json2 {

uint32_t LogTypeDictionary::addLogType(const std::string& log_template) {
    auto it = template_to_id_.find(log_template);
    if (it != template_to_id_.end()) {
        // std::cout << "[DEBUG][LogTypeDictionary::addLogType] already exists: id=" << it->second << ", template='" << log_template << "'" << std::endl;
        return it->second;
    }
    uint32_t id = static_cast<uint32_t>(id_to_template_.size() + 1);
    template_to_id_[log_template] = id;
    id_to_template_.push_back(log_template);
    // std::cout << "[DEBUG][LogTypeDictionary::addLogType] new: id=" << id << ", template='" << log_template << "'" << std::endl;
    return id;
}

std::string LogTypeDictionary::getLogTypeById(uint32_t id) const {
    // std::cout << "[DEBUG][LogTypeDictionary::getLogTypeById] template_id=" << id << std::endl;
    if (id == 0 || id > id_to_template_.size()) return "";
    return id_to_template_[id - 1];
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



EncodedLog LogTypeDictionary::encodeLog(const FieldKey& key, const std::string& log_template, const std::vector<std::string>& variables) {
    // 目前FieldKey未参与分发，保留接口兼容性
    return encodeLog(log_template, variables);
}

std::pair<std::string, std::vector<std::string>> LogTypeDictionary::decodeLog(const FieldKey& key, const EncodedLog& encoded) const {
    return decodeLog(encoded);
}

std::string LogTypeDictionary::decodeLogToString(const FieldKey& key, const EncodedLog& encoded) const {
    // std::cout << "[DEBUG] decodeLogToString: template_id=" << encoded.template_id << std::endl;
    auto [tmpl, vars] = decodeLog(encoded);
    // std::cout << "[DEBUG] decodeLogToString: tmpl=" << tmpl << std::endl;
    std::string result = tmpl;
    size_t var_idx = 0;
    size_t pos = 0;
    while ((pos = result.find("*", pos)) != std::string::npos && var_idx < vars.size()) {
        result.replace(pos, 1, vars[var_idx++]);
        pos += vars[var_idx-1].size();
    }
    // std::cout << "[DEBUG][LogTypeDictionary::decodeLogToString] result=" << result << std::endl;
    return result;
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

uint32_t LogTypeDictionary::getOrAddFieldValue(const FieldKey& key, const Value& value) {
    // 只支持 string 类型
    if (!std::holds_alternative<std::string>(value)) {
        throw std::invalid_argument("LogTypeDictionary only supports string values");
    }
    const std::string& str = std::get<std::string>(value);
    return addLogType(str);
}

} // namespace json2
