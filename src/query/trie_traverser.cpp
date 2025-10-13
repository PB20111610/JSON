#include "../include/query/trie_traverser.h"
#include "../include/loudsTotrie.h"
#include <chrono>
#include <algorithm>

namespace json2 {
namespace query {

TrieTraversalResult TrieTraverser::traverse(const LOUDSTrie& louds,
                                           const FieldDictionaryManager& dict_manager,
                                           const QueryNode& query_root) {
    TrieTraversalResult result;
    result.nodes_visited = 0;
    result.paths_found = 0;
    result.is_complete = true;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // 使用LOUDS结构遍历
        traverseLouds(louds, dict_manager, query_root, result);
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.traversal_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.is_complete = false;
        result.traversal_time_ms = 0.0;
    }
    
    return result;
}

void TrieTraverser::traverseLouds(const LOUDSTrie& louds,
                                 const FieldDictionaryManager& dict_manager,
                                 const QueryNode& query_node,
                                 TrieTraversalResult& result) {
    // 基于LOUDS的BFS顺序遍历，构建当前路径并进行匹配
    // 注意：LOUDS以BFS顺序存储节点，父子关系通过select/rank导航
    result.is_complete = true;
    std::vector<NodeValue> current_path;
    current_path.reserve(louds.layerCount());

    // 我们按层次迭代，维护一个简单的栈来模拟根到当前节点的路径
    // 使用显式栈记录 (node_idx, depth, sibling_progress)
    struct Frame { size_t node_idx; size_t depth; };
    std::vector<Frame> stack;

    if (louds.nodeCount() == 0) {
        return;
    }

    // LOUDS根通常位于BFS idx 0（或隐式根），这里从其第一个孩子开始
    size_t root_idx = 0;
    if (!louds.hasChild(root_idx)) {
        return;
    }
    size_t child = louds.firstChild(root_idx);
    for (; child != 0; child = louds.nextSibling(child)) {
        stack.push_back({child, 0});
    }

    // 仅用于简单字段查询的值层剪枝（FIELD 类型）
    bool enable_value_prune = (query_node.getType() == QueryNodeType::FIELD);
    std::string target_field;
    FieldType target_type = FieldType::String;
    QueryOperator target_op = QueryOperator::EQUALS;
    std::string expected_value;
    size_t target_depth = static_cast<size_t>(-1);
    if (enable_value_prune) {
        target_field = query_node.getContent();
        target_type = query_node.getFieldType();
        if (!query_node.getChildren().empty()) {
            // 约定第一个子节点为操作或范围描述
            const auto& op_or_val = query_node.getChildren()[0];
            if (op_or_val->getType() == QueryNodeType::OPERATOR) {
                target_op = op_or_val->getOperator();
                if (query_node.getChildren().size() > 1) {
                    expected_value = query_node.getChildren()[1]->getContent();
                }
            } else {
                expected_value = op_or_val->getContent();
            }
        }
        const auto& fields = louds.getFieldOrder();
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == target_field) { target_depth = i; break; }
        }
        if (target_depth == static_cast<size_t>(-1)) enable_value_prune = false;
    }

    while (!stack.empty()) {
        Frame frame = stack.back();
        stack.pop_back();
        result.nodes_visited++;

        // 将该节点值加入当前路径（按层索引）
        // 保证 current_path 大小与 depth 对齐
        if (current_path.size() > frame.depth) {
            current_path.resize(frame.depth);
        }
        const NodeValue& nv = louds.getLayerNodeValue(frame.depth, frame.node_idx);
        current_path.push_back(nv);

        // 值层剪枝：当到达目标字段层时进行值过滤，不匹配则不再下探
        if (enable_value_prune && frame.depth == target_depth) {
            const auto& fk = louds.getFieldOrder()[target_depth];
            // 解码 NodeValue 为字符串进行比较（最简实现）
            std::string actual;
            if (std::holds_alternative<uint32_t>(nv)) {
                uint32_t code = std::get<uint32_t>(nv);
                FieldKey key{fk.name, fk.type};
                auto opt = dict_manager.getFieldValueByCode(key, code);
                if (opt) {
                    const Value& v = *opt;
                    if (std::holds_alternative<std::string>(v)) actual = std::get<std::string>(v);
                    else if (std::holds_alternative<int64_t>(v)) actual = std::to_string(std::get<int64_t>(v));
                    else if (std::holds_alternative<double>(v)) actual = std::to_string(std::get<double>(v));
                    else if (std::holds_alternative<bool>(v)) actual = std::get<bool>(v) ? "true" : "false";
                }
            }
            // 比较，不匹配则跳过该分支
            if (!expected_value.empty()) {
                if (!compareValues(actual, expected_value, target_op)) {
                    // 回溯移除本层后继续下一个分支
                    current_path.pop_back();
                    continue;
                }
            }
        }

        // 路径匹配
        if (matchesQuery(current_path, dict_manager, query_node)) {
            result.matching_paths.push_back(current_path);
            result.paths_found++;
        }

        // 遍历孩子：按兄弟顺序压栈（后进先出，等效于DFS，但节点访问仍遵循LOUDS导航）
        if (louds.hasChild(frame.node_idx)) {
            size_t first = louds.firstChild(frame.node_idx);
            for (size_t c = first; c != 0; c = louds.nextSibling(c)) {
                stack.push_back({c, frame.depth + 1});
            }
        }
    }
}

