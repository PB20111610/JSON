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

// 新增：打印带BFS索引的LOUDS Trie结构
void printLoudsTrieWithBFSIndex(const LOUDSTrie& louds_trie, const std::vector<FieldKey>& fieldOrder) {
    std::cout << "\n=== LOUDS Trie结构 (带BFS索引) ===" << std::endl;
    
    size_t total_nodes = louds_trie.nodeCount();
    if (total_nodes == 0) {
        std::cout << "空树" << std::endl;
        return;
    }
    
    // 创建一个映射来跟踪BFS索引
    std::vector<std::string> node_labels(total_nodes);
    for (size_t i = 0; i < total_nodes; ++i) {
        const auto& value = louds_trie.getNodeValue(i);
        std::ostringstream label;
        std::visit([&label](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, uint32_t>) {
                label << "Code:" << arg;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                label << "Int:" << arg;
            } else if constexpr (std::is_same_v<T, double>) {
                label << "Double:" << arg;
            } else if constexpr (std::is_same_v<T, bool>) {
                label << "Bool:" << (arg ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                label << "null";
            } else if constexpr (std::is_same_v<T, TemplateEncodedTimestamp>) {
                label << "TS:" << arg.template_id;
            } else if constexpr (std::is_same_v<T, EncodedLog>) {
                label << "Log:" << arg.template_id;
            }
        }, value);
        node_labels[i] = label.str();
    }
    
    // 使用递归方式打印树结构
    std::function<void(size_t, int, std::string, bool)> printNode = 
        [&](size_t node_idx, int depth, std::string prefix, bool isLast) {
            if (node_idx >= total_nodes) {
                return;
            }
            
            // 打印当前节点
            std::cout << prefix;
            if (depth > 0) {
                std::cout << (isLast ? "└── " : "├── ");
            }
            std::cout << "BFS索引 " << node_idx << ": " << node_labels[node_idx];
            if (depth < fieldOrder.size()) {
                std::cout << " [" << fieldOrder[depth].name << "]";
            }
            std::cout << std::endl;
            
            // 生成子节点前缀
            std::string child_prefix = prefix;
            if (depth > 0) {
                child_prefix += (isLast ? "    " : "│   ");
            }
            
            // 获取子节点并打印
            if (louds_trie.hasChild(node_idx)) {
                size_t child = louds_trie.firstChild(node_idx);
                std::vector<size_t> children_list;
                
                // 收集所有直接子节点
                int child_count = 0;
                size_t current_child = child;
                while (current_child < total_nodes && child_count < 20) {
                    // 验证这确实是当前节点的子节点
                    if (louds_trie.parent(current_child) == node_idx) {
                        children_list.push_back(current_child);
                    }
                    size_t next = louds_trie.nextSibling(current_child);
                    if (next >= total_nodes || next == current_child) {
                        break;
                    }
                    current_child = next;
                    child_count++;
                }
                
                // 打印所有子节点
                for (size_t i = 0; i < children_list.size(); ++i) {
                    bool isLastChild = (i == children_list.size() - 1);
                    printNode(children_list[i], depth + 1, child_prefix, isLastChild);
                }
            }
        };
    
    // 从根节点开始打印
    printNode(0, 0, "", true);
}

void debugLoudsConstructionRecursive(const TrieNode* node, std::vector<bool>& bits, 
                                   std::vector<std::string>& node_info, int depth);

