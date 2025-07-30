#include "../include/louds.h"
#include "../include/trie.h"
#include "../include/field_dictionary_manager.h"
#include "../include/variable_dictionary.h"
#include "../include/timestamp_dictionary.h"
#include "../include/logtype_dictionary.h"
#include "../include/field_analyzer.h"
#include "../include/reconstruct.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_set>
#include <simdjson.h>
#include <nlohmann/json.hpp>
#include "../include/loudsTotrie.h"

using namespace json2;
using nlohmann::json;

void printNodeValue(const NodeValue& v);
void printDecodedNodeValue(const NodeValue& v, const FieldKey& field_key, const FieldDictionaryManager& manager);

// Helper function to convert FieldType to string
std::string fieldTypeToString(FieldType type) {
    switch (type) {
        case FieldType::Int: return "Int";
        case FieldType::Double: return "Double";
        case FieldType::Bool: return "Bool";
        case FieldType::String: return "String";
        case FieldType::Timestamp: return "Timestamp";
        case FieldType::LogType: return "LogType";
        case FieldType::StructuredArray: return "StructuredArray";
        case FieldType::UnstructuredArray: return "UnstructuredArray";
        case FieldType::Null: return "Null";
        default: return "Unknown";
    }
}

// Helper function to print field order
void printFieldOrder(const std::vector<FieldKey>& ordered_fields) {
    std::cout << "\nField Order (by redundancy factor):\n";
    std::cout << "================================\n";
    for (size_t i = 0; i < ordered_fields.size(); ++i) {
        const auto& key = ordered_fields[i];
        std::cout << (i + 1) << ". " << key.name << "[" << fieldTypeToString(key.type) << "]\n";
    }
}

// 打印字典内容
void printDictionary(const FieldDictionaryManager& manager, const std::vector<FieldKey>& ordered_fields) {
    std::cout << "\n=== Dictionary Contents ===\n";
    const Dictionary& dict = manager.variableDict();
    for (const auto& key : ordered_fields) {
        std::cout << "\nField: " << key.name << "[" << fieldTypeToString(key.type) << "]\n";
        size_t count = dict.getFieldValueCount(key);
        for (uint32_t code = 1; code <= count; ++code) {
            auto opt_value = dict.getFieldValueByCode(key, code);
            if (opt_value) {
                std::cout << "  Code " << code << ": ";
                if (std::holds_alternative<std::string>(*opt_value)) {
                    std::cout << std::get<std::string>(*opt_value);
                } else if (std::holds_alternative<int64_t>(*opt_value)) {
                    std::cout << std::get<int64_t>(*opt_value);
                } else if (std::holds_alternative<double>(*opt_value)) {
                    std::cout << std::get<double>(*opt_value);
                } else if (std::holds_alternative<bool>(*opt_value)) {
                    std::cout << (std::get<bool>(*opt_value) ? "true" : "false");
                } else if (std::holds_alternative<std::nullptr_t>(*opt_value)) {
                    std::cout << "null";
                }
                std::cout << "\n";
            }
        }
    }
}

