#include "../include/field_analyzer.h"
#include <set>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <simdjson.h>
#include <sstream>
#include "../include/field_dictionary_manager.h"
#include "../include/field_parser.h"
#include <regex>
#include <cctype>
#include <chrono>
#include <cmath> 

namespace json2 {

// 块级字段统计结构，用于准确收集当前数据块的统计信息
struct BlockFieldStats {
    size_t total_records_in_block = 0;
    std::unordered_map<FieldKey, size_t> field_occurrence_counts; // 字段在当前块中出现的记录数
    std::unordered_map<FieldKey, std::unordered_map<std::string, size_t>> field_value_distributions; // 字段值的分布统计
    std::set<FieldKey> all_fields_in_block; // 当前块中的所有字段
};

// 新的分组策略结构
struct FieldCategory {
    std::vector<std::pair<FieldKey, double>> perfect_fields;      // A组：freq高 + uniq=1
    std::vector<std::pair<FieldKey, double>> stable_diverse;      // B组：freq高 + uniq>1
    std::vector<std::pair<FieldKey, double>> low_frequency;       // C组：freq低
};

// B组内的类型分组
struct TypeGroup {
    std::vector<std::pair<FieldKey, double>> string_fields;
    std::vector<std::pair<FieldKey, double>> numeric_fields;     // Int, Double, Bool
    std::vector<std::pair<FieldKey, double>> logtype_fields;
    std::vector<std::pair<FieldKey, double>> timestamp_fields;
    std::vector<std::pair<FieldKey, double>> array_fields;       // UnstructuredArray
    std::vector<std::pair<FieldKey, double>> other_fields;       // Null, 其他类型
};

// 获取字段的类型分组
std::string getFieldTypeGroup(FieldType type) {
    switch (type) {
        case FieldType::String:     return "string";
        case FieldType::Int:        
        case FieldType::Double:     
        case FieldType::Bool:       return "numeric";
        case FieldType::LogType:    return "logtype";
        case FieldType::Timestamp:  return "timestamp";
        case FieldType::UnstructuredArray: return "array";
        default:                    return "other";
    }
}

// 收集块级字段统计信息的辅助函数
void collectBlockFieldStats(simdjson::dom::element node, 
                           const std::string& prefix, 
                           int depth,
                           BlockFieldStats& block_stats,
                           FieldDictionaryManager& manager) {
    switch (node.type()) {
        case simdjson::dom::element_type::OBJECT:
            // 对于对象类型，只递归处理嵌套字段，不创建扁平字段
            for (simdjson::dom::key_value_pair field : node.get_object().value()) {
                std::string full_name = prefix.empty() ? std::string(field.key) : prefix + "." + std::string(field.key);
                // 只有嵌套字段（depth > 0 或 prefix 不为空）才添加 ~ 前缀
                std::string nested_name = (depth > 0 || !prefix.empty()) ? ("~" + full_name) : full_name;
                collectBlockFieldStats(field.value, nested_name, depth+1, block_stats, manager);
            }
            break;
        case simdjson::dom::element_type::ARRAY: {
            // 处理数组类型
            if (manager.getStructurizeArrays()) {
                // 结构化数组处理
                for (size_t index = 0; index < node.get_array().size(); ++index) {
                    simdjson::dom::element element = node.get_array().at(index);
                    std::string element_name = prefix + "[" + std::to_string(index) + "]";
                    collectBlockFieldStats(element, element_name, depth + 1, block_stats, manager);
                }
            } else {
                // 非结构化数组处理
                FieldType type = FieldParser::inferFieldType(prefix, node, manager);
                std::string array_str = simdjson::to_string(node);
                FieldKey key{prefix, type};
                block_stats.all_fields_in_block.insert(key);
                block_stats.field_occurrence_counts[key]++;
                block_stats.field_value_distributions[key][array_str]++;
                manager.addFieldValue(key, type, array_str);
            }
            break;
        }
        default: {
            FieldType type = FieldParser::inferFieldType(prefix, node, manager, false);
            std::string value_str = simdjson::to_string(node);
            FieldKey key{prefix, type};
            block_stats.all_fields_in_block.insert(key);
            block_stats.field_occurrence_counts[key]++;
            block_stats.field_value_distributions[key][value_str]++;
            manager.addFieldValue(key, type, value_str);
            break;
        }
    }
}

void FieldAnalyzer::analyzeAndSortFields(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<FieldKey>& ordered_fields) {
    // 直接调用分层混合排序策略
    analyzeAndSortFieldsHierarchical(records, manager, ordered_fields);
}

void FieldAnalyzer::analyzeAndSortFieldsHierarchical(const std::vector<std::string>& records, FieldDictionaryManager& manager, std::vector<FieldKey>& ordered_fields) {
    simdjson::dom::parser parser;
    BlockFieldStats block_stats;

    // Phase 0: Collect accurate per-block statistics
    // std::cout << "\n[HIERARCHICAL SORTING] Phase 0: Collecting Per-Block Statistics" << std::endl;
    // std::cout << "=============================================================" << std::endl;

    for (const auto& rec_str : records) {
        try {
            simdjson::dom::element doc = parser.parse(rec_str).value();
            block_stats.total_records_in_block++;
            collectBlockFieldStats(doc, "", 0, block_stats, manager);
        } catch (const simdjson::simdjson_error& e) {
            std::cerr << "simdjson parse error: " << e.what() << std::endl;
            // std::cerr << "[DEBUG] 错误行内容: " << rec_str.substr(0, 100) << std::endl;
        }
    }
    
    // std::cout << "[INFO] Collected statistics for " << block_stats.total_records_in_block << " records" << std::endl;
    // std::cout << "[INFO] Found " << block_stats.all_fields_in_block.size() << " unique fields in block" << std::endl;

    // Configuration parameters for the new grouping strategy
    const double HIGH_FREQUENCY_THRESHOLD = 0.80;  // f_i > 0.80 为高频字段
    const double SMOOTHING_CONSTANT = 2.0;         // C in log(u_i + C)

    // Phase 1: New A/B/C Field Categorization
    FieldCategory field_categories;

    // std::cout << "\n[HIERARCHICAL SORTING] Phase 1: A/B/C Field Categorization" << std::endl;
    // std::cout << "=========================================================" << std::endl;

    for (const auto& key : block_stats.all_fields_in_block) {
        // 使用准确的块级统计数据
        size_t occ = block_stats.field_occurrence_counts[key];
        size_t val_count = block_stats.field_value_distributions[key].size();
        double frequency = (double)occ / block_stats.total_records_in_block;
        double uniqueness_ratio = occ > 0 ? (double)val_count / occ : 0.0;
        
        // 跳过从未出现的字段
        if (occ == 0) {
            // std::cout << "SKIPPED: " << key.name << " (never appears in block)" << std::endl;
            continue;
        }

        // 新的三组分类策略
        // 优先规则：如果唯一值比率>=95%（如严格递增整数、序列号等），无论频率高低，直接归入C组
        if (occ > 0 && uniqueness_ratio >= 0.95) {
            field_categories.low_frequency.emplace_back(key, frequency);
            // std::cout << "C组 (LOW_FREQUENCY - high_uniqueness): " << key.name << " (ratio: " << uniqueness_ratio << ")" << std::endl;
        } else if (frequency > HIGH_FREQUENCY_THRESHOLD && val_count == 1) {
            // A组：高频 + 唯一值=1 (完美字段)
            field_categories.perfect_fields.emplace_back(key, frequency);
            // std::cout << "A组 (PERFECT): " << key.name 
            //           << " (freq: " << std::fixed << std::setprecision(3) << frequency 
            //           << ", uniq: " << val_count << ", occ: " << occ << ")" << std::endl;
        } else if (frequency > HIGH_FREQUENCY_THRESHOLD && val_count > 1) {
            // B组：高频 + 唯一值>1 (结构稳定但值多样)
            field_categories.stable_diverse.emplace_back(key, val_count); // 存储唯一值数量用于排序
            // std::cout << "B组 (STABLE_DIVERSE): " << key.name 
            //           << " (freq: " << std::fixed << std::setprecision(3) << frequency 
            //           << ", uniq: " << val_count << ", occ: " << occ << ")" << std::endl;
        } else {
            // C组：低频字段
            field_categories.low_frequency.emplace_back(key, frequency);
            // std::cout << "C组 (LOW_FREQUENCY): " << key.name 
            //           << " (freq: " << std::fixed << std::setprecision(3) << frequency 
            //           << ", uniq: " << val_count << ", occ: " << occ << ")" << std::endl;
        }
    }

    // Phase 2: Intra-Category Ranking with Type Grouping
    // std::cout << "\n[HIERARCHICAL SORTING] Phase 2: Intra-Category Ranking with Type Grouping" << std::endl;
    // std::cout << "=======================================================================" << std::endl;

    // A组排序：按频率降序（所有字段都是Uniq=1，所以频率是主要排序键）
    std::sort(field_categories.perfect_fields.begin(), field_categories.perfect_fields.end(), 
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // std::cout << "A组 (PERFECT FIELDS) - 按频率降序排序:" << std::endl;
    // for (size_t i = 0; i < field_categories.perfect_fields.size(); ++i) {
    //     const auto& [key, freq] = field_categories.perfect_fields[i];
    //     std::cout << "  " << (i+1) << ". " << key.name 
    //               << " (freq: " << std::fixed << std::setprecision(3) << freq 
    //               << ", type: " << getFieldTypeGroup(key.type) << ")" << std::endl;
    // }

    // B组排序：按类型分组，每个类型内部按唯一值升序
    TypeGroup b_type_groups;
    
    // 将B组字段按类型分组
    for (const auto& [key, uniq_count] : field_categories.stable_diverse) {
        std::string type_group = getFieldTypeGroup(key.type);
        if (type_group == "string") {
            b_type_groups.string_fields.emplace_back(key, uniq_count);
        } else if (type_group == "numeric") {
            b_type_groups.numeric_fields.emplace_back(key, uniq_count);
        } else if (type_group == "logtype") {
            b_type_groups.logtype_fields.emplace_back(key, uniq_count);
        } else if (type_group == "timestamp") {
            b_type_groups.timestamp_fields.emplace_back(key, uniq_count);
        } else if (type_group == "array") {
            b_type_groups.array_fields.emplace_back(key, uniq_count);
        } else {
            b_type_groups.other_fields.emplace_back(key, uniq_count);
        }
    }
    
    // 对每个类型组内部按唯一值升序排序
    auto sortByUniqueCount = [](auto& group) {
        std::sort(group.begin(), group.end(), 
                  [](const auto& a, const auto& b) { return a.second < b.second; });
    };
    
    sortByUniqueCount(b_type_groups.string_fields);
    sortByUniqueCount(b_type_groups.numeric_fields);
    sortByUniqueCount(b_type_groups.logtype_fields);
    sortByUniqueCount(b_type_groups.timestamp_fields);
    sortByUniqueCount(b_type_groups.array_fields);
    sortByUniqueCount(b_type_groups.other_fields);
    
    // std::cout << "\nB组 (STABLE DIVERSE) - 按类型分组，组内按唯一值升序:" << std::endl;
    
    // 按类型顺序输出：String → Numeric → LogType → Timestamp → Array → Other
    // size_t b_rank = 1;
    
    // std::cout << "  String类型字段:" << std::endl;
    // for (const auto& [key, uniq_count] : b_type_groups.string_fields) {
    //     std::cout << "    " << b_rank++ << ". " << key.name 
    //               << " (uniq: " << (size_t)uniq_count << ")" << std::endl;
    // }
    
    // std::cout << "  Numeric类型字段 (Int/Double/Bool):" << std::endl;
    // for (const auto& [key, uniq_count] : b_type_groups.numeric_fields) {
    //     std::cout << "    " << b_rank++ << ". " << key.name 
    //               << " (uniq: " << (size_t)uniq_count << ")" << std::endl;
    // }
    
    // std::cout << "  LogType类型字段:" << std::endl;
    // for (const auto& [key, uniq_count] : b_type_groups.logtype_fields) {
    //     std::cout << "    " << b_rank++ << ". " << key.name 
    //               << " (uniq: " << (size_t)uniq_count << ")" << std::endl;
    // }
    
    // std::cout << "  Timestamp类型字段:" << std::endl;
    // for (const auto& [key, uniq_count] : b_type_groups.timestamp_fields) {
    //     std::cout << "    " << b_rank++ << ". " << key.name 
    //               << " (uniq: " << (size_t)uniq_count << ")" << std::endl;
    // }
    
    // std::cout << "  Array类型字段:" << std::endl;
    // for (const auto& [key, uniq_count] : b_type_groups.array_fields) {
    //     std::cout << "    " << b_rank++ << ". " << key.name 
    //               << " (uniq: " << (size_t)uniq_count << ")" << std::endl;
    // }
    
    // std::cout << "  Other类型字段:" << std::endl;
    // for (const auto& [key, uniq_count] : b_type_groups.other_fields) {
    //     std::cout << "    " << b_rank++ << ". " << key.name 
    //               << " (uniq: " << (size_t)uniq_count << ")" << std::endl;
    // }

    // C组排序：按频率降序
    std::sort(field_categories.low_frequency.begin(), field_categories.low_frequency.end(), 
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // std::cout << "\nC组 (LOW FREQUENCY) - 按频率降序排序:" << std::endl;
    // for (size_t i = 0; i < field_categories.low_frequency.size(); ++i) {
    //     const auto& [key, freq] = field_categories.low_frequency[i];
    //     std::cout << "  " << (i+1) << ". " << key.name 
    //               << " (freq: " << std::fixed << std::setprecision(3) << freq 
    //               << ", type: " << getFieldTypeGroup(key.type) << ")" << std::endl;
    // }

    // Phase 3: Final Order Assembly
    // std::cout << "\n[HIERARCHICAL SORTING] Phase 3: Final Order Assembly" << std::endl;
    // std::cout << "===================================================" << std::endl;
    // std::cout << "Final Order = [A组: Perfect Fields] + [B组: Stable Diverse] + [C组: Low Frequency]" << std::endl;

    ordered_fields.clear();
    
    // 添加A组字段（完美字段，最高优先级）
    for (const auto& [key, _] : field_categories.perfect_fields) {
        ordered_fields.push_back(key);
    }
    
    // 添加B组字段（按类型顺序：String → Numeric → LogType → Timestamp → Array → Other）
    for (const auto& [key, _] : b_type_groups.string_fields) {
        ordered_fields.push_back(key);
    }
    for (const auto& [key, _] : b_type_groups.numeric_fields) {
        ordered_fields.push_back(key);
    }
    for (const auto& [key, _] : b_type_groups.logtype_fields) {
        ordered_fields.push_back(key);
    }
    for (const auto& [key, _] : b_type_groups.timestamp_fields) {
        ordered_fields.push_back(key);
    }
    for (const auto& [key, _] : b_type_groups.array_fields) {
        ordered_fields.push_back(key);
    }
    for (const auto& [key, _] : b_type_groups.other_fields) {
        ordered_fields.push_back(key);
    }
    
    // 添加C组字段（低频字段，最低优先级）
    for (const auto& [key, _] : field_categories.low_frequency) {
        ordered_fields.push_back(key);
    }

    // Enhanced debug output with hierarchical reasoning using accurate block statistics
    // std::cout << "\n[HIERARCHICAL SORTING] Final Hierarchical Order:" << std::endl;
    // std::cout << "===============================================" << std::endl;
    // for (size_t i = 0; i < ordered_fields.size(); ++i) {
    //     const auto& key = ordered_fields[i];
    //     size_t occ = block_stats.field_occurrence_counts[key];
    //     size_t val_count = block_stats.field_value_distributions[key].size();
    //     double frequency = (double)occ / block_stats.total_records_in_block;
    //     double uniqueness_ratio = occ > 0 ? (double)val_count / occ : 0.0;
    //     
    //     // Determine category for display based on new A/B/C grouping
    //     std::string category;
    //     if (frequency > HIGH_FREQUENCY_THRESHOLD && val_count == 1) {
    //         category = "A组(PERFECT)";
    //     } else if (frequency > HIGH_FREQUENCY_THRESHOLD && val_count > 1) {
    //         category = "B组(STABLE)";
    //     } else {
    //         category = "C组(LOW_FREQ)";
    //     }
    //     
    //     std::string type_group = getFieldTypeGroup(key.type);
    //     std::cout << std::setw(4) << (i + 1) << ". [" << category << "] " 
    //               << key.name << "[" << (key.type == FieldType::String ? "String" : 
    //                    key.type == FieldType::Int ? "Int" : 
    //                    key.type == FieldType::Double ? "Double" : 
    //                    key.type == FieldType::Bool ? "Bool" : 
    //                    key.type == FieldType::Timestamp ? "Timestamp" : 
    //                    key.type == FieldType::LogType ? "LogType" : 
    //                    key.type == FieldType::Null ? "Null" : "Other") << "]" 
    //               << " - Occ: " << occ 
    //               << ", Uniq: " << val_count 
    //               << ", Freq: " << std::fixed << std::setprecision(3) << frequency
    //               << ", UniqRatio: " << std::fixed << std::setprecision(3) << uniqueness_ratio
    //               << ", TypeGroup: " << type_group << std::endl;
    // }
    
    // 保留简化的总结输出，便于监控
    // std::cout << "\n[HIERARCHICAL SORTING] Summary:" << std::endl;
    // std::cout << "A组 (Perfect Fields): " << field_categories.perfect_fields.size() << std::endl;
    // std::cout << "B组 (Stable Diverse): " << field_categories.stable_diverse.size() << std::endl;
    // std::cout << "  - String类型: " << b_type_groups.string_fields.size() << std::endl;
    // std::cout << "  - Numeric类型: " << b_type_groups.numeric_fields.size() << std::endl;
    // std::cout << "  - LogType类型: " << b_type_groups.logtype_fields.size() << std::endl;
    // std::cout << "  - Timestamp类型: " << b_type_groups.timestamp_fields.size() << std::endl;
    // std::cout << "  - Array类型: " << b_type_groups.array_fields.size() << std::endl;
    // std::cout << "  - Other类型: " << b_type_groups.other_fields.size() << std::endl;
    // std::cout << "C组 (Low Frequency): " << field_categories.low_frequency.size() << std::endl;
    // std::cout << "Total Fields: " << ordered_fields.size() << std::endl;
}

void FieldAnalyzer::analyzeWithCustomOrder(const std::vector<FieldKey>& custom_order, std::vector<FieldKey>& ordered_fields) {
    ordered_fields = custom_order;
}

// 类型敏感主解析接口（保留，便于单条记录类型推断）
std::vector<std::tuple<std::string, FieldType, Value>> FieldAnalyzer::parseFields(const std::string& record) {
    // 使用统一的字段解析工具
    FieldDictionaryManager temp_manager; // 临时manager用于类型推断
    return FieldParser::parseFields(record, temp_manager);
}

} // namespace json2
