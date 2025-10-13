#pragma once

#include "louds.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>

namespace json2 {
namespace query {

/**
 * 路径缓存项
 */
struct PathCacheItem {
    std::vector<NodeValue> path;                    // 缓存的路径
    std::chrono::steady_clock::time_point timestamp; // 缓存时间戳
    size_t access_count;                            // 访问次数
    size_t memory_size;                             // 内存大小（字节）
    
    PathCacheItem() : access_count(0), memory_size(0) {}
    PathCacheItem(const std::vector<NodeValue>& p) 
        : path(p), timestamp(std::chrono::steady_clock::now()), access_count(1) {
        memory_size = calculateMemorySize();
    }
    
private:
    size_t calculateMemorySize() const {
        size_t size = sizeof(PathCacheItem);
        size += path.size() * sizeof(NodeValue);
        // 估算NodeValue的内存占用
        for (const auto& nv : path) {
            if (std::holds_alternative<uint32_t>(nv)) {
                size += sizeof(uint32_t);
            } else if (std::holds_alternative<int64_t>(nv)) {
                size += sizeof(int64_t);
            } else if (std::holds_alternative<double>(nv)) {
                size += sizeof(double);
            } else if (std::holds_alternative<bool>(nv)) {
                size += sizeof(bool);
            }
            // 其他类型的NodeValue
        }
        return size;
    }
};

/**
 * 路径缓存配置
 */
struct PathCacheConfig {
    size_t max_cache_size = 100 * 1024 * 1024;     // 最大缓存大小（100MB）
    size_t max_cache_items = 10000;                 // 最大缓存项数
    std::chrono::minutes max_age{30};               // 最大缓存时间（30分钟）
    double hit_ratio_threshold = 0.8;               // 命中率阈值
    bool enable_lru_eviction = true;                // 启用LRU淘汰
    bool enable_compression = false;                // 启用压缩（未来扩展）
};

/**
 * 路径缓存统计
 */
struct PathCacheStats {
    size_t total_requests = 0;                      // 总请求数
    size_t cache_hits = 0;                          // 缓存命中数
    size_t cache_misses = 0;                        // 缓存未命中数
    size_t evictions = 0;                           // 淘汰次数
    size_t current_memory_usage = 0;                // 当前内存使用量
    size_t current_item_count = 0;                  // 当前缓存项数
    double hit_ratio = 0.0;                         // 命中率
    
    void updateHitRatio() {
        if (total_requests > 0) {
            hit_ratio = static_cast<double>(cache_hits) / total_requests;
        }
    }
};

/**
 * 路径缓存器
 * 缓存已重建的路径以提高查询性能
 */
class PathCache {
public:
    explicit PathCache(const PathCacheConfig& config = PathCacheConfig{});
    ~PathCache() = default;
    
    /**
     * 获取缓存的路径
     * @param bfs_idx BFS索引
     * @return 缓存的路径，如果不存在则返回nullptr
     */
    std::shared_ptr<const std::vector<NodeValue>> get(size_t bfs_idx);
    
    /**
     * 缓存路径
     * @param bfs_idx BFS索引
     * @param path 要缓存的路径
     * @return 是否成功缓存
     */
    bool put(size_t bfs_idx, const std::vector<NodeValue>& path);
    
    /**
     * 批量获取路径
     * @param bfs_indices BFS索引列表
     * @return 缓存的路径映射（只包含命中的项）
     */
    std::unordered_map<size_t, std::shared_ptr<const std::vector<NodeValue>>> 
    getBatch(const std::vector<size_t>& bfs_indices);
    
    /**
     * 批量缓存路径
     * @param paths 路径映射
     * @return 成功缓存的数量
     */
    size_t putBatch(const std::unordered_map<size_t, std::vector<NodeValue>>& paths);
    
    /**
     * 检查缓存中是否存在指定路径
     * @param bfs_idx BFS索引
     * @return 是否存在
     */
    bool contains(size_t bfs_idx) const;
    
    /**
     * 移除指定路径
     * @param bfs_idx BFS索引
     * @return 是否成功移除
     */
    bool remove(size_t bfs_idx);
    
    /**
     * 清空缓存
     */
    void clear();
    
    /**
     * 获取缓存统计信息
     * @return 统计信息
     */
    PathCacheStats getStats() const;
    
    /**
     * 获取缓存配置
     * @return 配置信息
     */
    const PathCacheConfig& getConfig() const { return config_; }
    
    /**
     * 更新缓存配置
     * @param config 新配置
     */
    void updateConfig(const PathCacheConfig& config);
    
    /**
     * 执行缓存维护（清理过期项、LRU淘汰等）
     */
    void performMaintenance();
    
    /**
     * 预热缓存
     * @param louds LOUDS结构
     * @param bfs_indices 要预热的BFS索引列表
     * @param max_items 最大预热项数
     */
    void warmup(const LOUDSTrie& louds, 
                const std::vector<size_t>& bfs_indices,
                size_t max_items = 1000);

private:
    PathCacheConfig config_;
    mutable std::mutex cache_mutex_;
    std::unordered_map<size_t, std::shared_ptr<PathCacheItem>> cache_;
    PathCacheStats stats_;
    
    // 辅助方法
    bool shouldEvict() const;
    void evictLRU();
    void evictExpired();
    void updateStats();
    bool isValidItem(const PathCacheItem& item) const;
    void cleanup();
};

/**
 * 路径缓存管理器
 * 管理多个路径缓存实例
 */
class PathCacheManager {
public:
    static PathCacheManager& getInstance();
    
    /**
     * 获取或创建缓存
     * @param cache_name 缓存名称
     * @param config 缓存配置
     * @return 缓存实例
     */
    std::shared_ptr<PathCache> getOrCreateCache(const std::string& cache_name,
                                               const PathCacheConfig& config = PathCacheConfig{});
    
    /**
     * 移除缓存
     * @param cache_name 缓存名称
     */
    void removeCache(const std::string& cache_name);
    
    /**
     * 清空所有缓存
     */
    void clearAllCaches();
    
    /**
     * 获取所有缓存的统计信息
     * @return 统计信息映射
     */
    std::unordered_map<std::string, PathCacheStats> getAllStats() const;
    
    /**
     * 执行全局缓存维护
     */
    void performGlobalMaintenance();

private:
    PathCacheManager() = default;
    ~PathCacheManager() = default;
    PathCacheManager(const PathCacheManager&) = delete;
    PathCacheManager& operator=(const PathCacheManager&) = delete;
    
    mutable std::mutex manager_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PathCache>> caches_;
};

} // namespace query
} // namespace json2
