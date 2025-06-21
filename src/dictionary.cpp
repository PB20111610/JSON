#include "../include/dictionary.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace json2 {

uint32_t Dictionary::addFieldValue(const std::string& field_name, const std::string& value) {
    auto& dict = field_dicts[field_name];
    auto it = dict.value_to_code.find(value);
    if (it != dict.value_to_code.end()) {
        return it->second;
    }
    uint32_t code = dict.next_code++;
    dict.value_to_code[value] = code;
    dict.code_to_value.push_back(value);
    return code;
}

uint32_t Dictionary::getOrAddFieldValue(const std::string& field_name, const std::string& value) {
    uint32_t code = getFieldValueCode(field_name, value);
    if (code == 0) {
        code = addFieldValue(field_name, value);
    }
    return code;
}

uint32_t Dictionary::getFieldValueCode(const std::string& field_name, const std::string& value) const {
    auto it = field_dicts.find(field_name);
    if (it == field_dicts.end()) return 0;
    auto code_it = it->second.value_to_code.find(value);
    return (code_it != it->second.value_to_code.end()) ? code_it->second : 0;
}

std::string Dictionary::getFieldValueByCode(const std::string& field_name, uint32_t code) const {
    auto it = field_dicts.find(field_name);
    if (it == field_dicts.end()) return "";
    if (code == 0 || code > it->second.code_to_value.size()) return "";
    return it->second.code_to_value[code - 1];
}

void Dictionary::clear() {
    field_dicts.clear();
}

size_t Dictionary::getFieldValueCount(const std::string& field) const {
    auto it = field_dicts.find(field);
    return (it != field_dicts.end()) ? it->second.code_to_value.size() : 0;
}

} // namespace json2