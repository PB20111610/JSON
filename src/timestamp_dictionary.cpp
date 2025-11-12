#include "../include/timestamp_dictionary.h"
#include "../include/variable_dictionary.h"
#include <regex>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <variant>
#include <iostream>
#include <map>

namespace json2 {

// 模板化编码相关方法实现
uint32_t TimestampDictionary::addTemplate(const std::string& template_str) {
    auto it = template_to_id_.find(template_str);
    if (it != template_to_id_.end()) {
        return it->second;
    }
    uint32_t id = static_cast<uint32_t>(id_to_template_.size() + 1);
    template_to_id_[template_str] = id;
    id_to_template_.push_back(template_str);
    return id;
}

uint32_t TimestampDictionary::encodeVariable(const std::string& var) {
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

std::string TimestampDictionary::decodeVariable(uint32_t code) const {
    if (code < code_to_variable_.size()) {
        return code_to_variable_[code];
        }
    return "";
}

std::pair<std::string, std::vector<std::string>> TimestampDictionary::extractTemplateAndVars(const std::string& timestamp) const {
    std::vector<std::string> vars;
    std::string tmpl = timestamp;
    
    // Simple and reliable approach for ISO 8601 timestamp format like "2023-01-05T14:00:00Z"
    // Extract numeric components: year, month, day, hour, minute, second
    std::regex var_regex(R"(\d{4}|\d{2})");
    std::smatch match;
    std::string::const_iterator searchStart(tmpl.cbegin());
    size_t offset = 0;
    
    while (std::regex_search(searchStart, tmpl.cend(), match, var_regex)) {
        std::string matched = match.str();
        // Only add actual numeric components, not separators
        if (matched.length() >= 2 && std::all_of(matched.begin(), matched.end(), ::isdigit)) {
            vars.push_back(matched);
            // Replace with *
            size_t pos = match.position(0) + offset;
            tmpl.replace(pos, match.length(0), "*");
            offset = pos + 1; // * length is 1
            searchStart = tmpl.cbegin() + offset;
        } else {
            searchStart = match.suffix().first;
        }
    }
    
    return {tmpl, vars};
}

TemplateEncodedTimestamp TimestampDictionary::encodeTemplate(const FieldKey& key, const std::string& value) {
    auto [tmpl, vars] = extractTemplateAndVars(value);
    TemplateEncodedTimestamp encoded;
    encoded.template_id = addTemplate(tmpl);
    for (const auto& var : vars) {
        encoded.var_codes.push_back(encodeVariable(var));
    }
    return encoded;
}

std::string TimestampDictionary::decodeTemplate(const FieldKey& key, const TemplateEncodedTimestamp& encoded) const {
    if (encoded.template_id > 0 && encoded.template_id <= id_to_template_.size()) {
        std::string tmpl = id_to_template_[encoded.template_id - 1];
        std::string result = tmpl;
        size_t var_idx = 0;
        size_t pos = 0;
        while ((pos = result.find("*", pos)) != std::string::npos && var_idx < encoded.var_codes.size()) {
            std::string var = decodeVariable(encoded.var_codes[var_idx]);
            result.replace(pos, 1, var);
            pos += var.size();
            var_idx++;
        }
        return result;
    }
    return "";
}

std::string TimestampDictionary::getTemplateById(uint32_t id) const {
    if (id > 0 && id <= id_to_template_.size()) return id_to_template_[id - 1];
    return "";
}

std::string TimestampDictionary::getVariableByCode(uint32_t code) const {
    return decodeVariable(code);
}

size_t TimestampDictionary::getTemplateCount() const {
    return id_to_template_.size();
}

size_t TimestampDictionary::getVariableCount() const {
    return code_to_variable_.size();
}

uint32_t TimestampDictionary::getOrAddFieldValue(const FieldKey& key, const Value& value) {
    if (!std::holds_alternative<std::string>(value)) {
        throw std::invalid_argument("TimestampDictionary only supports string values");
    }
    const std::string& str = std::get<std::string>(value);
    auto encoded = encodeTemplate(key, str);
    return encoded.template_id;
}

void TimestampDictionary::registerTemplate(const std::string& template_str) {
    if (!template_str.empty()) {
        addTemplate(template_str);
    }
}

void TimestampDictionary::registerVariable(const std::string& variable) {
    if (!variable.empty()) {
        encodeVariable(variable);
    }
}

void TimestampDictionary::clear() {
    template_to_id_.clear();
    id_to_template_.clear();
    next_template_id_ = 1;
    variable_to_code_.clear();
    code_to_variable_.clear();
    next_var_code_ = 1;
}

} // namespace json2 