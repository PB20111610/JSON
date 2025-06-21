#include "../include/parser.h"
#include <set>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <simdjson.h>
#include <sstream>

namespace json2 {

// Forward declaration for the new recursive helper
void collectFieldsSimd(simdjson::dom::element node, const std::string& prefix, std::set<std::string>& fields, std::unordered_map<std::string, size_t>& value_counts, Dictionary& dict);

void JsonParser::analyzeAndSortFields(const std::vector<std::string>& records, Dictionary& dict, std::vector<std::string>& ordered_fields) {
    std::set<std::string> all_fields;
    std::unordered_map<std::string, size_t> value_counts;
    simdjson::dom::parser parser;

    for (const auto& rec_str : records) {
        try {
            simdjson::dom::element doc = parser.parse(rec_str).value();
            collectFieldsSimd(doc, "", all_fields, value_counts, dict);
        } catch (const simdjson::simdjson_error& e) {
            std::cerr << "simdjson parse error: " << e.what() << std::endl;
        }
    }

    // The rest of the logic (redundancy calculation) remains the same
    std::vector<std::pair<std::string, double>> redundancy;
    for (const auto& field : all_fields) {
        size_t occ = value_counts[field];
        size_t val_count = dict.getFieldValueCount(field);
        double factor = (val_count > 0) ? (double)occ / val_count : 0.0;
        redundancy.emplace_back(field, factor);
    }
    std::sort(redundancy.begin(), redundancy.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    ordered_fields.clear();
    for (const auto& [field, _] : redundancy) {
        ordered_fields.push_back(field);
    }
}

// New implementation of collectFields using simdjson
void collectFieldsSimd(simdjson::dom::element node, const std::string& prefix, std::set<std::string>& fields, std::unordered_map<std::string, size_t>& value_counts, Dictionary& dict) {
    switch (node.type()) {
        case simdjson::dom::element_type::OBJECT:
            for (simdjson::dom::key_value_pair field : node.get_object().value()) {
                std::string full_name = prefix.empty() ? std::string(field.key) : prefix + "." + std::string(field.key);
                collectFieldsSimd(field.value, full_name, fields, value_counts, dict);
            }
            break;
        case simdjson::dom::element_type::ARRAY:
            {
                size_t i = 0;
                for (simdjson::dom::element child : node.get_array().value()) {
                    std::string full_name = prefix + "[" + std::to_string(i) + "]";
                    collectFieldsSimd(child, full_name, fields, value_counts, dict);
                    i++;
                }
            }
            break;
        default:
            // It's a scalar type
            fields.insert(prefix);
            std::string value_str;
            switch(node.type()) {
                case simdjson::dom::element_type::STRING:
                    value_str = std::string(node.get_string().value());
                    break;
                case simdjson::dom::element_type::INT64:
                    value_str = std::to_string(node.get_int64().value());
                    break;
                case simdjson::dom::element_type::UINT64:
                    value_str = std::to_string(node.get_uint64().value());
                    break;
                case simdjson::dom::element_type::DOUBLE: {
                    std::ostringstream oss;
                    oss << std::setprecision(17) << node.get_double().value();
                    value_str = oss.str();
                    break;
                }
                case simdjson::dom::element_type::BOOL:
                    value_str = node.get_bool().value() ? "true" : "false";
                    break;
                case simdjson::dom::element_type::NULL_VALUE:
                    value_str = "null";
                    break;
                default: // Should not happen
                    break;
            }
            dict.addFieldValue(prefix, value_str);
            value_counts[prefix]++;
            break;
    }
}

}