// 递归打印Trie树结构
void printTrieNode(const TrieNode* node, int depth, const std::vector<FieldKey>& fields, const FieldDictionaryManager& manager) {
    if (!node) {
        return;
    }
    const auto& path = node->getPath();
    std::cout << std::string(depth * 2, ' ') << "[Depth " << depth << "] ";
    if (node->isPlaceholder()) {
        std::cout << "[PLACEHOLDER] ";
    }
    if (!path.empty()) {
        std::cout << "Path: ";
        for (size_t i = 0; i < path.size(); ++i) {
            size_t field_idx = depth + i;
            if (field_idx >= fields.size()) {
                continue;
            }
            std::string field_desc = fields[field_idx].name + "[" + fieldTypeToString(fields[field_idx].type) + "]";
            const FieldKey* key_ptr = &fields[field_idx];
            std::cout << "{" << field_desc << ": ";
            const NodeValue& value = path[i];
            if (std::holds_alternative<uint32_t>(value) && key_ptr) {
                uint32_t code = std::get<uint32_t>(value);
                auto opt_val = manager.getFieldValueByCode(*key_ptr, code);
                if (opt_val) {
                    if (std::holds_alternative<std::string>(*opt_val)) {
                        std::cout << "Code " << code << " -> '" << std::get<std::string>(*opt_val) << "'";
                    } else if (std::holds_alternative<int64_t>(*opt_val)) {
                        std::cout << "Code " << code << " -> " << std::get<int64_t>(*opt_val);
                    } else if (std::holds_alternative<double>(*opt_val)) {
                        std::cout << "Code " << code << " -> " << std::get<double>(*opt_val);
                    } else if (std::holds_alternative<bool>(*opt_val)) {
                        std::cout << "Code " << code << " -> " << (std::get<bool>(*opt_val) ? "true" : "false");
                    } else if (std::holds_alternative<std::nullptr_t>(*opt_val)) {
                        std::cout << "Code " << code << " -> null";
                    }
                } else {
                    std::cout << "Code " << code << " -> DECODE_ERROR";
                }
            } else if (std::holds_alternative<int64_t>(value)) {
                std::cout << "Int: " << std::get<int64_t>(value);
            } else if (std::holds_alternative<double>(value)) {
                std::cout << "Double: " << std::get<double>(value);
            } else if (std::holds_alternative<bool>(value)) {
                std::cout << "Bool: " << (std::get<bool>(value) ? "true" : "false");
            }
            std::cout << "} ";
        }
    }
    std::cout << "\n";
    for (const auto& child : node->getChildren()) {
        printTrieNode(child.second.get(), depth + node->getPath().size(), fields, manager);
    }
}

void debugLoudsConstructionRecursive(const TrieNode* node, std::vector<bool>& bits, 
                                   std::vector<std::string>& node_info, int depth);

void printNodeTraversal(const LOUDSTrie& louds_trie) {
    std::cout << "\n=== 节点遍历测试 ===" << std::endl;
    
    for (size_t i = 0; i < std::min(louds_trie.nodeCount(), size_t(20)); ++i) {
        std::cout << "节点 " << i << ": ";
        
        // 获取节点值
        const auto& value = louds_trie.getNodeValue(i);
        std::cout << "值=";
        std::visit([](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, uint32_t>) {
                std::cout << "uint32(" << arg << ")";
            } else if constexpr (std::is_same_v<T, int64_t>) {
                std::cout << "int64(" << arg << ")";
            } else if constexpr (std::is_same_v<T, double>) {
                std::cout << "double(" << arg << ")";
            } else if constexpr (std::is_same_v<T, bool>) {
                std::cout << "bool(" << (arg ? "true" : "false") << ")";
            } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                std::cout << "null";
            } else if constexpr (std::is_same_v<T, TemplateEncodedTimestamp>) {
                std::cout << "timestamp(" << arg.template_id << ",[" << arg.var_codes.size() << "])";
            } else if constexpr (std::is_same_v<T, EncodedLog>) {
                std::cout << "log(" << arg.template_id << ",[" << arg.var_codes.size() << "])";
            }
        }, value);
        
        // 检查子节点
        if (louds_trie.hasChild(i)) {
            size_t first_child = louds_trie.firstChild(i);
            std::cout << ", 第一个子节点=" << first_child;
        } else {
            std::cout << ", 无子节点";
        }
        
        // 检查兄弟节点
        size_t next_sibling = louds_trie.nextSibling(i);
        if (next_sibling < louds_trie.nodeCount()) {
            std::cout << ", 下一个兄弟=" << next_sibling;
        }
        
        std::cout << std::endl;
    }
}

