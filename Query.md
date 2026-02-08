# JSON压缩框架查询系统设计文档

## 1. 项目概述

本文档基于项目实际架构设计查询系统。项目使用Trie树结构存储JSON数据，通过FieldDictionaryManager管理不同类型的字典（String、Timestamp、LogType），并支持分块压缩和类型感知压缩。

### 1.1 核心架构组件

- **Trie树**: 路径压缩的Trie结构，存储JSON字段路径和值
- **FieldDictionaryManager**: 管理三种字典（Variable、Timestamp、LogType）
- **分块压缩**: ChunkedTrieCompressor和ChunkedTypeAwareCompressor
- **LOUDS结构**: 用于高效存储和访问Trie位图
- **类型感知压缩**: 针对不同字段类型使用不同压缩策略

### 1.2 数据存储结构

```
压缩数据块 (ChunkedBlock)
├── 字段顺序 (ordered_fields)
├── 字典管理器 (FieldDictionaryManager)
│   ├── Variable Dictionary (String/Int/Double/Bool)
│   ├── Timestamp Dictionary (模板编码)
│   └── LogType Dictionary (日志模板)
└── Trie树 (路径压缩结构)
    └── 节点值 (NodeValue: uint32_t/int64_t/double/bool/nullptr/编码值)
```

## 2. 查询类型设计

### 2.1 基础字段查询

#### 精确匹配查询
```
# 基于字典编码的精确匹配
field: value

# 示例:
level: "ERROR"
service: "api-server" 
status: 200
```

#### 字段存在性查询
```
# 检查字段是否存在（基于Trie路径）
field: *

# 示例:
error: *
stack_trace: *
```

#### 类型匹配查询
```
# 基于FieldType的类型匹配
field:type: value

# 示例:
timestamp:timestamp: "2023-01-01T00:00:00Z"
message:string: "error occurred"
count:int: 100
```

### 2.2 数值范围查询

#### 数值比较操作
```
# 基于字典编码的数值比较
field > value
field >= value  
field < value
field <= value

# 示例:
response_time > 1000
status_code >= 400
request_size < 1024
```

#### 范围查询
```
# 数值范围查询
field: [min_value TO max_value]

# 示例:
response_time: [100 TO 1000]
status_code: [400 TO 599]
```

### 2.3 嵌套字段查询

#### 点号路径查询
```
# 基于Trie路径的嵌套字段访问
parent.child: value
parent.grandchild.field: value

# 示例:
user.id: 12345
request.headers.authorization: "Bearer *"
```

#### 路径模式匹配
```
# 支持通配符的路径匹配
parent.*.field: value
*.child: value

# 示例:
user.*.id: 12345
*.headers.authorization: "Bearer *"
```

### 2.4 字典查询

#### 字典值查询
```
# 基于字典编码的查询
field:dict: value

# 示例:
message:dict: "error occurred"
timestamp:dict: "2023-01-01T00:00:00Z"
```

#### 模板查询
```
# 基于LogType模板的查询
field:template: pattern

# 示例:
log:template: "User * logged in"
error:template: "* error in *"
```

### 2.5 复合查询

#### AND查询
```
# 多条件AND组合
field1: value1 AND field2: value2

# 示例:
level: "ERROR" AND service: "payment"
status: 500 AND response_time > 1000
```

#### OR查询
```
# 多条件OR组合
field1: value1 OR field2: value2

# 示例:
level: "ERROR" OR level: "CRITICAL"
service: "auth" OR service: "payment"
```

#### 复杂组合
```
# 括号分组
(field1: value1 OR field2: value2) AND field3: value3

# 示例:
(level: "ERROR" OR level: "CRITICAL") AND service: "payment"
```

## 3. 查询执行架构

### 3.1 查询处理流程

基于项目实际架构的查询处理流程：

1. **查询解析**: 将查询字符串解析为查询AST
2. **字段分析**: 分析查询涉及的字段和类型
3. **块选择**: 基于字段存在性和类型选择相关数据块
4. **选择性解压**: 只解压查询所需的字典和Trie部分
5. **Trie遍历**: 在Trie中查找匹配的路径和值
6. **结果重建**: 将匹配的编码值重建为原始JSON
7. **结果聚合**: 合并多个块的结果

### 3.2 查询处理架构图

