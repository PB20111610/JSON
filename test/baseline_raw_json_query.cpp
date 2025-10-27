#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <limits>

// Include nlohmann JSON library
#include "../include/nlohmann/json.hpp"

using json = nlohmann::json;

// Forward declarations
class RawJsonQueryEngine;
void printUsage(const char* program_name);

// Query result structures
struct FieldExistenceResult {
    bool exists = false;
    std::string error_message;
};

// Dictionary query result structure removed as it's not applicable to raw JSON querying

struct ExactMatchResult {
    size_t count = 0;
    std::vector<std::string> records;
    std::string error_message;
};

struct RangeQueryResult {
    size_t count = 0;
    std::vector<std::string> records;
    std::string error_message;
};

struct AggregateResult {
    double value = 0.0;
    std::string error_message;
};

struct GroupByResult {
    size_t total_count = 0;
    size_t groups_count = 0;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> grouped_values;
    std::string error_message;
};

// Complex query result structure
struct ComplexQueryResult {
    size_t count = 0;
    std::vector<std::string> records;
    std::string error_message;
};

// Raw JSON query engine for baseline comparison
class RawJsonQueryEngine {
private:
    std::vector<json> json_records_;
    std::string data_file_path_;

public:
    RawJsonQueryEngine() = default;

    bool loadDataFromFile(const std::string& file_path) {
        data_file_path_ = file_path;
        json_records_.clear();

        try {
            std::ifstream file(file_path);
            if (!file.is_open()) {
                std::cerr << "Error: Could not open file " << file_path << std::endl;
                return false;
            }

            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty()) {
                    try {
                        json j = json::parse(line);
                        json_records_.push_back(j);
                    } catch (const std::exception& e) {
                        std::cerr << "Warning: Failed to parse JSON line: " << e.what() << std::endl;
                    }
                }
            }