// 新增：专门的LOUDS结构测试
void testLoudsStructure(const LOUDSTrie& louds_trie, const FieldDictionaryManager& manager, const std::vector<FieldKey>& fieldOrder) {
    std::cout << "\n=== LOUDS位图结构详细分析 ===" << std::endl;
    
    const auto& louds_bv = louds_trie.getLoudsBv();
    std::cout << "LOUDS位图大小: " << louds_bv.size() << " 位" << std::endl;
    
    // 完整打印LOUDS位图
    std::cout << "\n=== 完整LOUDS位图 ===" << std::endl;
    std::cout << "位图内容: ";
    for (size_t i = 0; i < louds_bv.size(); ++i) {
        std::cout << (louds_bv[i] ? "1" : "0");
        if ((i + 1) % 10 == 0) std::cout << " "; // 每10位分组
        if ((i + 1) % 50 == 0) std::cout << "\n         "; // 每50位换行
    }
    std::cout << std::endl;
    
    // 统计1和0的数量
    size_t ones_count = 0, zeros_count = 0;
    for (size_t i = 0; i < louds_bv.size(); ++i) {
        if (louds_bv[i]) ones_count++;
        else zeros_count++;
    }
    std::cout << "\n=== 位图统计 ===" << std::endl;
    std::cout << "1的数量: " << ones_count << std::endl;
    std::cout << "0的数量: " << zeros_count << std::endl;
    std::cout << "总位数: " << louds_bv.size() << std::endl;
    std::cout << "1的比例: " << std::fixed << std::setprecision(2) 
              << (double)ones_count / louds_bv.size() * 100 << "%" << std::endl;
    
    // 分析LOUDS位图模式
    std::cout << "\n=== LOUDS位图模式分析 ===" << std::endl;
    std::vector<size_t> consecutive_ones;
    std::vector<size_t> consecutive_zeros;
    
    size_t current_ones = 0, current_zeros = 0;
    for (size_t i = 0; i < louds_bv.size(); ++i) {
        if (louds_bv[i]) {
            if (current_zeros > 0) {
                consecutive_zeros.push_back(current_zeros);
                current_zeros = 0;
            }
            current_ones++;
        } else {
            if (current_ones > 0) {
                consecutive_ones.push_back(current_ones);
                current_ones = 0;
            }
            current_zeros++;
        }
    }
    // 处理最后一段
    if (current_ones > 0) consecutive_ones.push_back(current_ones);
    if (current_zeros > 0) consecutive_zeros.push_back(current_zeros);
    
    std::cout << "连续1的段数: " << consecutive_ones.size() << std::endl;
    if (!consecutive_ones.empty()) {
        std::cout << "连续1的长度分布: ";
        for (size_t len : consecutive_ones) {
            std::cout << len << " ";
        }
        std::cout << std::endl;
    }
    
    std::cout << "连续0的段数: " << consecutive_zeros.size() << std::endl;
    if (!consecutive_zeros.empty()) {
        std::cout << "连续0的长度分布: ";
        for (size_t len : consecutive_zeros) {
            std::cout << len << " ";
        }
        std::cout << std::endl;
    }
    
    // 测试LOUDS导航功能
    std::cout << "\n=== LOUDS导航功能测试 ===" << std::endl;
    for (size_t i = 0; i < std::min(louds_trie.nodeCount(), size_t(15)); ++i) {
        std::cout << "节点 " << i << ": ";
        // select/rank调试输出
        std::cout << "select1=" << louds_trie.getLoudsSelect()(i+1)
                  << ", select0=" << louds_trie.getLoudsSelect0()(i+1)
                  << ", rank1@select1=" << louds_trie.getLoudsRank()(louds_trie.getLoudsSelect()(i+1))
                  << ", rank1@select0=" << louds_trie.getLoudsRank()(louds_trie.getLoudsSelect0()(i+1)) << "; ";
        // 测试子节点
        if (louds_trie.hasChild(i)) {
            size_t first_child = louds_trie.firstChild(i);
            std::cout << "有子节点(第一个=" << first_child << ") ";
        } else {
            std::cout << "无子节点 ";
        }
        // 测试兄弟节点
        size_t next_sibling = louds_trie.nextSibling(i);
        if (next_sibling < louds_trie.nodeCount()) {
            std::cout << "下一个兄弟=" << next_sibling;
        } else {
            std::cout << "无兄弟节点";
        }
        // 测试父节点
        size_t parent = louds_trie.parent(i);
        if (parent < louds_trie.nodeCount()) {
            std::cout << " 父节点=" << parent;
        } else {
            std::cout << " 无父节点";
        }
        std::cout << std::endl;
    }

    // 新增：打印每层的内容
    std::cout << "\n=== 分层内容详情 ===" << std::endl;
    for (size_t layer = 0; layer < louds_trie.layerCount(); ++layer) {
        const auto& l = louds_trie.getLayer(layer);
        std::cout << "第 " << layer << " 层 [" << fieldOrder[layer].name << "]: ";
        for (size_t i = 0; i < l.size(); ++i) {
            if (i > 0) std::cout << ", ";
            const auto& value = louds_trie.getLayerNodeValue(layer, i);
            printDecodedNodeValue(value, fieldOrder[layer], manager);
        }
        std::cout << std::endl;
    }
    std::cout << std::endl;

    // 新增：LOUDS导航与分层内容对应关系
    std::cout << "\n=== LOUDS导航与分层内容对应关系 ===" << std::endl;
    for (size_t i = 0; i < std::min(louds_trie.nodeCount(), size_t(30)); ++i) {
        std::cout << "节点 " << i << ": ";
        auto [layer_idx, node_idx] = louds_trie.getLayeredStorage().bfsToLayerIndex(i);
        const auto& l = louds_trie.getLayer(layer_idx);
        std::cout << "层 " << layer_idx << " [" << fieldOrder[layer_idx].name << "], 层内idx " << node_idx;
        const auto& value = louds_trie.getNodeValue(i);
        std::cout << ", 值=";
        printNodeValue(value);
        std::cout << ", hasChild=" << louds_trie.hasChild(i);
        if (louds_trie.hasChild(i)) {
            std::cout << ", firstChild=" << louds_trie.firstChild(i);
        }
        std::cout << ", nextSibling=" << louds_trie.nextSibling(i);
        std::cout << ", parent=" << louds_trie.parent(i);
        std::cout << std::endl;
    }
}