```
查询处理流程:
┌─────────────────┐
│   查询解析器     │  ← 解析查询语法，生成AST
└─────────┬───────┘
          │
┌─────────▼────────┐
│   字段分析器     │  ← 分析查询字段和类型
└─────────┬────────┘
          │
┌─────────▼────────┐
│   块选择器       │  ← 基于字段存在性选择块
└─────────┬────────┘
          │
┌─────────▼────────┐
│ 选择性解压器     │  ← 只解压相关字典和Trie
└─────────┬────────┘
          │
┌─────────▼────────┐
│  Trie遍历器      │  ← 在Trie中查找匹配路径
└─────────┬────────┘
          │
┌─────────▼────────┐
│   结果重建器     │  ← 将编码值重建为JSON
└─────────┬────────┘
          │
┌─────────▼────────┐
│   结果聚合器     │  ← 合并多块结果
└──────────────────┘
```

### 3.3 选择性解压机制

#### 字段级解压
只解压查询涉及的字段字典：
```
查询: level: "ERROR"
处理:
1. 识别包含"level"字段的块
2. 只解压Variable Dictionary中的String部分
3. 在字典中查找"ERROR"的编码
4. 在Trie中查找包含该编码的路径
```

#### 类型感知解压
基于字段类型选择解压策略：
```
查询: timestamp: "2023-01-01T00:00:00Z"
处理:
1. 识别Timestamp类型字段
2. 只解压Timestamp Dictionary
3. 使用模板匹配查找时间戳
4. 在Trie中查找匹配的编码路径
```

#### 字典级解压
基于查询类型选择字典：
```
查询: message:template: "User * logged in"
处理:
1. 识别LogType模板查询
2. 只解压LogType Dictionary
3. 查找匹配的日志模板
4. 在Trie中查找对应的编码路径
```

## 4. 性能优化策略

### 4.1 索引策略

#### 字段存在性索引
- 为每个块维护字段存在性位图
- 快速识别包含特定字段的块
- 减少不必要的块解压

#### 字典值索引
- 为常用字典值建立反向索引
- 快速定位包含特定值的块
- 支持范围查询优化

#### Trie路径索引
- 为常用路径模式建立索引
- 加速嵌套字段查询
- 支持路径通配符匹配

### 4.2 缓存机制

#### 字典缓存
```
策略:
- 缓存常用字典在内存中
- 使用LRU策略管理内存
- 预加载高频查询的字典
```

#### Trie结构缓存
```
策略:
- 缓存部分解压的Trie结构
- 复用Trie遍历结果
- 支持增量更新
```

#### 查询结果缓存
```
策略:
- 缓存频繁查询的结果
- 实现查询结果失效策略
- 支持参数化查询缓存
```

### 4.3 并行处理

#### 块级并行
```
方法:
- 并行处理多个数据块
- 利用多核CPU资源
- 异步合并结果
```

#### 字典并行解压
```
方法:
- 并行解压不同类型的字典
- 重叠I/O和CPU操作
- 流水线处理
```

#### Trie遍历并行
```
方法:
- 并行遍历Trie的不同分支
- 利用SIMD指令加速
- 批量处理节点值
```

## 5. 性能指标和量化

### 5.1 查询性能指标

#### 响应时间
- **指标**: 从查询提交到结果返回的时间
- **测量**: 毫秒
- **优化目标**: 95%的查询 < 50ms

#### 吞吐量
- **指标**: 每秒处理的查询数
- **测量**: QPS (Queries Per Second)
- **优化目标**: 简单查询 > 2000 QPS

#### 资源利用率
- **CPU使用率**: CPU核心利用率百分比
- **内存使用**: 查询执行时的RAM消耗
- **I/O操作**: 每次查询的磁盘读写次数

### 5.2 压缩效率指标

#### 压缩比
- **指标**: 实现的压缩率
- **公式**: (原始大小 - 压缩大小) / 原始大小
- **目标**: > 80% 压缩比

#### 解压速度
- **指标**: 解压数据的时间
- **测量**: MB/s
- **目标**: > 200 MB/s 解压速度

### 5.3 选择性解压指标

#### 解压减少率
- **指标**: 避免解压的数据百分比
- **公式**: (总数据 - 解压数据) / 总数据
- **目标**: 针对性查询 > 90% 减少

#### 块命中率
- **指标**: 访问的块数 vs 总块数
- **公式**: 访问块数 / 总块数
- **目标**: 良好索引的查询 < 10%

### 5.4 内存效率指标

#### 工作集大小
- **指标**: 查询处理所需内存
- **测量**: MB
- **目标**: 典型查询 < 50MB

#### 缓存命中率
- **指标**: 缓存命中 vs 总访问次数
- **公式**: 缓存命中 / 总访问
- **目标**: 字典缓存 > 95%