            file.close();
            std::cout << "Successfully loaded " << json_records_.size() << " JSON records from " << file_path << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error loading data: " << e.what() << std::endl;
            return false;
        }
    }

    // Field existence query
    FieldExistenceResult checkFieldExistence(const std::string& field_name) {
        FieldExistenceResult result;

        if (json_records_.empty()) {
            result.error_message = "No data loaded";
            return result;
        }

        auto start = std::chrono::high_resolution_clock::now();

        // Check if field exists in any record
        for (const auto& record : json_records_) {
            if (record.contains(field_name)) {
                result.exists = true;
                break;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) << duration.count() << " ms" << std::endl;

        return result;
    }

    // Dictionary query functionality removed as it's not applicable to raw JSON querying

    // Exact match query
    ExactMatchResult executeExactMatchQuery(const std::string& field_name, const std::string& value) {
        ExactMatchResult result;

        if (json_records_.empty()) {
            result.error_message = "No data loaded";
            return result;
        }

        auto start = std::chrono::high_resolution_clock::now();

        for (const auto& record : json_records_) {
            if (record.contains(field_name)) {
                try {
                    std::string record_value = record[field_name].get<std::string>();
                    if (record_value == value) {
                        result.count++;
                        if (result.records.size() < 3) { // Only store first few examples
                            result.records.push_back(record.dump());
                        }
                    }
                } catch (...) {
                    // Handle non-string values
                    std::string record_value = record[field_name].dump();
                    if (record_value == value) {
                        result.count++;
                        if (result.records.size() < 3) { // Only store first few examples
                            result.records.push_back(record.dump());
                        }
                    }
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) << duration.count() << " ms" << std::endl;

        return result;
    }

    // Range query (for string comparison)
    RangeQueryResult executeRangeQuery(const std::string& field_name, const std::string& lower_bound, const std::string& upper_bound) {
        RangeQueryResult result;

        if (json_records_.empty()) {
            result.error_message = "No data loaded";
            return result;
        }

        auto start = std::chrono::high_resolution_clock::now();

        for (const auto& record : json_records_) {
            if (record.contains(field_name)) {
                try {
                    std::string record_value = record[field_name].get<std::string>();
                    if (record_value >= lower_bound && record_value <= upper_bound) {
                        result.count++;
                        if (result.records.size() < 3) { // Only store first few examples
                            result.records.push_back(record.dump());
                        }
                    }
                } catch (...) {
                    // Handle non-string values
                    std::string record_value = record[field_name].dump();
                    if (record_value >= lower_bound && record_value <= upper_bound) {
                        result.count++;
                        if (result.records.size() < 3) { // Only store first few examples
                            result.records.push_back(record.dump());
                        }
                    }
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) << duration.count() << " ms" << std::endl;

        return result;
    }

    // Aggregate query
    AggregateResult executeAggregateQuery(const std::string& function, const std::string& field_name) {
        AggregateResult result;

        if (json_records_.empty()) {
            result.error_message = "No data loaded";
            return result;
        }

        auto start = std::chrono::high_resolution_clock::now();

        if (function == "COUNT") {
            if (field_name.empty()) {
                // COUNT(*)
                result.value = static_cast<double>(json_records_.size());
            } else {
                // COUNT(field)
                size_t count = 0;
                for (const auto& record : json_records_) {
                    if (record.contains(field_name)) {
                        count++;
                    }
                }
                result.value = static_cast<double>(count);
            }
        } else if (function == "SUM" || function == "AVG" || function == "MAX" || function == "MIN") {
            if (field_name.empty()) {
                result.error_message = "Field name required for " + function + " aggregate function";
                return result;
            }

            double sum = 0.0;
            size_t count = 0;
            double min_val = std::numeric_limits<double>::max();
            double max_val = std::numeric_limits<double>::lowest();

            for (const auto& record : json_records_) {
                if (record.contains(field_name)) {
                    try {
                        double value = record[field_name].get<double>();
                        sum += value;
                        count++;
                        if (value < min_val) min_val = value;
                        if (value > max_val) max_val = value;
                    } catch (...) {
                        // Skip non-numeric values
                    }
                }
            }

            if (count == 0) {
                result.error_message = "No numeric values found for field " + field_name;
                return result;
            }

            if (function == "SUM") {
                result.value = sum;
            } else if (function == "AVG") {
                result.value = sum / count;
            } else if (function == "MAX") {
                result.value = max_val;
            } else if (function == "MIN") {
                result.value = min_val;
            }
        } else {
            result.error_message = "Unknown aggregate function: " + function;
            return result;
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) << duration.count() << " ms" << std::endl;

        return result;
    }

    // Group by query
    GroupByResult executeGroupByQuery(const std::vector<std::string>& field_names) {
        GroupByResult result;

        if (json_records_.empty()) {
            result.error_message = "No data loaded";
            return result;
        }

        if (field_names.empty()) {
            result.error_message = "At least one field name required for GROUP BY";
            return result;
        }

        auto start = std::chrono::high_resolution_clock::now();

        // Map to store grouped values: group_key -> {count -> value}
        std::unordered_map<std::string, std::unordered_map<std::string, double>> grouped_data;

        for (const auto& record : json_records_) {
            result.total_count++;

            // Create group key from field values
            std::string group_key;
            bool has_all_fields = true;

            for (size_t i = 0; i < field_names.size(); ++i) {
                const std::string& field = field_names[i];
                if (i > 0) group_key += "|"; // Separator

                if (record.contains(field)) {
                    try {
                        group_key += record[field].get<std::string>();
                    } catch (...) {
                        group_key += record[field].dump();
                    }
                } else {
                    has_all_fields = false;
                    break;
                }
            }

            if (has_all_fields) {
                // Increment count for this group
                grouped_data[group_key]["COUNT"] += 1.0;
            }
        }

        result.groups_count = grouped_data.size();
        result.grouped_values = grouped_data;

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) << duration.count() << " ms" << std::endl;

        return result;
    }

    // Complex query (simple implementation for baseline comparison)
    ComplexQueryResult executeComplexQuery(const std::string& query_expression) {
        ComplexQueryResult result;

        if (json_records_.empty()) {
            result.error_message = "No data loaded";
            return result;
        }

        auto start = std::chrono::high_resolution_clock::now();

        // For baseline purposes, we'll implement a simple parser for basic queries
        // This is a simplified implementation that handles basic field:value queries
        // and simple logical operations (AND)
        
        try {
            // Split the query expression into tokens
            std::istringstream iss(query_expression);
            std::vector<std::string> tokens;
            std::string token;
            while (iss >> token) {
                tokens.push_back(token);
            }

            // Process each record
            for (const auto& record : json_records_) {
                bool matches = true;
                
                // Simple parsing - look for field:value pairs
                for (size_t i = 0; i < tokens.size(); i++) {
                    const std::string& current_token = tokens[i];
                    
                    // Handle logical operators
                    if (current_token == "AND" || current_token == "and") {
                        continue; // Skip logical operators in this simple implementation
                    }
                    
                    // Handle field:value pairs
                    size_t colon_pos = current_token.find(':');
                    if (colon_pos != std::string::npos) {
                        std::string field = current_token.substr(0, colon_pos);
                        std::string value = current_token.substr(colon_pos + 1);
                        
                        // Remove quotes if present
                        if (!value.empty() && value.front() == '"' && value.back() == '"') {
                            value = value.substr(1, value.length() - 2);
                        }
                        
                        // Check if record has this field with this value
                        if (record.contains(field)) {
                            std::string record_value;
                            try {
                                record_value = record[field].get<std::string>();
                            } catch (...) {
                                record_value = record[field].dump();
                            }
                            
                            if (record_value != value) {
                                matches = false;
                                break;
                            }
                        } else {
                            matches = false;
                            break;
                        }
                    }
                }
                
                if (matches) {
                    result.count++;
                    if (result.records.size() < 3) { // Only store first few examples
                        result.records.push_back(record.dump());
                    }
                }
            }
        } catch (const std::exception& e) {
            result.error_message = "Error parsing complex query: " + std::string(e.what());
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(end - start);
        std::cout << "  Query time: " << std::fixed << std::setprecision(2) << duration.count() << " ms" << std::endl;

        return result;
    }
};

// Print usage information
void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] --data-file FILE\n";
    std::cout << "\nOptions:\n";
    std::cout << "  --field FIELD_NAME              Check field existence\n";
    std::cout << "  --point FIELD_NAME VALUE        Exact match query\n";
    std::cout << "  --range FIELD_NAME LOW HIGH     Range query\n";
    std::cout << "  --aggregate FUNCTION [FIELD]    Aggregate query (COUNT, SUM, AVG, MAX, MIN)\n";
    std::cout << "  --complex QUERY_EXPRESSION      Complex query with logical operations\n";
    std::cout << "  --group FIELD_NAMES...          Group by query\n";
    std::cout << "  --data-file FILE                JSON file containing data (one JSON object per line)\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << " --data-file test_data.json --field user\n";
    std::cout << "  " << program_name << " --data-file test_data.json --point user postgres\n";
    std::cout << "  " << program_name << " --data-file test_data.json --range pid 7880 7890\n";
    std::cout << "  " << program_name << " --data-file test_data.json --aggregate COUNT\n";
    std::cout << "  " << program_name << " --data-file test_data.json --aggregate SUM pid\n";
    std::cout << "  " << program_name << " --data-file test_data.json --complex \"user:postgres AND dbname:example\"\n";
    std::cout << "  " << program_name << " --data-file test_data.json --group user dbname\n";
    std::cout << "  " << program_name << " --data-file test_data.json --point message \"error occurred\"\n";
    std::cout << "  " << program_name << " --data-file test_data.json --point name \"John Doe\" --range city \"New York\" \"San Francisco\"\n";
    std::cout << "\nNote: Use quotes around values that contain spaces.\n";
    std::cout << "\nMultiple queries can be executed in sequence:\n";
    std::cout << "  " << program_name << " --data-file test_data.json --field user --point user postgres --aggregate COUNT\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string data_file = "test_data.json"; // Default file
    RawJsonQueryEngine engine;

    // Parse command line arguments
    std::vector<std::pair<std::string, std::vector<std::string>>> queries;
    std::string current_query_type;
    std::vector<std::string> current_query_params;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--data-file") {
            if (i + 1 < argc) {
                data_file = argv[++i];
            } else {
                std::cerr << "Error: --data-file requires a file argument" << std::endl;
                return 1;
            }
        } else if (arg == "--field" || arg == "--point" || 
                   arg == "--range" || arg == "--aggregate" || arg == "--complex" || arg == "--group") {
            // Save previous query if exists
            if (!current_query_type.empty()) {
                queries.push_back({current_query_type, current_query_params});
                current_query_type.clear();
                current_query_params.clear();
            }

            // Set new query type
            if (arg == "--field") current_query_type = "field";
            else if (arg == "--point") current_query_type = "point";
            else if (arg == "--range") current_query_type = "range";
            else if (arg == "--aggregate") current_query_type = "aggregate";
            else if (arg == "--complex") current_query_type = "complex";
            else if (arg == "--group") current_query_type = "group";
        } else if (!current_query_type.empty()) {
            // Add parameter to current query
            // Handle quoted strings for parameters with spaces
            if (!arg.empty() && arg.front() == '"' && arg.back() != '"') {
                // Start of a quoted string, accumulate until we find the closing quote
                std::string quoted_arg = arg.substr(1); // Remove opening quote
                while (i + 1 < argc) {
                    std::string next_arg = argv[++i];
                    if (!next_arg.empty() && next_arg.back() == '"') {
                        // Found closing quote
                        quoted_arg += " " + next_arg.substr(0, next_arg.length() - 1); // Remove closing quote
                        break;
                    } else {
                        quoted_arg += " " + next_arg;
                    }
                }
                current_query_params.push_back(quoted_arg);
            } else if (!arg.empty() && arg.front() == '"' && arg.back() == '"') {
                // Fully quoted argument
                current_query_params.push_back(arg.substr(1, arg.length() - 2));
            } else {
                // Regular argument
                current_query_params.push_back(arg);
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    // Save last query if exists
    if (!current_query_type.empty()) {
        queries.push_back({current_query_type, current_query_params});
    }

    if (queries.empty()) {
        std::cerr << "No queries specified. Use --help for usage information." << std::endl;
        return 1;
    }

    std::cout << "Raw JSON Query Baseline Test" << std::endl;
    std::cout << "Data file: " << data_file << std::endl;

    if (!std::filesystem::exists(data_file)) {
        std::cerr << "Error: Data file does not exist: " << data_file << std::endl;
        return 1;
    }

    // Load JSON data
    if (!engine.loadDataFromFile(data_file)) {
        std::cerr << "Failed to load JSON data" << std::endl;
        return 1;
    }

    // Execute all queries
    for (const auto& query : queries) {
        const std::string& query_type = query.first;
        const std::vector<std::string>& params = query.second;

        std::cout << "\n=== Executing " << query_type << " Query ===" << std::endl;

        try {
            if (query_type == "field") {
                if (params.empty()) {
                    std::cout << "Field existence query requires a field name parameter" << std::endl;
                    continue;
                }

                std::string field_name = params[0];
                std::cout << "Checking existence of field: " << field_name << std::endl;

                auto result = engine.checkFieldExistence(field_name);
                std::cout << "Field: " << field_name << std::endl;
                std::cout << "  Exists: " << (result.exists ? "Yes" : "No") << std::endl;
                if (!result.error_message.empty()) {
                    std::cout << "  Error: " << result.error_message << std::endl;
                }

            } else if (query_type == "point") {
                if (params.size() < 2) {
                    std::cout << "Exact match query requires field name and value parameters" << std::endl;
                    continue;
                }

                std::string field_name = params[0];
                std::string value = params[1];
                std::cout << "Executing exact match query: " << field_name << " = \"" << value << "\"" << std::endl;

                auto result = engine.executeExactMatchQuery(field_name, value);
                std::cout << "  Records found: " << result.count << std::endl;
                if (!result.error_message.empty()) {
                    std::cout << "  Error: " << result.error_message << std::endl;
                } else {
                    // Show first few matching records
                    std::cout << "  Matching record examples:" << std::endl;
                    for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                        std::cout << "    " << result.records[i] << std::endl;
                    }
                }

            } else if (query_type == "range") {
                if (params.size() < 3) {
                    std::cout << "Range query requires field name, lower bound, and upper bound parameters" << std::endl;
                    continue;
                }

                std::string field_name = params[0];
                std::string lower_bound = params[1];
                std::string upper_bound = params[2];
                std::cout << "Executing range query: " << field_name << " in [" << lower_bound << ", " << upper_bound << "]" << std::endl;

                auto result = engine.executeRangeQuery(field_name, lower_bound, upper_bound);
                std::cout << "  Records found: " << result.count << std::endl;
                if (!result.error_message.empty()) {
                    std::cout << "  Error: " << result.error_message << std::endl;
                } else {
                    // Show first few matching records
                    std::cout << "  Matching record examples:" << std::endl;
                    for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                        std::cout << "    " << result.records[i] << std::endl;
                    }
                }

            } else if (query_type == "aggregate") {
                if (params.size() < 1) {
                    std::cout << "Aggregate query requires at least a function parameter" << std::endl;
                    continue;
                }

                std::string function = params[0];
                std::string field_name = (params.size() > 1) ? params[1] : "";
                std::cout << "Executing aggregate query: " << function << "(" << (field_name.empty() ? "*" : field_name) << ")" << std::endl;

                auto result = engine.executeAggregateQuery(function, field_name);
                std::cout << "  Aggregate value: " << result.value << std::endl;
                if (!result.error_message.empty()) {
                    std::cout << "  Error: " << result.error_message << std::endl;
                }

            } else if (query_type == "complex") {
                if (params.empty()) {
                    std::cout << "Complex query requires a query expression parameter" << std::endl;
                    continue;
                }

                // Join all parameters to form the complex query expression
                std::string query_expression;
                for (size_t i = 0; i < params.size(); ++i) {
                    if (i > 0) query_expression += " ";
                    query_expression += params[i];
                }
                
                std::cout << "Executing complex query: " << query_expression << std::endl;

                auto result = engine.executeComplexQuery(query_expression);
                std::cout << "  Records found: " << result.count << std::endl;
                if (!result.error_message.empty()) {
                    std::cout << "  Error: " << result.error_message << std::endl;
                } else {
                    // Show first few matching records
                    std::cout << "  Matching record examples:" << std::endl;
                    for (size_t i = 0; i < std::min(size_t(3), result.records.size()); ++i) {
                        std::cout << "    " << result.records[i] << std::endl;
                    }
                }
            } else if (query_type == "group") {
                if (params.empty()) {
                    std::cout << "Group by query requires at least one field name parameter" << std::endl;
                    continue;
                }

                std::cout << "Executing group by query on fields: ";
                for (size_t i = 0; i < params.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << params[i];
                }
                std::cout << std::endl;

                auto result = engine.executeGroupByQuery(params);
                std::cout << "  Total records: " << result.total_count << std::endl;
                std::cout << "  Groups found: " << result.groups_count << std::endl;
                if (!result.error_message.empty()) {
                    std::cout << "  Error: " << result.error_message << std::endl;
                } else {
                    // Show first few group results
                    std::cout << "  Group examples:" << std::endl;
                    size_t count = 0;
                    for (const auto& group_entry : result.grouped_values) {
                        if (count >= 5) break;

                        std::cout << "    Group key: " << group_entry.first << " => COUNT=" << group_entry.second.at("COUNT") << std::endl;
                        count++;
                    }
                }
            } else {
                std::cout << "Unknown query type: " << query_type << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Query execution failed: " << e.what() << std::endl;
        }
    }

    std::cout << "\nRaw JSON query testing completed!" << std::endl;
    return 0;
}