// 新增：LOUDS位图构建过程调试
void debugLoudsConstruction(const Trie& trie) {
    std::cout << "\n=== LOUDS位图构建过程调试 ===" << std::endl;
    
    // 模拟LOUDS位图构建过程
    std::vector<bool> debug_louds_bits;
    std::vector<std::string> debug_node_info;
    
    // 递归构建调试信息
    debugLoudsConstructionRecursive(trie.getRoot(), debug_louds_bits, debug_node_info, 0);
    
    std::cout << "调试位图大小: " << debug_louds_bits.size() << " 位" << std::endl;
    std::cout << "调试位图内容: ";
    for (size_t i = 0; i < debug_louds_bits.size(); ++i) {
        std::cout << (debug_louds_bits[i] ? "1" : "0");
        if ((i + 1) % 10 == 0) std::cout << " ";
        if ((i + 1) % 50 == 0) std::cout << "\n                ";
    }
    std::cout << std::endl;
    
    std::cout << "\n=== 节点信息 ===" << std::endl;
    for (size_t i = 0; i < debug_node_info.size(); ++i) {
        std::cout << "位[" << i << "]: " << debug_node_info[i] << std::endl;
    }
}

void debugLoudsConstructionRecursive(const TrieNode* node, std::vector<bool>& bits, 
                                   std::vector<std::string>& node_info, int depth) {
    const auto& children = node->getChildren();
    
    // 为当前节点的所有子节点添加标记
    for (const auto& child : children) {
        bits.push_back(1); // 有子节点
        
        // 记录节点信息
        std::string info = "1-有子节点";
        if (!child.second->getPath().empty()) {
            info += " (路径长度=" + std::to_string(child.second->getPath().size()) + ")";
        }
        node_info.push_back(info);
        
        // 递归处理子节点
        debugLoudsConstructionRecursive(child.second.get(), bits, node_info, depth + 1);
    }
    
    // 如果没有子节点，添加0标记
    if (children.empty()) {
        bits.push_back(0); // 无子节点
        node_info.push_back("0-无子节点");
    }
}