bool TrieTraverser::matchesQuery(const std::vector<NodeValue>& path,
                                const FieldDictionaryManager& dict_manager,
                                const QueryNode& query_node) {
    switch (query_node.getType()) {
        case QueryNodeType::FIELD: {
            // Check if this is a field existence query
            const auto& children = query_node.getChildren();
            if (!children.empty() && children[0]->getType() == QueryNodeType::OPERATOR && 
                children[0]->getOperator() == QueryOperator::EXISTS) {
                // For field existence, we just need to check if we can extract a value for the field
                std::string actual_value = extractFieldValue(path, dict_manager, query_node.getContent(), 
                                                           query_node.getFieldType());
                return !actual_value.empty();
            }
            // Regular field query
            return matchesLogical(path, dict_manager, query_node);
        }
        case QueryNodeType::LOGICAL:
            return matchesLogical(path, dict_manager, query_node);
        default:
            return false;
    }
}

bool TrieTraverser::matchesLogical(const std::vector<NodeValue>& path,
                                  const FieldDictionaryManager& dict_manager,
                                  const QueryNode& logical_node) {
    QueryOperator op = logical_node.getOperator();
    
    switch (op) {
        case QueryOperator::AND: {
            bool result = true;
            for (const auto& child : logical_node.getChildren()) {
                if (!matchesQuery(path, dict_manager, *child)) {
                    result = false;
                    break;
                }
            }
            return result;
        }
        case QueryOperator::OR: {
            bool result = false;
            for (const auto& child : logical_node.getChildren()) {
                if (matchesQuery(path, dict_manager, *child)) {
                    result = true;
                    break;
                }
            }
            return result;
        }
        case QueryOperator::NOT: {
            if (logical_node.getChildren().empty()) {
                return false;
            }
            return !matchesQuery(path, dict_manager, *logical_node.getChildren()[0]);
        }
        default:
            return false;
    }
}

