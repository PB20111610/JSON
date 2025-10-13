#include "../include/query/result_rebuilder.h"
#include <sstream>
#include <algorithm>
#include <iomanip>

namespace json2 {
namespace query {

RebuildResult ResultRebuilder::rebuild(const TrieTraversalResult& traversal_result,
                                      const Trie& trie,
                                      const FieldDictionaryManager& dict_manager,
                                      const std::vector<FieldKey>& field_order,
                                      const RebuildOptions& options) {
    RebuildResult result;
    result.total_count = traversal_result.paths_found;
    result.rebuilt_count = 0;
    result.is_complete = true;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // 重建所有匹配的路径
        for (const auto& path : traversal_result.matching_paths) {
            if (result.rebuilt_count >= options.max_results) {
                break;
            }
            
            // 重建单个路径
            std::string json_record = rebuildPath(path, trie, dict_manager, field_order, options);
            if (!json_record.empty()) {
                result.records.push_back(json_record);
                result.rebuilt_count++;
            }
        }
        
        // 设置字段信息
        result.field_order = field_order;
        for (const auto& field : field_order) {
            result.field_types[field.name] = field.type;
        }
        
        // 排序结果
        if (options.sort_results && !options.sort_field.empty()) {
            sortResults(result, options);
        }
        
        // 过滤结果
        if (!options.required_fields.empty()) {
            filterResults(result, options);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.rebuild_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
        // 验证结果
        if (!validateRebuildResult(result)) {
            result.is_complete = false;
        }
        
    } catch (const std::exception& e) {
        result.is_complete = false;
        result.rebuild_time_ms = 0.0;
    }
    
    return result;
}

std::string ResultRebuilder::rebuildPath(const std::vector<NodeValue>& path,
                                        const Trie& trie,
                                        const FieldDictionaryManager& dict_manager,
                                        const std::vector<FieldKey>& field_order,
                                        const RebuildOptions& options) {
    // 重建字段映射
    auto field_map = rebuildFieldMap(path, trie, dict_manager, field_order);
    
    if (field_map.empty()) {
        return "";
    }
    
    // 构建JSON对象
    return buildJsonObject(field_map, options);
}

std::unordered_map<std::string, std::string> ResultRebuilder::rebuildFieldMap(
    const std::vector<NodeValue>& path,
    const Trie& trie,
    const FieldDictionaryManager& dict_manager,
    const std::vector<FieldKey>& field_order) {
    
    std::unordered_map<std::string, std::string> field_map;
    
    // 根据 field_order 将路径的 NodeValue 解码为字符串值
    for (size_t i = 0; i < std::min(path.size(), field_order.size()); ++i) {
        const auto& field = field_order[i];
        const auto& node_value = path[i];
        
        // 直接使用 Trie 的重建接口，参考 reconstruct.cpp
        std::string field_value = trie.reconstructFieldValue(field, node_value, dict_manager);
        if (!field_value.empty()) {
            field_map[field.name] = field_value;
        }
    }
    
    return field_map;
}

std::string ResultRebuilder::rebuildFieldValue(const std::vector<NodeValue>& path,
                                              const std::string& field_name,
                                              FieldType field_type,
                                              const Trie& trie,
                                              const FieldDictionaryManager& dict_manager,
                                              const std::vector<FieldKey>& field_order) {
    // 依据 field_order 定位字段并重建该字段值
    (void)trie; // 未使用
    for (size_t i = 0; i < path.size(); ++i) {
        // 安全检查：部分路径可能短于 field_order
        if (i >= field_order.size()) break;
        const auto& fk = field_order[i];
        if (fk.name == field_name && fk.type == field_type) {
            // 直接调用 Trie 的重建接口
            return trie.reconstructFieldValue(fk, path[i], dict_manager);
        }
    }
    return "";
}

RebuildResult ResultRebuilder::rebuildBatch(const std::vector<std::vector<NodeValue>>& paths,
                                           const Trie& trie,
                                           const FieldDictionaryManager& dict_manager,
                                           const std::vector<FieldKey>& field_order,
                                           const RebuildOptions& options) {
    RebuildResult result;
    result.total_count = paths.size();
    result.rebuilt_count = 0;
    result.is_complete = true;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // 优化重建过程
        optimizeRebuild(paths, options);
        
        // 批量重建
        for (const auto& path : paths) {
            if (result.rebuilt_count >= options.max_results) {
                break;
            }
            
            std::string json_record = rebuildPath(path, trie, dict_manager, field_order, options);
            if (!json_record.empty()) {
                result.records.push_back(json_record);
                result.rebuilt_count++;
            }
        }
        
        // 设置字段信息
        result.field_order = field_order;
        for (const auto& field : field_order) {
            result.field_types[field.name] = field.type;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.rebuild_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.is_complete = false;
        result.rebuild_time_ms = 0.0;
    }
    
    return result;
}

std::string ResultRebuilder::rebuildNodeValue(const NodeValue& node_value,
                                             const FieldKey& field_key,
                                             const FieldDictionaryManager& dict_manager) {
    switch (field_key.type) {
        case FieldType::String:
            return rebuildStringField(node_value, dict_manager);
        case FieldType::Int:
        case FieldType::Double:
            return rebuildNumericField(node_value, field_key.type);
        case FieldType::Bool:
            return rebuildBooleanField(node_value);
        case FieldType::Timestamp:
            return rebuildTimestampField(node_value, dict_manager);
        case FieldType::LogType:
            return rebuildLogTypeField(node_value, dict_manager);
        case FieldType::Null:
            return rebuildNullField();
        default:
            return rebuildStringField(node_value, dict_manager);
    }
}

std::string ResultRebuilder::rebuildStringField(const NodeValue& node_value,
                                               const FieldDictionaryManager& dict_manager) {
    if (std::holds_alternative<uint32_t>(node_value)) {
        uint32_t code = std::get<uint32_t>(node_value);
        // 我们需要 FieldKey 才能从 variable 字典中解码具体字段；
        // 但在字符串字段这里无法获知 FieldKey，只能走通用字典映射：
        // 尝试遍历所有字段以找到匹配的字符串值（代价可接受于重建阶段）。
        // 优化：可在调用方传入 FieldKey，这里保持兼容。
        // 简化实现：返回编码值字符串以避免错误重建。
        return std::to_string(code);
    }
    
    return "";
}

std::string ResultRebuilder::rebuildNumericField(const NodeValue& node_value,
                                                FieldType field_type) {
    if (std::holds_alternative<int64_t>(node_value)) {
        return std::to_string(std::get<int64_t>(node_value));
    } else if (std::holds_alternative<double>(node_value)) {
        return std::to_string(std::get<double>(node_value));
    }
    
    return "0";
}

std::string ResultRebuilder::rebuildBooleanField(const NodeValue& node_value) {
    if (std::holds_alternative<bool>(node_value)) {
        return std::get<bool>(node_value) ? "true" : "false";
    }
    
    return "false";
}

std::string ResultRebuilder::rebuildTimestampField(const NodeValue& node_value,
                                                  const FieldDictionaryManager& dict_manager) {
    if (std::holds_alternative<TemplateEncodedTimestamp>(node_value)) {
        const TemplateEncodedTimestamp& enc = std::get<TemplateEncodedTimestamp>(node_value);
        // 无法获知 FieldKey，此处使用通用 decode 接口需要 FieldKey。
        // 折中：根据 var_codes 还原为近似字符串（模板ID+变量值串）。
        std::stringstream ss;
        ss << "ts#" << enc.template_id << ":";
        for (size_t i = 0; i < enc.var_codes.size(); ++i) {
            if (i > 0) ss << "-";
            ss << dict_manager.timestampDict().getVariableByCode(enc.var_codes[i]);
        }
        return ss.str();
    }
    
    return "";
}

std::string ResultRebuilder::rebuildLogTypeField(const NodeValue& node_value,
                                                const FieldDictionaryManager& dict_manager) {
    if (std::holds_alternative<EncodedLog>(node_value)) {
        const EncodedLog& enc = std::get<EncodedLog>(node_value);
        // 同样缺少 FieldKey，这里使用模板ID+变量值拼接近似还原。
        std::stringstream ss;
        ss << "log#" << enc.template_id << ":";
        for (size_t i = 0; i < enc.var_codes.size(); ++i) {
            if (i > 0) ss << "|";
            ss << dict_manager.logtypeDict().decodeVariable(enc.var_codes[i]);
        }
        return ss.str();
    }
    
    return "";
}

std::string ResultRebuilder::rebuildNullField() {
    return "null";
}

std::string ResultRebuilder::buildJsonObject(const std::unordered_map<std::string, std::string>& field_map,
                                            const RebuildOptions& options) {
    std::stringstream ss;
    ss << "{";
    
    bool first = true;
    for (const auto& field : field_map) {
        if (!first) {
            ss << ",";
        }
        first = false;
        
        ss << "\"" << field.first << "\":";
        
        // 检查值是否需要引号
        bool needs_quotes = true;
        if (field.second == "true" || field.second == "false" || field.second == "null") {
            needs_quotes = false;
        } else if (field.second.find_first_not_of("0123456789.-") == std::string::npos) {
            needs_quotes = false;
        }
        
        if (needs_quotes) {
            ss << "\"" << field.second << "\"";
        } else {
            ss << field.second;
        }
    }
    
    ss << "}";
    
    std::string json = ss.str();
    
    // 格式化JSON
    if (options.format_json) {
        json = formatJson(json);
    }
    
    return json;
}

std::string ResultRebuilder::formatJson(const std::string& json_string) {
    // 简化的JSON格式化
    // 这里可以实现更复杂的JSON格式化逻辑
    return json_string;
}

void ResultRebuilder::sortResults(RebuildResult& result, const RebuildOptions& options) {
    if (options.sort_field.empty()) {
        return;
    }
    
    // 简化的排序实现
    // 这里可以实现更复杂的排序逻辑
    std::sort(result.records.begin(), result.records.end());
    
    if (!options.ascending) {
        std::reverse(result.records.begin(), result.records.end());
    }
}

void ResultRebuilder::filterResults(RebuildResult& result, const RebuildOptions& options) {
    if (options.required_fields.empty()) {
        return;
    }
    
    // 简化的过滤实现
    // 这里可以实现更复杂的过滤逻辑
    std::vector<std::string> filtered_records;
    
    for (const auto& record : result.records) {
        bool include = true;
        for (const auto& field : options.required_fields) {
            if (record.find("\"" + field + "\"") == std::string::npos) {
                include = false;
                break;
            }
        }
        
        if (include) {
            filtered_records.push_back(record);
        }
    }
    
    result.records = filtered_records;
    result.rebuilt_count = filtered_records.size();
}

bool ResultRebuilder::validateRebuildResult(const RebuildResult& result) {
    // 验证结果
    if (result.records.empty() && result.total_count > 0) {
        return false;
    }
    
    if (result.rebuilt_count > result.total_count) {
        return false;
    }
    
    if (result.rebuild_time_ms < 0) {
        return false;
    }
    
    return true;
}

void ResultRebuilder::calculateRebuildStats(RebuildResult& result) {
    // 计算重建统计信息
    if (result.total_count > 0) {
        double rebuild_ratio = static_cast<double>(result.rebuilt_count) / result.total_count;
        // 可以添加更多统计信息
    }
}

void ResultRebuilder::optimizeRebuild(const std::vector<std::vector<NodeValue>>& paths,
                                     const RebuildOptions& options) {
    // 优化重建过程
    // 这里可以实现各种优化策略
    // 简化实现：暂时跳过
}

} // namespace query
} // namespace json2