// 从LOUDS Trie还原TrieNode的辅助函数
TrieNode* restoreTrieNodeFromLOUDS(const LOUDSTrie& louds, size_t idx) {
    // 防止无限递归 - 限制递归深度
    static int depth = 0;
    if (depth > 10) {
        depth--;
        return nullptr;
    }
    depth++;
    
    // 1. 取出当前节点的值
    NodeValue value = louds.getNodeValue(idx);
    std::vector<NodeValue> path = {value};
    auto node = std::make_unique<TrieNode>(path, false);

    // 2. 递归还原所有子节点（限制只处理前几个子节点）
    if (louds.hasChild(idx)) {
        size_t child_idx = louds.firstChild(idx);
        int child_count = 0;
        while (child_idx < louds.nodeCount() && child_count < 5) { // 限制子节点数量
            TrieNode* child_node = restoreTrieNodeFromLOUDS(louds, child_idx);
            if (child_node) {
                node->getChildren()[louds.getNodeValue(child_idx)] = std::unique_ptr<TrieNode>(child_node);
            }
            size_t next = louds.nextSibling(child_idx);
            if (next == louds.nodeCount() || next == child_idx) break;
            child_idx = next;
            child_count++;
        }
    }
    
    depth--;
    return node.release();
}

// 打印NodeValue的辅助函数
void printNodeValue(const NodeValue& v) {
    std::visit([](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>) {
            std::cout << val;
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
            std::cout << "null";
        } else if constexpr (std::is_same_v<T, TemplateEncodedTimestamp>) {
            std::cout << "TemplateEncodedTimestamp{template_id=" << val.template_id << ", var_codes=[";
            for (size_t i = 0; i < val.var_codes.size(); ++i) {
                std::cout << val.var_codes[i];
                if (i + 1 < val.var_codes.size()) std::cout << ", ";
            }
            std::cout << "]}";
        } else if constexpr (std::is_same_v<T, EncodedLog>) {
            std::cout << "EncodedLog{template_id=" << val.template_id << ", var_codes=[";
            for (size_t i = 0; i < val.var_codes.size(); ++i) {
                std::cout << val.var_codes[i];
                if (i + 1 < val.var_codes.size()) std::cout << ", ";
            }
            std::cout << "]}";
        } else {
            std::cout << "[UnknownType]";
        }
    }, v);
}

// 解码并打印节点值的辅助函数
void printDecodedNodeValue(const NodeValue& v, const FieldKey& field_key, const FieldDictionaryManager& manager) {
    std::visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, uint32_t>) {
            // 尝试从字典解码
            auto opt_val = manager.getFieldValueByCode(field_key, val);
            if (opt_val) {
                if (std::holds_alternative<std::string>(*opt_val)) {
                    std::cout << "'" << std::get<std::string>(*opt_val) << "'";
                } else if (std::holds_alternative<int64_t>(*opt_val)) {
                    std::cout << std::get<int64_t>(*opt_val);
                } else if (std::holds_alternative<double>(*opt_val)) {
                    std::cout << std::get<double>(*opt_val);
                } else if (std::holds_alternative<bool>(*opt_val)) {
                    std::cout << (std::get<bool>(*opt_val) ? "true" : "false");
                } else if (std::holds_alternative<std::nullptr_t>(*opt_val)) {
                    std::cout << "null";
                }
            } else {
                std::cout << "Code_" << val;
            }
        } else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, double> || std::is_same_v<T, bool>) {
            std::cout << val;
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
            std::cout << "null";
        } else if constexpr (std::is_same_v<T, TemplateEncodedTimestamp>) {
            std::cout << "Timestamp{template_id=" << val.template_id << ", var_codes=[";
            for (size_t i = 0; i < val.var_codes.size(); ++i) {
                std::cout << val.var_codes[i];
                if (i + 1 < val.var_codes.size()) std::cout << ", ";
            }
            std::cout << "]}";
        } else if constexpr (std::is_same_v<T, EncodedLog>) {
            std::cout << "Log{template_id=" << val.template_id << ", var_codes=[";
            for (size_t i = 0; i < val.var_codes.size(); ++i) {
                std::cout << val.var_codes[i];
                if (i + 1 < val.var_codes.size()) std::cout << ", ";
            }
            std::cout << "]}";
        } else {
            std::cout << "[UnknownType]";
        }
    }, v);
}