std::string TrieTraverser::extractFieldValue(const std::vector<NodeValue>& path,
                                            const FieldDictionaryManager& dict_manager,
                                            const std::string& field_name,
                                            FieldType field_type) {
    // 基于 field_order 顺序假设：path[i] 对应 field_order[i]
    // 我们无法直接访问 field_order，这里用启发式：在同层次路径中匹配第一个同类型字段。
    // 更优做法：在遍历入口设置当前的 field_order 到 Traverser 状态中。
    for (const NodeValue& nv : path) {
        // 仅对不同类型分支做基本解码
        switch (field_type) {
            case FieldType::String:
            case FieldType::UnstructuredArray: {
                if (std::holds_alternative<uint32_t>(nv)) {
                    uint32_t code = std::get<uint32_t>(nv);
                    // 使用字典管理器解码字符串值
                    FieldKey fk{field_name, field_type};
                    auto opt_value = dict_manager.getFieldValueByCode(fk, code);
                    if (opt_value && std::holds_alternative<std::string>(*opt_value)) {
                        return std::get<std::string>(*opt_value);
                    }
                }
                break;
            }
            case FieldType::Int: {
                if (std::holds_alternative<int64_t>(nv)) {
                    return std::to_string(std::get<int64_t>(nv));
                }
                break;
            }
            case FieldType::Double: {
                if (std::holds_alternative<double>(nv)) {
                    return std::to_string(std::get<double>(nv));
                }
                break;
            }
            case FieldType::Bool: {
                if (std::holds_alternative<bool>(nv)) {
                    return std::get<bool>(nv) ? "true" : "false";
                }
                break;
            }
            case FieldType::Timestamp: {
                if (std::holds_alternative<TemplateEncodedTimestamp>(nv)) {
                    const auto& enc = std::get<TemplateEncodedTimestamp>(nv);
                    // 使用字典管理器解码时间戳
                    FieldKey fk{field_name, field_type};
                    return dict_manager.timestampDict().decodeTemplate(fk, enc);
                }
                break;
            }
            case FieldType::LogType: {
                if (std::holds_alternative<EncodedLog>(nv)) {
                    const auto& enc = std::get<EncodedLog>(nv);
                    // 使用字典管理器解码日志类型
                    FieldKey fk{field_name, field_type};
                    return dict_manager.logtypeDict().decodeLogToString(fk, enc);
                }
                break;
            }
            default:
                break;
        }
    }
    return "";
}

bool TrieTraverser::compareValues(const std::string& actual_value,
                                 const std::string& expected_value,
                                 QueryOperator operator_) {
    switch (operator_) {
        case QueryOperator::EQUALS:
            return actual_value == expected_value;
        case QueryOperator::NOT_EQUALS:
            return actual_value != expected_value;
        case QueryOperator::GREATER:
            return actual_value > expected_value;
        case QueryOperator::GREATER_EQUAL:
            return actual_value >= expected_value;
        case QueryOperator::LESS:
            return actual_value < expected_value;
        case QueryOperator::LESS_EQUAL:
            return actual_value <= expected_value;
        case QueryOperator::CONTAINS:
            return actual_value.find(expected_value) != std::string::npos;
        case QueryOperator::MATCHES:
            // 简化的模式匹配
            return actual_value.find(expected_value) != std::string::npos;
        default:
            return false;
    }
}

void TrieTraverser::optimizeTraversal(const QueryNode& query_node, 
                                     std::vector<PathMatchCondition>& conditions) {
    // 这里需要实现遍历优化逻辑
    // 简化实现：暂时跳过
}

bool TrieTraverser::shouldTerminate(const TrieTraversalResult& result, 
                                   size_t max_results) const {
    return result.paths_found >= max_results;
}

// ========== 新增：适配新查询思路的实现 ==========

