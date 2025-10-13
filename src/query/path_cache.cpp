#include "../include/query/path_cache.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace json2 {
namespace query {

// ========== PathCache 实现 ==========

PathCache::PathCache(const PathCacheConfig& config) : config_(config) {
    cache_.reserve(config_.max_cache_items);
}

std::shared_ptr<const std::vector<NodeValue>> PathCache::get(size_t bfs_idx) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    stats_.total_requests++;
    
    auto it = cache_.find(bfs_idx);
    if (it != cache_.end()) {
        auto& item = it->second;
        
        // 检查是否过期
        if (!isValidItem(*item)) {
            cache_.erase(it);
            stats_.cache_misses++;
            updateStats();
            return nullptr;
        }
        
        // 更新访问信息
        item->access_count++;
        item->timestamp = std::chrono::steady_clock::now();
        
        stats_.cache_hits++;
        updateStats();
        return std::shared_ptr<const std::vector<NodeValue>>(item, &item->path);
    }
    
    stats_.cache_misses++;
    updateStats();
    return nullptr;
}

bool PathCache::put(size_t bfs_idx, const std::vector<NodeValue>& path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // 检查是否需要清理
    if (shouldEvict()) {
        performMaintenance();
    }
    
    // 检查缓存大小限制
    if (cache_.size() >= config_.max_cache_items) {
        if (config_.enable_lru_eviction) {
            evictLRU();
        } else {
            return false; // 缓存已满且未启用LRU淘汰
        }
    }
    
    // 创建缓存项
    auto item = std::make_shared<PathCacheItem>(path);
    
    // 检查内存限制
    if (stats_.current_memory_usage + item->memory_size > config_.max_cache_size) {
        if (config_.enable_lru_eviction) {
            evictLRU();
            // 再次检查内存限制
            if (stats_.current_memory_usage + item->memory_size > config_.max_cache_size) {
                return false;
            }
        } else {
            return false;
        }
    }
    
    // 添加到缓存
    cache_[bfs_idx] = item;
    stats_.current_memory_usage += item->memory_size;
    stats_.current_item_count++;
    
    return true;
}

std::unordered_map<size_t, std::shared_ptr<const std::vector<NodeValue>>> 
PathCache::getBatch(const std::vector<size_t>& bfs_indices) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    std::unordered_map<size_t, std::shared_ptr<const std::vector<NodeValue>>> result;
    
    for (size_t bfs_idx : bfs_indices) {
        stats_.total_requests++;
        
        auto it = cache_.find(bfs_idx);
        if (it != cache_.end()) {
            auto& item = it->second;
            
            if (isValidItem(*item)) {
                item->access_count++;
                item->timestamp = std::chrono::steady_clock::now();
                
                result[bfs_idx] = std::shared_ptr<const std::vector<NodeValue>>(item, &item->path);
                stats_.cache_hits++;
            } else {
                cache_.erase(it);
                stats_.cache_misses++;
            }
        } else {
            stats_.cache_misses++;
        }
    }
    
    updateStats();
    return result;
}

size_t PathCache::putBatch(const std::unordered_map<size_t, std::vector<NodeValue>>& paths) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    size_t success_count = 0;
    
    for (const auto& [bfs_idx, path] : paths) {
        if (put(bfs_idx, path)) {
            success_count++;
        }
    }
    
    return success_count;
}

bool PathCache::contains(size_t bfs_idx) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = cache_.find(bfs_idx);
    if (it != cache_.end()) {
        return isValidItem(*it->second);
    }
    
    return false;
}

bool PathCache::remove(size_t bfs_idx) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    auto it = cache_.find(bfs_idx);
    if (it != cache_.end()) {
        stats_.current_memory_usage -= it->second->memory_size;
        stats_.current_item_count--;
        cache_.erase(it);
        return true;
    }
    
    return false;
}

void PathCache::clear() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    cache_.clear();
    stats_.current_memory_usage = 0;
    stats_.current_item_count = 0;
}

PathCacheStats PathCache::getStats() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return stats_;
}