int main() {
    const size_t CHUNK_SIZE = 1000; // 处理1000条记录每块

    try {
        // 创建字典管理器
        FieldDictionaryManager manager;
        
        // 配置时间戳字段
        std::vector<std::string> timestamp_fields = {"@timestamp", "timestamp", "session_start"};
        manager.setTimestampFields(timestamp_fields);
        manager.setStructurizeArrays(false);  // 设置为 false 启用非结构化数组处理
        
        std::vector<FieldKey> fieldOrder;
        std::unique_ptr<Trie> trie = nullptr;

        // 尝试从文件读取数据
        std::ifstream in("test_data.json"); 
        if (!in.is_open()) {
            std::cout << "Warning: Cannot open test_data.json, using synthetic test data instead." << std::endl;
            
            // 使用包含数组字段的合成测试数据
            std::vector<std::string> test_data = {
                R"({"@timestamp":"2023-03-28T04:00:00.040Z", "log.level":"TRACE", "message":"scheduling refresh every 1s", "ecs.version": "1.2.0","service.name":"ES_ECS","event.dataset":"elasticsearch.server","process.thread.name":"elasticsearch[hostb9][refresh][T#7]","log.logger":"org.elasticsearch.index.IndexService","elasticsearch.cluster.uuid":"bp-xkJD1S1iyBlj6I_JRHA","elasticsearch.node.id":"iKPGCkp9RVOKXOj20uOt4g","elasticsearch.node.name":"hostb9","elasticsearch.cluster.name":"elasticsearch","tags":[" [abacus_go]"]})",
                R"({"@timestamp":"2023-03-28T04:00:00.202Z", "log.level":"TRACE", "message":"scheduling refresh every 1s", "ecs.version": "1.2.0","service.name":"ES_ECS","event.dataset":"elasticsearch.server","process.thread.name":"elasticsearch[hostb9][refresh][T#1]","log.logger":"org.elasticsearch.index.IndexService","elasticsearch.cluster.uuid":"bp-xkJD1S1iyBlj6I_JRHA","elasticsearch.node.id":"iKPGCkp9RVOKXOj20uOt4g","elasticsearch.node.name":"hostb9","elasticsearch.cluster.name":"elasticsearch","tags":[" [rider_product_cored]"]})",
                R"({"@timestamp":"2023-03-28T04:00:00.202Z", "log.level":"TRACE", "message":"scheduling refresh every 1s", "ecs.version": "1.2.0","service.name":"ES_ECS","event.dataset":"elasticsearch.server","process.thread.name":"elasticsearch[hostb9][refresh][T#4]","log.logger":"org.elasticsearch.index.IndexService","elasticsearch.cluster.uuid":"bp-xkJD1S1iyBlj6I_JRHA","elasticsearch.node.id":"iKPGCkp9RVOKXOj20uOt4g","elasticsearch.node.name":"hostb9","elasticsearch.cluster.name":"elasticsearch","tags":[" [fares_management]"]})",
                R"({"@timestamp":"2023-03-28T04:00:00.201Z", "log.level":"TRACE", "message":"scheduling refresh every 1s", "ecs.version": "1.2.0","service.name":"ES_ECS","event.dataset":"elasticsearch.server","process.thread.name":"elasticsearch[hostb9][refresh][T#8]","log.logger":"org.elasticsearch.index.IndexService","elasticsearch.cluster.uuid":"bp-xkJD1S1iyBlj6I_JRHA","elasticsearch.node.id":"iKPGCkp9RVOKXOj20uOt4g","elasticsearch.node.name":"hostb9","elasticsearch.cluster.name":"elasticsearch","tags":[" [fulfillment_compatibled]"]})",
                R"({"@timestamp":"2023-03-28T04:00:00.201Z", "log.level":"TRACE", "message":"scheduling refresh every 1s", "ecs.version": "1.2.0","service.name":"ES_ECS","event.dataset":"elasticsearch.server","process.thread.name":"elasticsearch[hostb9][refresh][T#3]","log.logger":"org.elasticsearch.index.IndexService","elasticsearch.cluster.uuid":"bp-xkJD1S1iyBlj6I_JRHA","elasticsearch.node.id":"iKPGCkp9RVOKXOj20uOt4g","elasticsearch.node.name":"hostb9","elasticsearch.cluster.name":"elasticsearch","tags":[" [k8s_apiserver]"]})",
                R"({"@timestamp":"2023-03-28T04:00:00.202Z", "log.level":"TRACE", "message":"scheduling refresh every 1s", "ecs.version": "1.2.0","service.name":"ES_ECS","event.dataset":"elasticsearch.server","process.thread.name":"elasticsearch[hostb9][refresh][T#6]","log.logger":"org.elasticsearch.index.IndexService","elasticsearch.cluster.uuid":"bp-xkJD1S1iyBlj6I_JRHA","elasticsearch.node.id":"iKPGCkp9RVOKXOj20uOt4g","elasticsearch.node.name":"hostb9","elasticsearch.cluster.name":"elasticsearch","tags":[" [event_logs]"]})"
            };
            
            simdjson::dom::parser parser;
            
            // 分析字段并排序
            FieldAnalyzer::analyzeAndSortFields(test_data, manager, fieldOrder);
            printFieldOrder(fieldOrder);
            trie = std::make_unique<Trie>(fieldOrder);
            
            // 插入测试数据
            for (const auto& json_str : test_data) {
                trie->insert(json_str, manager, parser);
            }
            
        } else {
            // 从文件读取数据
            simdjson::dom::parser parser;
            bool isFirstChunk = true;
            size_t chunk_idx = 0;
            
            while (in) {
                std::vector<std::string> records;
                records.reserve(CHUNK_SIZE);
                std::string line;
                size_t line_count = 0;
                
                for (size_t i = 0; i < CHUNK_SIZE && std::getline(in, line); ++i) {
                    if (line.empty()) continue;
                    try {
                        simdjson::dom::element doc = parser.parse(line).value();
                        records.push_back(line);
                        ++line_count;
                    } catch (const std::exception& e) {
                        std::cerr << "[DEBUG] JSON parse error in line, skipping: " << e.what() << "\n";
                    }
                }

                if (records.empty()) {
                    break;
                }

                if (isFirstChunk) {
                    std::cout << "--- Processing First Chunk ---\n";
                    FieldAnalyzer::analyzeAndSortFields(records, manager, fieldOrder);
                    printFieldOrder(fieldOrder);
                    trie = std::make_unique<Trie>(fieldOrder);
                    isFirstChunk = false;
                }

                // 插入记录到Trie
                size_t insert_count = 0;
                for (const auto& record : records) {
                    trie->insert(record, manager, parser);
                    ++insert_count;
                }
                std::cout << "[DEBUG] 本chunk插入Trie完成, 共 " << insert_count << " 条" << std::endl;
                ++chunk_idx;
            }
            in.close();
        }

        if (!trie) {
            std::cout << "No records processed, exiting." << std::endl;
            return 0;
        }

        // std::cout << "\n=== 原始Trie树结构 ===" << std::endl;
        // printTrieNode(trie->getRoot(), 0, fieldOrder, manager);

        // 构建LOUDS Trie
        std::cout << "\n[DEBUG] Building LOUDSTrie from Trie..." << std::endl;
        LOUDSTrie louds_trie;
        size_t root_children = louds_trie.buildFromTrie(*trie);
        std::cout << "LOUDS Trie构建完成，根节点子节点数: " << root_children << std::endl;

        // === 新增：LOUDS位图和层内容序列化/反序列化测试 ===
        {
            std::ofstream bv_out("louds_bitmap.bin", std::ios::binary);
            std::ofstream layers_out("louds_layers.bin", std::ios::binary);
            louds_trie.serializeBitmap(bv_out);
            louds_trie.getLayeredStorage().serialize(layers_out);
            bv_out.close();
            layers_out.close();
        }
        // 反序列化到新对象
        LOUDSTrie loaded_trie;
        {
            std::ifstream bv_in("louds_bitmap.bin", std::ios::binary);
            std::ifstream layers_in("louds_layers.bin", std::ios::binary);
            loaded_trie.deserializeBitmap(bv_in);
            loaded_trie.getLayeredStorage().deserialize(layers_in);
            bv_in.close();
            layers_in.close();
        }
        // 输出对比
        std::cout << "\n[TEST] LOUDS位图序列化/反序列化对比：" << std::endl;
        const auto& orig_bv = louds_trie.getLoudsBv();
        const auto& loaded_bv = loaded_trie.getLoudsBv();
        std::cout << "原始位图:  ";
        for (size_t i = 0; i < std::min(orig_bv.size(), size_t(50)); ++i) std::cout << (orig_bv[i] ? "1" : "0");
        std::cout << std::endl;
        std::cout << "反序列化: ";
        for (size_t i = 0; i < std::min(loaded_bv.size(), size_t(50)); ++i) std::cout << (loaded_bv[i] ? "1" : "0");
        std::cout << std::endl;
        std::cout << "长度一致: " << (orig_bv.size() == loaded_bv.size() ? "✅" : "❌") << std::endl;
        bool bv_equal = (orig_bv.size() == loaded_bv.size());
        for (size_t i = 0; bv_equal && i < orig_bv.size(); ++i) {
            if (orig_bv[i] != loaded_bv[i]) bv_equal = false;
        }
        std::cout << "内容一致: " << (bv_equal ? "✅" : "❌") << std::endl;
        // 层内容对比
        std::cout << "\n[TEST] LOUDS每层内容序列化/反序列化对比：" << std::endl;
        bool all_layers_equal = true;
        for (size_t i = 0; i < louds_trie.layerCount(); ++i) {
            const auto& orig_layer = louds_trie.getLayer(i);
            const auto& loaded_layer = loaded_trie.getLayer(i);
            std::cout << "层 " << i << ": 节点数(" << orig_layer.size() << ", " << loaded_layer.size() << ")";
            bool layer_equal = (orig_layer.size() == loaded_layer.size());
            for (size_t j = 0; layer_equal && j < orig_layer.size(); ++j) {
                if (orig_layer[j] != loaded_layer[j]) layer_equal = false;
            }
            std::cout << (layer_equal ? " ✅" : " ❌") << std::endl;
            if (!layer_equal) all_layers_equal = false;
        }
        std::cout << "所有层内容一致: " << (all_layers_equal ? "✅" : "❌") << std::endl;
        // === END ===

        // 新增：详细LOUDS导航调试输出
        // std::cout << "\n=== LOUDS节点导航详细调试 ===" << std::endl;
        // for (size_t i = 0; i < louds_trie.nodeCount(); ++i) {
        //     std::cout << "节点" << i
        //               << ": select1=" << louds_trie.getLoudsSelect()(i+1)
        //               << ", select0=" << louds_trie.getLoudsSelect0()(i+1)
        //               << ", rank1@select1=" << louds_trie.getLoudsRank()(louds_trie.getLoudsSelect()(i+1))
        //               << ", rank1@select0=" << louds_trie.getLoudsRank()(louds_trie.getLoudsSelect0()(i+1))
        //               << ", parent=" << louds_trie.parent(i)
        //               << ", firstChild=" << louds_trie.firstChild(i)
        //               << ", nextSibling=" << louds_trie.nextSibling(i)
        //               << std::endl;
        // }
        
        // LOUDS->Trie 还原测试
        std::cout << "\n=== LOUDS还原Trie树结构 ===" << std::endl;
        Trie restored_trie(fieldOrder);
        loudsToTrie(louds_trie, restored_trie);
        // printTrieNode(restored_trie.getRoot(), 0, fieldOrder, manager);

        // 新增：LOUDS还原文档输出
        std::string reconstructed_json = reconstructJsonFromTrie(restored_trie, manager);
        // 写入文件
        std::ofstream out_file("reconstructed.json");
        if (out_file.is_open()) {
            out_file << reconstructed_json;
            out_file.close();
            std::cout << "\n[INFO] LOUDS还原文档已写入 reconstructed.json\n";
        } else {
            std::cerr << "[ERROR] 无法写入 reconstructed.json 文件！\n";
        }
        
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 