void printNodeTraversal(const LOUDSTrie& louds_trie) {
    std::cout << "\n=== 节点遍历测试 ===" << std::endl;
    
    for (size_t i = 0; i < std::min(louds_trie.nodeCount(), size_t(20)); ++i) {
        std::cout << "节点 " << i << ": ";
        
        // 获取节点值
        const auto& value = louds_trie.getNodeValue(i);
        std::cout << "값=";
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
        std::cout << "select1=" << louds_trie.getLoudsSelect1()(i+1)
                  << ", select0=" << louds_trie.getLoudsSelect0()(i+1)
                  << ", rank1@select1=" << louds_trie.getLoudsRank1()(louds_trie.getLoudsSelect1()(i+1))
                  << ", rank1@select0=" << louds_trie.getLoudsRank0()(louds_trie.getLoudsSelect0()(i+1)) << "; ";
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

void printPathsFromIntermediateNode(const LOUDSTrie& louds, 
                                    size_t bfs_idx,
                                    const FieldDictionaryManager& dict_manager) {
    std::cout << "\n=== 从中间节点 " << bfs_idx << " 重建路径集合 ===" << std::endl;

    // 获取字段顺序
    const auto& field_order = louds.getFieldOrder();
    std::cout << "字段顺序: ";
    for (size_t i = 0; i < field_order.size(); ++i) {
    std::cout << field_order[i].name;
    if (i < field_order.size() - 1) std::cout << " -> ";
    }
    std::cout << std::endl;

    // 重建并解码路径
    auto decoded_paths = reconstructAndDecodePathsFromIntermediateNode(louds, bfs_idx, dict_manager);

    std::cout << "找到 " << decoded_paths.size() << " 条路径:" << std::endl;
    for (size_t i = 0; i < decoded_paths.size(); ++i) {
    std::cout << "路径 " << i + 1 << ": ";
    for (size_t j = 0; j < decoded_paths[i].size(); ++j) {
    std::cout << decoded_paths[i][j];
    if (j < decoded_paths[i].size() - 1) std::cout << " -> ";
    }
    std::cout << std::endl;
}
}

// 新增：LOUDS导航函数测试
void testLoudsNavigation(const LOUDSTrie& louds_trie) {
    std::cout << "\n=== LOUDS导航函数测试 ===" << std::endl;
    
    size_t total_nodes = louds_trie.nodeCount();
    std::cout << "总节点数: " << total_nodes << std::endl;
    
    // 打印LOUDS位图
    const auto& louds_bv = louds_trie.getLoudsBv();
    std::cout << "LOUDS位图: ";
    for (size_t i = 0; i < std::min(louds_bv.size(), size_t(30)); ++i) {
        std::cout << (louds_bv[i] ? "1" : "0");
    }
    if (louds_bv.size() > 30) std::cout << "...";
    std::cout << std::endl;
    
    // 测试所有节点的导航功能
    for (size_t i = 0; i < std::min(total_nodes, size_t(100)); ++i) {
        std::cout << "\n节点 " << i << ":" << std::endl;
        
        // 显示位图位置信息
        if (i + 1 <= louds_bv.size()) {
            size_t pos = louds_trie.getLoudsSelect0()(i + 1);
            std::cout << "  位图位置: select0(" << (i+1) << ")=" << pos << std::endl;
            if (pos + 1 < louds_bv.size()) {
                std::cout << "  下一位: " << (louds_bv[pos+1] ? "1" : "0") << std::endl;
            }
        }
        
        // 测试hasChild
        bool has_child = louds_trie.hasChild(i);
        std::cout << "  hasChild: " << (has_child ? "true" : "false") << std::endl;
        
        // 测试firstChild
        if (has_child) {
            size_t first_child = louds_trie.firstChild(i);
            std::cout << "  firstChild: " << first_child << std::endl;
        } else {
            std::cout << "  firstChild: N/A (无子节点)" << std::endl;
        }
        
        // 测试nextSibling
        size_t next_sibling = louds_trie.nextSibling(i);
        if (next_sibling < total_nodes) {
            std::cout << "  nextSibling: " << next_sibling << std::endl;
        } else {
            std::cout << "  nextSibling: N/A (无兄弟节点)" << std::endl;
        }
        
        // 测试parent (除了根节点)
        if (i > 0) {
            size_t parent = louds_trie.parent(i);
            if (parent < total_nodes) {
                std::cout << "  parent: " << parent << std::endl;
            } else {
                std::cout << "  parent: N/A (无父节点)" << std::endl;
            }
        } else {
            std::cout << "  parent: N/A (根节点)" << std::endl;
        }
        
        // 验证导航函数的一致性
        if (i > 0) {  // 非根节点
            size_t parent = louds_trie.parent(i);
            if (parent < total_nodes) {
                // 检查父节点是否有子节点
                if (louds_trie.hasChild(parent)) {
                    // 检查当前节点是否在父节点的子节点链中
                    size_t first_child_of_parent = louds_trie.firstChild(parent);
                    bool found_in_sibling_chain = false;
                    size_t sibling = first_child_of_parent;
                    int sibling_count = 0;
                    while (sibling < total_nodes && sibling_count < 20) { // 防止无限循环
                        if (sibling == i) {
                            found_in_sibling_chain = true;
                            break;
                        }
                        size_t next = louds_trie.nextSibling(sibling);
                        if (next >= total_nodes || next == sibling) {
                            break;
                        }
                        sibling = next;
                        sibling_count++;
                    }
                    std::cout << "  一致性检查: " << (found_in_sibling_chain ? "✅ 通过" : "❌ 失败") << std::endl;
                } else {
                    std::cout << "  一致性检查: ❌ 失败 (父节点无子节点)" << std::endl;
                }
            } else {
                std::cout << "  一致性检查: ❌ 失败 (无效父节点)" << std::endl;
            }
        }
    }
    
    // 测试边界情况
    std::cout << "\n=== 边界情况测试 ===" << std::endl;
    
    // 测试无效索引
    size_t invalid_idx = total_nodes + 100;
    std::cout << "无效索引 " << invalid_idx << " 测试:" << std::endl;
    std::cout << "  hasChild: " << (louds_trie.hasChild(invalid_idx) ? "true" : "false") << std::endl;
    std::cout << "  firstChild: " << louds_trie.firstChild(invalid_idx) << std::endl;
    std::cout << "  nextSibling: " << louds_trie.nextSibling(invalid_idx) << std::endl;
    std::cout << "  parent: " << louds_trie.parent(invalid_idx) << std::endl;
    
    // 测试根节点
    std::cout << "根节点 0 测试:" << std::endl;
    std::cout << "  hasChild: " << (louds_trie.hasChild(0) ? "true" : "false") << std::endl;
    std::cout << "  firstChild: " << louds_trie.firstChild(0) << std::endl;
    std::cout << "  nextSibling: " << louds_trie.nextSibling(0) << std::endl;
    std::cout << "  parent: " << louds_trie.parent(0) << std::endl;
    
    // 验证兄弟节点链
    std::cout << "\n=== 兄弟节点链验证 ===" << std::endl;
    for (size_t i = 0; i < std::min(total_nodes, size_t(3)); ++i) {
        if (louds_trie.hasChild(i)) {
            std::cout << "节点 " << i << " 的子节点链: ";
            size_t child = louds_trie.firstChild(i);
            int count = 0;
            while (child < total_nodes && count < 10) { // 限制遍历数量
                std::cout << child;
                size_t next = louds_trie.nextSibling(child);
                if (next < total_nodes && next != child) {
                    std::cout << " -> ";
                    child = next;
                } else {
                    std::cout << " (结束)";
                    break;
                }
                count++;
            }
            std::cout << std::endl;
        }
    }
}

// 新增：LOUDS结构验证测试
void testLoudsStructureValidation(const LOUDSTrie& louds_trie) {
    std::cout << "\n=== LOUDS结构验证测试 ===" << std::endl;
    
    const auto& louds_bv = louds_trie.getLoudsBv();
    std::cout << "LOUDS位图大小: " << louds_bv.size() << " 位" << std::endl;
    
    // 打印完整的LOUDS位图
    std::cout << "LOUDS位图内容: ";
    for (size_t i = 0; i < louds_bv.size(); ++i) {
        std::cout << (louds_bv[i] ? "1" : "0");
    }
    std::cout << std::endl;
    
    // 验证LOUDS位图结构
    size_t zeros_count = 0, ones_count = 0;
    for (size_t i = 0; i < louds_bv.size(); ++i) {
        if (louds_bv[i]) {
            ones_count++;
        } else {
            zeros_count++;
        }
    }
    std::cout << "0的数量: " << zeros_count << ", 1的数量: " << ones_count << std::endl;
    std::cout << "节点数(通过rank计算): " << louds_trie.nodeCount() << std::endl;
    std::cout << "节点数(通过1的数量): " << ones_count << std::endl;
    std::cout << "一致性检查: " << (louds_trie.nodeCount() == ones_count ? "✅ 通过" : "❌ 失败") << std::endl;
    
    // 验证select和rank函数
    std::cout << "\nSelect/Rank函数验证:" << std::endl;
    for (size_t i = 1; i <= std::min(zeros_count, size_t(5)); ++i) {
        size_t pos0 = louds_trie.getLoudsSelect0()(i);  // 第i个0的位置
        size_t rank1 = louds_trie.getLoudsRank1()(pos0); // pos0位置前1的数量
        size_t rank0 = louds_trie.getLoudsRank0()(pos0); // pos0位置前0的数量
        std::cout << "  select0(" << i << ")=" << pos0 
                  << ", rank1(" << pos0 << ")=" << rank1
                  << ", rank0(" << pos0 << ")=" << rank0 << std::endl;
    }
    
    for (size_t i = 1; i <= std::min(ones_count, size_t(5)); ++i) {
        size_t pos1 = louds_trie.getLoudsSelect1()(i);  // 第i个1的位置
        size_t rank1 = louds_trie.getLoudsRank1()(pos1); // pos1位置前1的数量
        size_t rank0 = louds_trie.getLoudsRank0()(pos1); // pos1位置前0的数量
        std::cout << "  select1(" << i << ")=" << pos1 
                  << ", rank1(" << pos1 << ")=" << rank1
                  << ", rank0(" << pos1 << ")=" << rank0 << std::endl;
    }
    
    // 验证LOUDS结构的正确性
    std::cout << "\nLOUDS结构验证:" << std::endl;
    std::cout << "  位图首尾: " << (louds_bv[0] ? "1" : "0") << "..." << (louds_bv[louds_bv.size()-1] ? "1" : "0") << std::endl;
    std::cout << "  首位应该是1: " << (louds_bv[0] ? "✅ 通过" : "❌ 失败") << std::endl;
    std::cout << "  末位应该是0: " << (!louds_bv[louds_bv.size()-1] ? "✅ 通过" : "❌ 失败") << std::endl;
}

// 新增：打印LOUDS Trie结构（按BFS索引）
void printLoudsTrieStructure(const LOUDSTrie& louds_trie, const std::vector<FieldKey>& fieldOrder, const FieldDictionaryManager& manager) {
    std::cout << "\n=== LOUDS Trie结构 (BFS索引) ===" << std::endl;
    
    size_t total_nodes = louds_trie.nodeCount();
    if (total_nodes == 0) {
        std::cout << "空树" << std::endl;
        return;
    }
    
    // 使用队列进行BFS遍历
    std::queue<std::pair<size_t, int>> q; // {bfs_index, depth}
    q.push({0, 0});
    
    std::vector<bool> visited(total_nodes, false);
    
    while (!q.empty()) {
        auto [node_idx, depth] = q.front();
        q.pop();
        
        if (node_idx >= total_nodes || visited[node_idx]) {
            continue;
        }
        
        visited[node_idx] = true;
        
        // 打印节点信息
        std::cout << std::string(depth * 2, ' ') << "Node " << node_idx << ": ";
        
        // 获取节点值
        const auto& value = louds_trie.getNodeValue(node_idx);
        std::cout << "Value=";
        printNodeValue(value);
        
        // 获取字段信息（如果可用）
        if (depth < fieldOrder.size()) {
            std::cout << " [Field: " << fieldOrder[depth].name << "]";
        }
        
        std::cout << std::endl;
        
        // 添加子节点到队列
        if (louds_trie.hasChild(node_idx)) {
            size_t child = louds_trie.firstChild(node_idx);
            int child_count = 0;
            while (child < total_nodes && child_count < 20) { // 限制遍历数量防止无限循环
                // 检查是否是当前节点的直接子节点（通过parent验证）
                if (louds_trie.parent(child) == node_idx) {
                    q.push({child, depth + 1});
                }
                
                // 移动到下一个兄弟节点
                size_t next = louds_trie.nextSibling(child);
                if (next >= total_nodes || next == child) {
                    break;
                }
                child = next;
                child_count++;
            }
        }
    }
    
    // 打印未访问的节点（可能是森林结构）
    bool has_unvisited = false;
    for (size_t i = 0; i < total_nodes; ++i) {
        if (!visited[i]) {
            if (!has_unvisited) {
                std::cout << "\n未连接的节点:" << std::endl;
                has_unvisited = true;
            }
            std::cout << "Node " << i << ": ";
            const auto& value = louds_trie.getNodeValue(i);
            std::cout << "Value=";
            printNodeValue(value);
            std::cout << std::endl;
        }
    }
}

// 新增：测试从中间节点重建路径的函数 - 算法优化版本
void testReconstructPathsFromIntermediateNode(const LOUDSTrie& louds_trie, const FieldDictionaryManager& dict_manager) {
    std::cout << "\n=== 从中间节点重建路径测试 ===" << std::endl;
    
    size_t total_nodes = louds_trie.nodeCount();
    if (total_nodes == 0) {
        std::cout << "空树，无法测试" << std::endl;
        return;
    }
    
    // 测试几个不同的中间节点
    std::vector<size_t> test_nodes = {0, 1, 2, 9, 17};
    for (size_t test_bfs_idx : test_nodes) {
        if (test_bfs_idx < total_nodes) {
            std::cout << "\n--- 测试中间节点 " << test_bfs_idx << " ---" << std::endl;
            
            // 显示节点信息
            std::cout << "节点 " << test_bfs_idx << " 信息:" << std::endl;
            std::cout << "  hasChild: " << (louds_trie.hasChild(test_bfs_idx) ? "true" : "false") << std::endl;
            if (louds_trie.hasChild(test_bfs_idx)) {
                std::cout << "  firstChild: " << louds_trie.firstChild(test_bfs_idx) << std::endl;
            }
            if (test_bfs_idx > 0) {
                std::cout << "  parent: " << louds_trie.parent(test_bfs_idx) << std::endl;
            }
            
            // 也显示BFS索引路径
            auto bfs_paths = louds_trie.reconstructPathsFromIntermediateNode(test_bfs_idx);
            std::cout << "BFS索引路径数量: " << bfs_paths.size() << std::endl;
            
            // 算法优化：批量解码所有路径
            if (!bfs_paths.empty()) {
                // 获取字段顺序（只需获取一次）
                const auto& field_order = louds_trie.getFieldOrder();
                
                // 算法优化：批量解码，减少重复计算
                for (size_t i = 0; i < std::min(bfs_paths.size(), size_t(3)); ++i) {
                    const auto& path = bfs_paths[i];
                    std::vector<std::string> decoded_values;
                    decoded_values.reserve(path.size());
                    
                    // 算法优化：缓存常用的值
                    const auto& layered_storage = louds_trie.getLayeredStorage();
                    
                    for (size_t k = 0; k < std::min(path.size(), field_order.size()); ++k) {
                        try {
                            const FieldKey& field_key = field_order[k];
                            
                            // 根据BFS索引计算层和偏移量
                            auto [layer_idx, node_idx] = layered_storage.bfsToLayerIndex(path[k]);
                            
                            // 获取该层的值
                            const NodeValue& node_value = louds_trie.getLayerNodeValue(layer_idx, node_idx);
                            
                            // 解码当前字段的值
                            std::string decoded_value = decodeNodeValueToFieldValue(
                                field_key, node_value, dict_manager);
                            decoded_values.push_back(std::move(decoded_value));
                        } catch (const std::exception& e) {
                            std::cerr << "Error decoding field " << field_order[k].name 
                                      << " at position " << k << ": " << e.what() << std::endl;
                            decoded_values.push_back("ERROR");
                        }
                    }
                    
                    std::cout << "  解码路径 " << (i + 1) << ": ";
                    for (size_t k = 0; k < decoded_values.size(); ++k) {
                        if (k > 0) std::cout << " -> ";
                        std::cout << decoded_values[k];
                    }
                    std::cout << std::endl;
                }
                
                if (bfs_paths.size() > 3) {
                    std::cout << "  ... 还有 " << (bfs_paths.size() - 3) << " 条路径" << std::endl;
                }
            }
        }
    }
    
    // 测试边界情况
    std::cout << "\n--- 边界情况测试 ---" << std::endl;
    
    // 测试无效索引
    auto invalid_paths = reconstructAndDecodePathsFromIntermediateNode(louds_trie, total_nodes + 100, dict_manager);
    std::cout << "无效索引测试 - 路径数量: " << invalid_paths.size() << " (应该为0)" << std::endl;
    
    // 测试叶子节点
    for (size_t i = 0; i < std::min(total_nodes, size_t(5)); ++i) {
        if (!louds_trie.hasChild(i)) {
            std::cout << "叶子节点 " << i << " 测试:" << std::endl;
            auto leaf_paths = reconstructAndDecodePathsFromIntermediateNode(louds_trie, i, dict_manager);
            std::cout << "  路径数量: " << leaf_paths.size() << " (应该≥1)" << std::endl;
            if (!leaf_paths.empty()) {
                std::cout << "  第一条路径长度: " << leaf_paths[0].size() << std::endl;
            }
            break;
        }
    }
    
    std::cout << "\n=== 中间节点路径重建测试完成 ===" << std::endl;
}

// 新增：测试BFS索引和层索引转换的函数
void testBFSAndLayerIndexConversion(const LOUDSTrie& louds_trie) {
    std::cout << "\n=== BFS索引和层索引转换测试 ===" << std::endl;
    
    size_t total_nodes = louds_trie.nodeCount();
    if (total_nodes == 0) {
        std::cout << "空树，无法测试" << std::endl;
        return;
    }
    
    // 测试LayeredNodeStorage的bfsToLayerIndex和layerIndexToBFS方法
    const auto& layered_storage = louds_trie.getLayeredStorage();
    for (size_t bfs_idx = 0; bfs_idx < std::min(total_nodes, size_t(10)); ++bfs_idx) {
        // BFS索引 -> 层索引
        auto [layer_idx, node_idx_in_layer] = layered_storage.bfsToLayerIndex(bfs_idx);
        
        // 层索引 -> BFS索引
        size_t converted_bfs_idx = layered_storage.layerIndexToBFS(layer_idx, node_idx_in_layer);
        
        bool is_correct = (bfs_idx == converted_bfs_idx);
        std::cout << "  BFS索引 " << bfs_idx 
                  << " -> 层 " << layer_idx 
                  << ", 层内索引 " << node_idx_in_layer
                  << " -> 转换回BFS索引 " << converted_bfs_idx
                  << " -> 转换验证 " << (is_correct ? "✅" : "❌") << std::endl;
    }
    
    std::cout << "\n=== BFS索引和层索引转换测试完成 ===" << std::endl;
}

// 新增：将重建的路径写入JSON文件 - 算法优化版本
void writeReconstructedPathsToJson(const LOUDSTrie& louds_trie, const FieldDictionaryManager& dict_manager, const std::string& filename) {
    std::cout << "\n=== 将重建路径写入JSON文件 ===" << std::endl;
    
    std::ofstream out_file(filename);
    if (!out_file.is_open()) {
        std::cerr << "无法打开文件 " << filename << " 进行写入!" << std::endl;
        return;
    }
    
    out_file << "[\n";
    
    size_t total_nodes = louds_trie.nodeCount();
    bool first_record = true;
    
    // 算法优化：批量处理所有节点的路径重建，减少重复计算
    std::vector<std::vector<std::string>> all_decoded_paths;
    all_decoded_paths.reserve(total_nodes * 2); // 估算容量
    
    // 算法优化：获取字段顺序一次
    const auto& field_order = louds_trie.getFieldOrder();
    
    // 算法优化：缓存常用的值
    const auto& layered_storage = louds_trie.getLayeredStorage();
    
    // 批量处理所有节点
    for (size_t i = 0; i < total_nodes; ++i) {
        // 从LOUDS重建所有路径（BFS索引路径）
        auto bfs_paths = louds_trie.reconstructPathsFromIntermediateNode(i);
        
        // 批量解码所有路径
        for (const auto& bfs_path : bfs_paths) {
            std::vector<std::string> decoded_path;
            decoded_path.reserve(bfs_path.size());
            
            for (size_t j = 0; j < std::min(bfs_path.size(), field_order.size()); ++j) {
                try {
                    const FieldKey& field_key = field_order[j];
                    
                    // 根据BFS索引计算层和偏移量
                    auto [layer_idx, node_idx] = layered_storage.bfsToLayerIndex(bfs_path[j]);
                    
                    // 获取该层的值
                    const NodeValue& node_value = louds_trie.getLayerNodeValue(layer_idx, node_idx);
                    
                    // 解码当前字段的值
                    std::string decoded_value = decodeNodeValueToFieldValue(
                        field_key, node_value, dict_manager);
                    decoded_path.push_back(std::move(decoded_value));
                } catch (const std::exception& e) {
                    std::cerr << "Error decoding field " << field_order[j].name 
                              << " at position " << j << ": " << e.what() << std::endl;
                    decoded_path.push_back("ERROR");
                }
            }
            
            // 算法优化：使用move避免拷贝
            all_decoded_paths.emplace_back(std::move(decoded_path));
        }
    }
    
    // 为所有解码路径创建JSON对象
    for (const auto& path : all_decoded_paths) {
        if (!first_record) {
            out_file << ",\n";
        }
        
        out_file << "  {\n";
        
        // 为路径中的每个值创建字段
        for (size_t j = 0; j < std::min(path.size(), field_order.size()); ++j) {
            if (j > 0) {
                out_file << ",\n";
            }
            
            out_file << "    \"" << field_order[j].name << "\": ";
            
            // 根据字段类型决定如何格式化值
            switch (field_order[j].type) {
                case FieldType::String:
                case FieldType::LogType:
                case FieldType::Timestamp:
                case FieldType::UnstructuredArray:
                    out_file << "\"" << path[j] << "\"";
                    break;
                case FieldType::Int:
                case FieldType::Double:
                    // 尝试解析为数字，如果失败则作为字符串
                    try {
                        // 简化处理，直接输出值
                        out_file << path[j];
                    } catch (...) {
                        out_file << "\"" << path[j] << "\"";
                    }
                    break;
                case FieldType::Bool:
                    out_file << (path[j] == "true" ? "true" : "false");
                    break;
                default:
                    out_file << "\"" << path[j] << "\"";
                    break;
            }
        }
        
        out_file << "\n  }";
        first_record = false;
    }
    
    out_file << "\n]\n";
    out_file.close();
    
    std::cout << "重建的路径已写入 " << filename << std::endl;
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

        // LOUDS结构验证测试
        // testLoudsStructureValidation(louds_trie);
        
        // LOUDS导航函数测试
        // testLoudsNavigation(louds_trie);
        
        // 测试从中间节点重建路径
        testReconstructPathsFromIntermediateNode(louds_trie, manager);
         
        // 将重建的路径写入JSON文件
        // writeReconstructedPathsToJson(louds_trie, manager, "trie_reconstruct.json");
        
        // 测试BFS索引和层索引转换
        // testBFSAndLayerIndexConversion(louds_trie);
    
        // 打印带BFS索引的LOUDS Trie结构
        printLoudsTrieWithBFSIndex(louds_trie, fieldOrder);
        
        // === LOUDS位图和层内容序列化/反序列化测试 ===
        // {
        //     std::ofstream bv_out("louds_bitmap.bin", std::ios::binary);
        //     std::ofstream layers_out("louds_layers.bin", std::ios::binary);
        //     louds_trie.serializeBitmap(bv_out);
        //     louds_trie.getLayeredStorage().serialize(layers_out);
        //     bv_out.close();
        //     layers_out.close();
        // }
        // // 反序列化到新对象
        // LOUDSTrie loaded_trie;
        // {
        //     std::ifstream bv_in("louds_bitmap.bin", std::ios::binary);
        //     std::ifstream layers_in("louds_layers.bin", std::ios::binary);
        //     loaded_trie.deserializeBitmap(bv_in);
        //     loaded_trie.getLayeredStorage().deserialize(layers_in);
        //     bv_in.close();
        //     layers_in.close();
        // }
        // // 输出对比
        // std::cout << "\n[TEST] LOUDS位图序列化/反序列化对比：" << std::endl;
        // const auto& orig_bv = louds_trie.getLoudsBv();
        // const auto& loaded_bv = loaded_trie.getLoudsBv();
        // std::cout << "原始位图:  ";
        // for (size_t i = 0; i < std::min(orig_bv.size(), size_t(50)); ++i) std::cout << (orig_bv[i] ? "1" : "0");
        // std::cout << std::endl;
        // std::cout << "反序列化: ";
        // for (size_t i = 0; i < std::min(loaded_bv.size(), size_t(50)); ++i) std::cout << (loaded_bv[i] ? "1" : "0");
        // std::cout << std::endl;
        // std::cout << "长度一致: " << (orig_bv.size() == loaded_bv.size() ? "✅" : "❌") << std::endl;
        // bool bv_equal = (orig_bv.size() == loaded_bv.size());
        // for (size_t i = 0; bv_equal && i < orig_bv.size(); ++i) {
        //     if (orig_bv[i] != loaded_bv[i]) bv_equal = false;
        // }
        // std::cout << "内容一致: " << (bv_equal ? "✅" : "❌") << std::endl;
        // // 层内容对比
        // std::cout << "\n[TEST] LOUDS每层内容序列化/反序列化对比：" << std::endl;
        // bool all_layers_equal = true;
        // for (size_t i = 0; i < louds_trie.layerCount(); ++i) {
        //     const auto& orig_layer = louds_trie.getLayer(i);
        //     const auto& loaded_layer = loaded_trie.getLayer(i);
        //     std::cout << "层 " << i << ": 节点数(" << orig_layer.size() << ", " << loaded_layer.size() << ")";
        //     bool layer_equal = (orig_layer.size() == loaded_layer.size());
        //     for (size_t j = 0; layer_equal && j < orig_layer.size(); ++j) {
        //         if (orig_layer[j] != loaded_layer[j]) layer_equal = false;
        //     }
        //     std::cout << (layer_equal ? " ✅" : " ❌") << std::endl;
        //     if (!layer_equal) all_layers_equal = false;
        // }
        // std::cout << "所有层内容一致: " << (all_layers_equal ? "✅" : "❌") << std::endl;
        // === END ===

        // 详细LOUDS导航调试输出
        // std::cout << "\n=== LOUDS节点导航详细调试 ===" << std::endl;
        // for (size_t i = 0; i < louds_trie.nodeCount(); ++i) {
        //     std::cout << "节点" << i
        //               << ": select1=" << louds_trie.getLoudsSelect()(i+1)
        //               << ", select0=" << louds_trie.getLoudsSelect0()(i+1)
        //               << ", rank1@select1=" << louds_trie.getLoudsRank1()(louds_trie.getLoudsSelect1()(i+1))
        //               << ", rank1@select0=" << louds_trie.getLoudsRank1()(louds_trie.getLoudsSelect0()(i+1))
        //               << ", parent=" << louds_trie.parent(i)
        //               << ", firstChild=" << louds_trie.firstChild(i)
        //               << ", nextSibling=" << louds_trie.nextSibling(i)
        //               << std::endl;
        // }
        
        // LOUDS->Trie 还原测试
        // std::cout << "\n=== LOUDS还原Trie树结构 ===" << std::endl;
        // Trie restored_trie(fieldOrder);
        // loudsToTrie(louds_trie, restored_trie);
        // printTrieNode(restored_trie.getRoot(), 0, fieldOrder, manager);

        // LOUDS还原文档输出
        // std::string reconstructed_json = reconstructJsonFromTrie(restored_trie, manager);
        // 写入文件
        // std::ofstream out_file("reconstructed.json");
        // if (out_file.is_open()) {
        //     out_file << reconstructed_json;
        //     out_file.close();
        //     std::cout << "\n[INFO] LOUDS还原文档已写入 reconstructed.json\n";
        // } else {
        //     std::cerr << "[ERROR] 无法写入 reconstructed.json 文件！\n";
        // }
        
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 