TrieTraversalResult TrieTraverser::findExactMatchesWithNewApproach(const LOUDSTrie& louds,
                                                                  const FieldDictionaryManager& dict_manager,
                                                                  const std::string& field_name,
                                                                  FieldType field_type,
                                                                  const std::string& value) {
    TrieTraversalResult result;
    result.nodes_visited = 0;
    result.paths_found = 0;
    result.is_complete = true;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // 1. 找到字段对应的层索引
        size_t layer_index = findFieldLayerIndex(field_name, louds);
        
        // 2. 遍历该层的所有节点值
        const std::vector<NodeValue>& layer_values = louds.getLayer(layer_index);
        
        // 3. 对于每个节点值，检查是否匹配查询值
        for (size_t node_idx = 0; node_idx < layer_values.size(); ++node_idx) {
            result.nodes_visited++;
            
            const NodeValue& node_value = layer_values[node_idx];
            
            // 4. 解码节点值并与查询值比较
            FieldKey field_key{field_name, field_type};
            std::string decoded_value = decodeNodeValueToFieldValue(field_key, node_value, dict_manager);
            
            if (decoded_value == value) {
                // 5. 如果匹配，重建完整路径
                size_t bfs_idx = louds.getLayeredStorage().layerIndexToBFS(layer_index, node_idx);
                std::vector<size_t> bfs_path = louds.reconstructPathToRoot(bfs_idx);
                
                // 6. 获取路径中的所有节点值
                std::vector<NodeValue> path_values;
                path_values.reserve(bfs_path.size());
                for (size_t idx : bfs_path) {
                    path_values.push_back(louds.getNodeValue(idx));
                }
                
                result.matching_paths.push_back(path_values);
                result.path_indices.push_back(bfs_idx);
                result.paths_found++;
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.traversal_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.is_complete = false;
        result.traversal_time_ms = 0.0;
    }
    
    return result;
}

TrieTraversalResult TrieTraverser::findRangeMatchesWithNewApproach(const LOUDSTrie& louds,
                                                                  const FieldDictionaryManager& dict_manager,
                                                                  const std::string& field_name,
                                                                  FieldType field_type,
                                                                  const std::string& min_value,
                                                                  const std::string& max_value) {
    TrieTraversalResult result;
    result.nodes_visited = 0;
    result.paths_found = 0;
    result.is_complete = true;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // 1. 找到字段对应的层索引
        size_t layer_index = findFieldLayerIndex(field_name, louds);
        
        // 2. 遍历该层的所有节点值
        const std::vector<NodeValue>& layer_values = louds.getLayer(layer_index);
        
        // 3. 对于每个节点值，检查是否在查询范围内
        for (size_t node_idx = 0; node_idx < layer_values.size(); ++node_idx) {
            result.nodes_visited++;
            
            const NodeValue& node_value = layer_values[node_idx];
            
            // 4. 解码节点值并与查询范围比较
            FieldKey field_key{field_name, field_type};
            std::string decoded_value = decodeNodeValueToFieldValue(field_key, node_value, dict_manager);
            
            if (decoded_value >= min_value && decoded_value <= max_value) {
                // 5. 如果匹配，重建完整路径
                size_t bfs_idx = louds.getLayeredStorage().layerIndexToBFS(layer_index, node_idx);
                std::vector<size_t> bfs_path = louds.reconstructPathToRoot(bfs_idx);
                
                // 6. 获取路径中的所有节点值
                std::vector<NodeValue> path_values;
                path_values.reserve(bfs_path.size());
                for (size_t idx : bfs_path) {
                    path_values.push_back(louds.getNodeValue(idx));
                }
                
                // 7. 解码路径值为字符串
                std::vector<std::string> decoded_path = decodeLayerValuesToPath(
                    getLayerValuesFromBFSPath(louds, bfs_path, louds.getFieldOrder()),
                    louds.getFieldOrder(),
                    dict_manager
                );
                
                result.matching_paths.push_back(path_values);
                result.decoded_paths.push_back(decoded_path);
                result.path_indices.push_back(bfs_idx);
                result.paths_found++;
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.traversal_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.is_complete = false;
        result.traversal_time_ms = 0.0;
    }
    
    return result;
}

TrieTraversalResult TrieTraverser::findFieldExistsWithNewApproach(const LOUDSTrie& louds,
                                                                 const std::string& field_name) {
    TrieTraversalResult result;
    result.nodes_visited = 0;
    result.paths_found = 0;
    result.is_complete = true;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // 1. 找到字段对应的层索引
        size_t layer_index = findFieldLayerIndex(field_name, louds);
        
        // 2. 遍历该层的所有节点值
        const std::vector<NodeValue>& layer_values = louds.getLayer(layer_index);
        
        // 3. 对于每个节点值，重建完整路径
        for (size_t node_idx = 0; node_idx < layer_values.size(); ++node_idx) {
            result.nodes_visited++;
            
            // 4. 重建完整路径
            size_t bfs_idx = louds.getLayeredStorage().layerIndexToBFS(layer_index, node_idx);
            std::vector<size_t> bfs_path = louds.reconstructPathToRoot(bfs_idx);
            
            // 5. 获取路径中的所有节点值
            std::vector<NodeValue> path_values;
            path_values.reserve(bfs_path.size());
            for (size_t idx : bfs_path) {
                path_values.push_back(louds.getNodeValue(idx));
            }
            
            result.matching_paths.push_back(path_values);
            result.path_indices.push_back(bfs_idx);
            result.paths_found++;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        result.traversal_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
    } catch (const std::exception& e) {
        result.is_complete = false;
        result.traversal_time_ms = 0.0;
    }
    
    return result;
}

// ========== 新增：适配新查询思路的辅助方法实现 ==========

size_t TrieTraverser::findFieldLayerIndex(const std::string& field_name, const LOUDSTrie& louds) const {
    const auto& field_order = louds.getFieldOrder();
    for (size_t i = 0; i < field_order.size(); ++i) {
        if (field_order[i].name == field_name) {
            return i;
        }
    }
    return 0; // 默认返回第一层
}

std::vector<std::string> TrieTraverser::decodePathValues(const std::vector<NodeValue>& path,
                                                        const LOUDSTrie& louds,
                                                        const FieldDictionaryManager& dict_manager) const {
    std::vector<std::string> decoded_path;
    const auto& field_order = louds.getFieldOrder();
    
    for (size_t i = 0; i < path.size() && i < field_order.size(); ++i) {
        const NodeValue& node_value = path[i];
        const FieldKey& field_key = field_order[i];
        
        std::string decoded_value = decodeNodeValueToFieldValue(field_key, node_value, dict_manager);
        decoded_path.push_back(decoded_value);
    }
    
    return decoded_path;
}

// ========== 新增：缓存相关方法实现 ==========

void TrieTraverser::setPathCache(std::shared_ptr<PathCache> cache) {
    path_cache_ = cache;
}

std::shared_ptr<PathCache> TrieTraverser::getPathCache() const {
    return path_cache_;
}

void TrieTraverser::warmupCache(const LOUDSTrie& louds, 
                               const std::vector<size_t>& bfs_indices,
                               size_t max_items) {
    if (path_cache_) {
        path_cache_->warmup(louds, bfs_indices, max_items);
    }
}

std::vector<NodeValue> TrieTraverser::getPathWithCache(size_t bfs_idx, const LOUDSTrie& louds) {
    if (path_cache_) {
        auto cached_path = path_cache_->get(bfs_idx);
        if (cached_path) {
            return *cached_path;
        }
    }
    
    // 缓存未命中，重建路径
    std::vector<size_t> bfs_path = louds.reconstructPathToRoot(bfs_idx);
    std::vector<NodeValue> path;
    path.reserve(bfs_path.size());
    
    for (size_t idx : bfs_path) {
        path.push_back(louds.getNodeValue(idx));
    }
    
    // 缓存路径
    if (path_cache_) {
        path_cache_->put(bfs_idx, path);
    }
    
    return path;
}

std::unordered_map<size_t, std::vector<NodeValue>> 
TrieTraverser::getBatchPathsWithCache(const std::vector<size_t>& bfs_indices, const LOUDSTrie& louds) {
    std::unordered_map<size_t, std::vector<NodeValue>> result;
    
    if (path_cache_) {
        // 批量获取缓存
        auto cached_paths = path_cache_->getBatch(bfs_indices);
        
        // 处理缓存命中的路径
        for (const auto& [bfs_idx, cached_path] : cached_paths) {
            result[bfs_idx] = *cached_path;
        }
        
        // 处理缓存未命中的路径
        std::vector<size_t> uncached_indices;
        for (size_t bfs_idx : bfs_indices) {
            if (result.find(bfs_idx) == result.end()) {
                uncached_indices.push_back(bfs_idx);
            }
        }
        
        if (!uncached_indices.empty()) {
            std::unordered_map<size_t, std::vector<NodeValue>> paths_to_cache;
            
            // 批量重建未缓存的路径
            for (size_t bfs_idx : uncached_indices) {
                std::vector<size_t> bfs_path = louds.reconstructPathToRoot(bfs_idx);
                std::vector<NodeValue> path;
                path.reserve(bfs_path.size());
                
                for (size_t idx : bfs_path) {
                    path.push_back(louds.getNodeValue(idx));
                }
                
                result[bfs_idx] = path;
                paths_to_cache[bfs_idx] = path;
            }
            
            // 批量缓存未命中的路径
            if (!paths_to_cache.empty()) {
                path_cache_->putBatch(paths_to_cache);
            }
        }
    } else {
        // 没有缓存，直接重建所有路径
        for (size_t bfs_idx : bfs_indices) {
            std::vector<size_t> bfs_path = louds.reconstructPathToRoot(bfs_idx);
            std::vector<NodeValue> path;
            path.reserve(bfs_path.size());
            
            for (size_t idx : bfs_path) {
                path.push_back(louds.getNodeValue(idx));
            }
            
            result[bfs_idx] = path;
        }
    }
    
    return result;
}

} // namespace query
} // namespace json2