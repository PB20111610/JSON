#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <set>
#include <map>
#include <simdjson.h>
#include "../include/field_parser.h"
#include "../include/field_dictionary_manager.h"
#include "../include/field_key.h"

namespace json2 {

// Get a human-readable name for each field type
std::string getFieldTypeName(FieldType type) {
    switch (type) {
        case FieldType::STRING: return "String";
        case FieldType::INT64: return "Numeric";
        case FieldType::DOUBLE: return "Numeric";
        case FieldType::BOOL: return "Boolean";
        case FieldType::TIMESTAMP: return "Timestamp";
        case FieldType::LOGTYPE: return "LogType";
        case FieldType::NULL_TYPE: return "Other";
        case FieldType::ARRAY: return "Array";

        default: return "Other";
    }
}

} // namespace json2

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <json_file>" << std::endl;
        return 1;
    }
    
    std::string filename = argv[1];
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return 1;
    }
    
    // Statistics tracking
    std::map<json2::FieldKey, size_t> field_key_occurrences;  // Occurrences of each FieldKey
    std::unordered_map<std::string, size_t> type_occurrence_sum; // Sum of occurrences per type
    std::unordered_map<std::string, size_t> type_unique_values_sum; // Sum of unique values per type
    std::unordered_map<std::string, size_t> type_field_count; // Count of FieldKeys per type
    json2::FieldDictionaryManager global_manager; // Global manager to track all values
    size_t total_records = 0;
    
    // Process file line by line
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            simdjson::dom::parser parser;
            simdjson::dom::element doc;
            
            auto error = parser.parse(line).get(doc);
            if (error) {
                continue; // Skip malformed records
            }
            
            // Collect all fields in the record
            std::set<json2::FieldKey> fields_in_record;
            std::unordered_map<json2::FieldKey, size_t> field_value_counts;
            json2::FieldDictionaryManager record_manager; // Per-record manager
            json2::FieldParser::collectAllFields(doc, "", 0, fields_in_record, field_value_counts, record_manager);
            
            // Process each field in record
            for (const auto& field_key : fields_in_record) {
                // Count FieldKey occurrences (new variable: tracks how many records contain each FieldKey)
                field_key_occurrences[field_key]++;
                
                // Track type occurrence sums (sum of all FieldKey occurrences per type)
                std::string type_name = json2::getFieldTypeName(field_key.type);
                type_occurrence_sum[type_name]++;
            }
            
            // Merge record manager data into global manager
            // This ensures we collect all unique values across all records
            for (const auto& field_key : record_manager.getAllFieldsAndTypes()) {
                // Copy all unique values from record manager to global manager
                const std::set<std::string>& unique_values = record_manager.getUniqueValues(field_key);
                for (const auto& value : unique_values) {
                    global_manager.addFieldValue(field_key, field_key.type, value);
                }
            }
            
            total_records++;
        }
    }
    
    file.close();
    
    // Calculate sum of unique values per type and count of FieldKeys per type
    std::set<json2::FieldKey> unique_field_keys;
    for (const auto& pair : field_key_occurrences) {
        const json2::FieldKey& field_key = pair.first;
        unique_field_keys.insert(field_key);
        std::string type_name = json2::getFieldTypeName(field_key.type);
        type_field_count[type_name]++;
    }
    
    for (const auto& field_key : global_manager.getAllFieldsAndTypes()) {
        std::string type_name = json2::getFieldTypeName(field_key.type);
        size_t unique_count = global_manager.getUniqueValueCount(field_key);
        type_unique_values_sum[type_name] += unique_count;
    }
    
    // Output detailed results for verification
    std::cout << "Processing file: " << filename << std::endl;
    std::cout << "Total records processed: " << total_records << std::endl;
    std::cout << "Total unique FieldKeys: " << unique_field_keys.size() << std::endl;
    
    // Field type distribution based on occurrence counts and unique values
    std::cout << "\nDetailed Field Type Distribution:" << std::endl;
    std::cout << "Type\t\tField Count\tOccurrence Sum\tUnique Values Sum\tPercentage" << std::endl;
    std::cout << "------------------------------------------------------------------------" << std::endl;
    
    // Define the order of field types
    std::vector<std::string> field_types = {"String", "Numeric", "Boolean", "LogType", "Timestamp", "Array", "Other"};
    
    // Calculate total field count for percentage calculation
    size_t total_fields = 0;
    for (const auto& type_name : field_types) {
        auto it = type_field_count.find(type_name);
        size_t count = (it != type_field_count.end()) ? it->second : 0;
        total_fields += count;
    }
    
    for (const auto& type_name : field_types) {
        auto field_count_it = type_field_count.find(type_name);
        auto occurrence_it = type_occurrence_sum.find(type_name);
        auto unique_it = type_unique_values_sum.find(type_name);
        
        size_t field_count = (field_count_it != type_field_count.end()) ? field_count_it->second : 0;
        size_t occurrence_sum = (occurrence_it != type_occurrence_sum.end()) ? occurrence_it->second : 0;
        size_t unique_values_sum = (unique_it != type_unique_values_sum.end()) ? unique_it->second : 0;
        
        double percentage = (total_fields > 0) ? (double)field_count / total_fields * 100.0 : 0.0;
        
        std::cout << type_name;
        if (type_name.length() < 8) std::cout << "\t\t";
        else std::cout << "\t";
        std::cout << field_count << "\t\t"
                  << occurrence_sum << "\t\t"
                  << unique_values_sum << "\t\t\t"
                  << std::fixed << std::setprecision(1) << percentage << "%" << std::endl;
    }
    
    // FieldKey occurrence rates (detailed per field)
    std::cout << "\nDetailed FieldKey Statistics (Top 20):" << std::endl;
    std::cout << "FieldKey\t\t\t\tType\t\tOccurrence Count\tOccurrence Rate\t\tUnique Values" << std::endl;
    std::cout << "--------------------------------------------------------------------------------" << std::endl;
    
    // Sort FieldKeys by occurrence count (descending)
    std::vector<std::pair<json2::FieldKey, size_t>> sorted_field_keys(field_key_occurrences.begin(), field_key_occurrences.end());
    std::sort(sorted_field_keys.begin(), sorted_field_keys.end(), 
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second; // Descending by count
                  return a.first.name < b.first.name; // Ascending by name for ties
              });
    
    // Display top 20 FieldKeys
    size_t display_count = 0;
    for (const auto& pair : sorted_field_keys) {
        if (display_count >= 20) break;
        
        const json2::FieldKey& field_key = pair.first;
        size_t occurrences = pair.second; // This is the FieldKey occurrence count
        double occurrence_rate = total_records > 0 ? (double)occurrences / total_records : 0.0;
        
        std::string type_name = json2::getFieldTypeName(field_key.type);
        size_t unique_values = global_manager.getUniqueValueCount(field_key);
        
        std::cout << field_key.name;
        // Add padding for alignment
        if (field_key.name.length() < 24) std::cout << "\t\t\t";
        else if (field_key.name.length() < 32) std::cout << "\t\t";
        else std::cout << "\t";
        std::cout << type_name;
        if (type_name.length() < 8) std::cout << "\t\t";
        else std::cout << "\t";
        std::cout << occurrences << "\t\t\t"
                  << std::fixed << std::setprecision(2) << occurrence_rate << "\t\t\t"
                  << unique_values << std::endl;
                  
        display_count++;
    }
    
    // Field type redundancy statistics (paper format)
    std::cout << "\n\nField Type Redundancy Statistics (Paper Format):" << std::endl;
    std::cout << "Field Type\tfi\t\tui\t\tPercentage" << std::endl;
    std::cout << "------------------------------------------------------------" << std::endl;
    
    for (const auto& type_name : field_types) {
        auto field_count_it = type_field_count.find(type_name);
        auto occurrence_it = type_occurrence_sum.find(type_name);
        auto unique_it = type_unique_values_sum.find(type_name);
        
        size_t field_count = (field_count_it != type_field_count.end()) ? field_count_it->second : 0;
        size_t occurrence_sum = (occurrence_it != type_occurrence_sum.end()) ? occurrence_it->second : 0;
        size_t unique_values_sum = (unique_it != type_unique_values_sum.end()) ? unique_it->second : 0;
        
        // Calculate fi (occurrence frequency) as total occurrences / (total records * field count)
        // This gives us the average occurrence rate per field of this type
        double fi = (total_records > 0 && field_count > 0) ? (double)occurrence_sum / (total_records * field_count) : 0.0;
        
        // Calculate ui (unique-value cardinality) as total unique values for all fields of this type
        // This is the sum, not the average
        size_t ui = unique_values_sum;
        
        // Calculate percentage as field count / total fields
        double percentage = (total_fields > 0) ? (double)field_count / total_fields * 100.0 : 0.0;
        
        std::cout << type_name;
        if (type_name.length() < 8) std::cout << "\t\t";
        else std::cout << "\t";
        std::cout << std::fixed << std::setprecision(2) << fi << "\t\t"
                  << ui << "\t\t"
                  << std::fixed << std::setprecision(1) << percentage << "%" << std::endl;
    }
    
    // Explanation of calculations
    std::cout << "\n\nCalculation Explanation:" << std::endl;
    std::cout << "--------------------------------------------------------------------------------" << std::endl;
    std::cout << "fi (Occurrence frequency) = (Sum of occurrences for all fields of this type) / (Total records * Number of fields of this type)" << std::endl;
    std::cout << "ui (Unique-value cardinality) = Sum of unique values for all fields of this type" << std::endl;
    std::cout << "Percentage = (Number of fields of this type) / (Total number of fields) * 100%" << std::endl;
    
    return 0;
}