## 6. 实现指南

### 6.1 查询引擎设计

#### 模块化架构
```
组件:
- 查询解析器: 将文本转换为AST
- 字段分析器: 分析查询字段和类型
- 块管理器: 管理数据块访问
- 选择性解压器: 处理选择性解压
- Trie遍历器: 在Trie中查找匹配
- 结果重建器: 重建JSON结果
- 结果聚合器: 合并部分结果
```

#### 错误处理
```
原则:
- 部分结果的优雅降级
- 清晰的调试错误消息
- 瞬态故障的恢复机制
```

### 6.2 高效查询的数据结构

#### 查询AST节点
```cpp
struct QueryNode {
    enum Type { FIELD, VALUE, OPERATOR, LOGICAL };
    Type type;
    std::string field_name;
    FieldType field_type;
    std::string value;
    std::string operator;
    std::vector<std::unique_ptr<QueryNode>> children;
};
```

#### 块元数据
```cpp
struct ChunkMetadata {
    std::vector<FieldKey> schema;           // 块中的字段
    std::unordered_set<std::string> fields; // 字段存在性
    size_t record_count;                    // 记录数量
    size_t compressed_size;                 // 压缩后大小
    double compression_ratio;               // 压缩比
};
```

#### 查询结果
```cpp
struct QueryResult {
    std::vector<std::string> records;       // 匹配的记录
    size_t count;                          // 结果数量
    double query_time_ms;                  // 执行时间
    size_t chunks_accessed;                // 访问的块数
    size_t dict_hits;                      // 字典命中数
    size_t trie_nodes_visited;             // 访问的Trie节点数
};
```

### 6.3 查询优化技术

#### 早期终止
```
策略:
- 找到足够结果时停止处理
- 实现LIMIT子句支持
- 使用结果边界进行优化
```

#### 谓词下推
```
策略:
- 将过滤器下推到块级别
- 在解压过程中应用条件
- 减少数据移动
```

#### 投影下推
```
策略:
- 只重建需要的字段
- 跳过不必要的字段处理
- 减少内存占用
```

## 7. 查询执行示例

### 7.1 简单精确匹配
```cpp
// 查询: level: "ERROR"
QueryResult result = queryEngine.exactMatchQuery("level", "ERROR");

// 性能特征:
// - 访问块数: 15% 总数
// - 解压数据: 8% 总数据
// - 响应时间: 25ms
// - 字典命中: 95%
```

### 7.2 复杂复合查询
```cpp
// 查询: (level: "ERROR" OR level: "CRITICAL") AND service: "payment"
QueryNode root = QueryNode::logical("AND");
root.addChild(QueryNode::logical("OR")
    .addChild(QueryNode::field("level", "ERROR"))
    .addChild(QueryNode::field("level", "CRITICAL")));
root.addChild(QueryNode::field("service", "payment"));

QueryResult result = queryEngine.execute(root);

// 性能特征:
// - 访问块数: 8% 总数
// - 解压数据: 3% 总数据
// - 响应时间: 45ms
// - Trie节点访问: 1200
```

### 7.3 时间戳范围查询
```cpp
// 查询: timestamp: [1640995200000 TO 1641081600000]
QueryResult result = queryEngine.rangeQuery(
    "timestamp", 1640995200000, 1641081600000);

// 性能特征:
// - 访问块数: 5% 总数
// - 解压数据: 2% 总数据
// - 响应时间: 15ms
// - 时间戳字典命中: 98%
```

### 7.4 模板查询
```cpp
// 查询: message:template: "User * logged in"
QueryResult result = queryEngine.templateQuery(
    "message", "User * logged in");

// 性能特征:
// - 访问块数: 12% 总数
// - 解压数据: 5% 总数据
// - 响应时间: 30ms
// - LogType字典命中: 92%
```

## 8. 实现计划

### 8.1 第一阶段：基础查询
- 实现查询解析器
- 实现字段分析器
- 实现基础Trie遍历
- 实现结果重建

### 8.2 第二阶段：优化功能
- 实现选择性解压
- 实现缓存机制
- 实现索引支持
- 实现并行处理

### 8.3 第三阶段：高级功能
- 实现复合查询
- 实现模板查询
- 实现性能监控
- 实现查询优化

### 8.4 第四阶段：扩展功能
- 支持正则表达式
- 支持全文搜索
- 支持分布式查询
- 支持增量更新

本文档提供了基于项目实际架构的查询系统实现框架，确保与现有压缩系统的高度集成和优化性能。