void PathCache::updateConfig(const PathCacheConfig& config) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    config_ = config;
}

void PathCache::performMaintenance() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    // 清理过期项
    evictExpired();
    
    // 如果仍然超过限制，执行LRU淘汰
    if (shouldEvict()) {
        evictLRU();
    }
    
    updateStats();
}

void PathCache::warmup(const LOUDSTrie& louds, 
                      const std::vector<size_t>& bfs_indices,
                      size_t max_items) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    
    size_t processed = 0;
    for (size_t bfs_idx : bfs_indices) {
        if (processed >= max_items) break;
        
        // 检查是否已缓存
        if (cache_.find(bfs_idx) != cache_.end()) {
            continue;
        }
        
        // 重建路径
        std::vector<size_t> bfs_path = louds.reconstructPathToRoot(bfs_idx);
        std::vector<NodeValue> path;
        path.reserve(bfs_path.size());
        
        for (size_t idx : bfs_path) {
            path.push_back(louds.getNodeValue(idx));
        }
        
        // 缓存路径
        if (put(bfs_idx, path)) {
            processed++;
        }
    }
}

// ========== 私有辅助方法 ==========

bool PathCache::shouldEvict() const {
    return cache_.size() >= config_.max_cache_items || 
           stats_.current_memory_usage >= config_.max_cache_size;
}

void PathCache::evictLRU() {
    if (cache_.empty()) return;
    
    // 找到最少访问的项
    auto min_it = std::min_element(cache_.begin(), cache_.end(),
        [](const auto& a, const auto& b) {
            return a.second->access_count < b.second->access_count ||
                   (a.second->access_count == b.second->access_count && 
                    a.second->timestamp < b.second->timestamp);
        });
    
    if (min_it != cache_.end()) {
        stats_.current_memory_usage -= min_it->second->memory_size;
        stats_.current_item_count--;
        cache_.erase(min_it);
        stats_.evictions++;
    }
}

void PathCache::evictExpired() {
    auto now = std::chrono::steady_clock::now();
    auto expired_threshold = now - config_.max_age;
    
    auto it = cache_.begin();
    while (it != cache_.end()) {
        if (it->second->timestamp < expired_threshold) {
            stats_.current_memory_usage -= it->second->memory_size;
            stats_.current_item_count--;
            it = cache_.erase(it);
            stats_.evictions++;
        } else {
            ++it;
        }
    }
}

void PathCache::updateStats() {
    stats_.updateHitRatio();
}

bool PathCache::isValidItem(const PathCacheItem& item) const {
    auto now = std::chrono::steady_clock::now();
    return (now - item.timestamp) < config_.max_age;
}

void PathCache::cleanup() {
    // 定期清理，移除过期项
    evictExpired();
    updateStats();
}

// ========== PathCacheManager 实现 ==========

PathCacheManager& PathCacheManager::getInstance() {
    static PathCacheManager instance;
    return instance;
}

std::shared_ptr<PathCache> PathCacheManager::getOrCreateCache(const std::string& cache_name,
                                                            const PathCacheConfig& config) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    auto it = caches_.find(cache_name);
    if (it != caches_.end()) {
        return it->second;
    }
    
    auto cache = std::make_shared<PathCache>(config);
    caches_[cache_name] = cache;
    return cache;
}

void PathCacheManager::removeCache(const std::string& cache_name) {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    caches_.erase(cache_name);
}

void PathCacheManager::clearAllCaches() {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for (auto& [name, cache] : caches_) {
        cache->clear();
    }
}

std::unordered_map<std::string, PathCacheStats> PathCacheManager::getAllStats() const {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    std::unordered_map<std::string, PathCacheStats> stats;
    for (const auto& [name, cache] : caches_) {
        stats[name] = cache->getStats();
    }
    
    return stats;
}

void PathCacheManager::performGlobalMaintenance() {
    std::lock_guard<std::mutex> lock(manager_mutex_);
    
    for (auto& [name, cache] : caches_) {
        cache->performMaintenance();
    }
}

} // namespace query
} // namespace json2
