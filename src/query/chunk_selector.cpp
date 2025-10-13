#include "../include/query/chunk_selector.h"
#include <algorithm>
#include <numeric>

namespace json2 {
namespace query {

ChunkSelection ChunkSelector::selectChunks(const FieldAnalysis& analysis, 
                                          const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks) {
    ChunkSelection selection;
    selection.total_chunks = chunks.size();
    selection.required_fields = analysis.required_fields;
    
    // 提取所有块的元数据
    std::vector<ChunkMetadata> metadata_list;
    for (size_t i = 0; i < chunks.size(); ++i) {
        metadata_list.push_back(extractMetadata(i, chunks[i]));
    }
    
    // 应用选择策略
    selection.selected_chunks = applySelectionStrategy(analysis, metadata_list);
    selection.chunk_metadata = metadata_list;
    
    // 计算选择比例
    selection.selection_ratio = static_cast<double>(selection.selected_chunks.size()) / chunks.size();
    
    // 检查是否需要全扫描
    selection.needs_full_scan = (selection.selection_ratio > 0.8);
    
    // 优化选择
    optimizeSelection(selection, analysis);
    
    return selection;
}

ChunkSelection ChunkSelector::selectChunks(const FieldAnalysis& analysis,
                                          const std::vector<ChunkedTypeAwareBlock>& chunks) {
    ChunkSelection selection;
    selection.total_chunks = chunks.size();
    selection.required_fields = analysis.required_fields;
    
    // 提取所有块的元数据
    std::vector<ChunkMetadata> metadata_list;
    for (size_t i = 0; i < chunks.size(); ++i) {
        metadata_list.push_back(extractMetadata(i, chunks[i]));
    }
    
    // 应用选择策略
    selection.selected_chunks = applySelectionStrategy(analysis, metadata_list);
    selection.chunk_metadata = metadata_list;
    
    // 计算选择比例
    selection.selection_ratio = static_cast<double>(selection.selected_chunks.size()) / chunks.size();
    
    // 检查是否需要全扫描
    selection.needs_full_scan = (selection.selection_ratio > 0.8);
    
    // 优化选择
    optimizeSelection(selection, analysis);
    
    return selection;
}

std::vector<size_t> ChunkSelector::selectByFieldPresence(const std::unordered_set<std::string>& required_fields,
                                                        const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks) {
    std::vector<size_t> selected_chunks;
    
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto metadata = extractMetadata(i, chunks[i]);
        if (hasRequiredFields(metadata, required_fields)) {
            selected_chunks.push_back(i);
        }
    }
    
    return selected_chunks;
}

std::vector<size_t> ChunkSelector::selectByFieldPresence(const std::unordered_set<std::string>& required_fields,
                                                        const std::vector<ChunkedTypeAwareBlock>& chunks) {
    std::vector<size_t> selected_chunks;
    
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto metadata = extractMetadata(i, chunks[i]);
        if (hasRequiredFields(metadata, required_fields)) {
            selected_chunks.push_back(i);
        }
    }
    
    return selected_chunks;
}

std::vector<size_t> ChunkSelector::selectByTimestampRange(uint64_t min_timestamp, uint64_t max_timestamp,
                                                         const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks) {
    std::vector<size_t> selected_chunks;
    
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto metadata = extractMetadata(i, chunks[i]);
        if (isInTimestampRange(metadata, min_timestamp, max_timestamp)) {
            selected_chunks.push_back(i);
        }
    }
    
    return selected_chunks;
}

std::vector<size_t> ChunkSelector::selectByTimestampRange(uint64_t min_timestamp, uint64_t max_timestamp,
                                                         const std::vector<ChunkedTypeAwareBlock>& chunks) {
    std::vector<size_t> selected_chunks;
    
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto metadata = extractMetadata(i, chunks[i]);
        if (isInTimestampRange(metadata, min_timestamp, max_timestamp)) {
            selected_chunks.push_back(i);
        }
    }
    
    return selected_chunks;
}

std::vector<size_t> ChunkSelector::selectByFieldTypes(const std::unordered_map<std::string, FieldType>& field_types,
                                                     const std::vector<ChunkedTrieCompressor::ChunkedBlock>& chunks) {
    std::vector<size_t> selected_chunks;
    
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto metadata = extractMetadata(i, chunks[i]);
        bool matches = true;
        
        for (const auto& field_type : field_types) {
            if (!hasFieldType(metadata, field_type.first, field_type.second)) {
                matches = false;
                break;
            }
        }
        
        if (matches) {
            selected_chunks.push_back(i);
        }
    }
    
    return selected_chunks;
}

std::vector<size_t> ChunkSelector::selectByFieldTypes(const std::unordered_map<std::string, FieldType>& field_types,
                                                     const std::vector<ChunkedTypeAwareBlock>& chunks) {
    std::vector<size_t> selected_chunks;
    
    for (size_t i = 0; i < chunks.size(); ++i) {
        auto metadata = extractMetadata(i, chunks[i]);
        bool matches = true;
        
        for (const auto& field_type : field_types) {
            if (!hasFieldType(metadata, field_type.first, field_type.second)) {
                matches = false;
                break;
            }
        }
        
        if (matches) {
            selected_chunks.push_back(i);
        }
    }
    
    return selected_chunks;
}

ChunkMetadata ChunkSelector::extractMetadata(size_t chunk_id, const ChunkedTrieCompressor::ChunkedBlock& chunk) {
    ChunkMetadata metadata;
    metadata.chunk_id = chunk_id;
    metadata.schema = chunk.field_order;
    metadata.record_count = 0; // 需要从Trie中计算
    metadata.compressed_size = 0; // 需要从压缩数据中获取
    metadata.compression_ratio = 0.0;
    metadata.has_timestamp = false;
    metadata.has_template = false;
    metadata.min_timestamp = 0;
    metadata.max_timestamp = 0;
    
    // 提取字段信息
    for (const auto& field : chunk.field_order) {
        metadata.fields.insert(field.name);
        metadata.field_types[field.name] = field.type;
        
        // 检查时间戳字段
        if (field.type == FieldType::Timestamp) {
            metadata.has_timestamp = true;
        }
        
        // 检查模板字段
        if (field.type == FieldType::LogType) {
            metadata.has_template = true;
        }
    }
    
    return metadata;
}

ChunkMetadata ChunkSelector::extractMetadata(size_t chunk_id, const ChunkedTypeAwareBlock& chunk) {
    ChunkMetadata metadata;
    metadata.chunk_id = chunk_id;
    metadata.schema = chunk.field_order;
    metadata.record_count = 0; // 需要从Trie中计算
    metadata.compressed_size = 0; // 需要从压缩数据中获取
    metadata.compression_ratio = 0.0;
    metadata.has_timestamp = false;
    metadata.has_template = false;
    metadata.min_timestamp = 0;
    metadata.max_timestamp = 0;
    
    // 提取字段信息
    for (const auto& field : chunk.field_order) {
        metadata.fields.insert(field.name);
        metadata.field_types[field.name] = field.type;
        
        // 检查时间戳字段
        if (field.type == FieldType::Timestamp) {
            metadata.has_timestamp = true;
        }
        
        // 检查模板字段
        if (field.type == FieldType::LogType) {
            metadata.has_template = true;
        }
    }
    
    return metadata;
}

bool ChunkSelector::hasRequiredFields(const ChunkMetadata& metadata, 
                                     const std::unordered_set<std::string>& required_fields) const {
    for (const auto& field : required_fields) {
        if (metadata.fields.find(field) == metadata.fields.end()) {
            return false;
        }
    }
    return true;
}

bool ChunkSelector::hasFieldType(const ChunkMetadata& metadata, 
                                const std::string& field_name, FieldType field_type) const {
    auto it = metadata.field_types.find(field_name);
    return it != metadata.field_types.end() && it->second == field_type;
}

bool ChunkSelector::isInTimestampRange(const ChunkMetadata& metadata, 
                                      uint64_t min_timestamp, uint64_t max_timestamp) const {
    if (!metadata.has_timestamp) {
        return false;
    }
    
    // 检查时间戳范围是否重叠
    return !(metadata.max_timestamp < min_timestamp || metadata.min_timestamp > max_timestamp);
}

std::vector<size_t> ChunkSelector::applySelectionStrategy(const FieldAnalysis& analysis,
                                                         const std::vector<ChunkMetadata>& metadata_list) {
    std::vector<size_t> selected_chunks;
    
    // 策略1：基于字段存在性选择
    if (!analysis.required_fields.empty()) {
        for (size_t i = 0; i < metadata_list.size(); ++i) {
            if (hasRequiredFields(metadata_list[i], analysis.required_fields)) {
                selected_chunks.push_back(i);
            }
        }
    } else {
        // 如果没有特定字段要求，选择所有块
        for (size_t i = 0; i < metadata_list.size(); ++i) {
            selected_chunks.push_back(i);
        }
    }
    
    // 策略2：基于字段类型进一步筛选
    if (!analysis.field_types.empty()) {
        std::vector<size_t> type_filtered_chunks;
        for (size_t chunk_id : selected_chunks) {
            bool matches_types = true;
            for (const auto& field_type : analysis.field_types) {
                if (!hasFieldType(metadata_list[chunk_id], field_type.first, field_type.second)) {
                    matches_types = false;
                    break;
                }
            }
            if (matches_types) {
                type_filtered_chunks.push_back(chunk_id);
            }
        }
        selected_chunks = type_filtered_chunks;
    }
    
    // 策略3：基于时间戳范围筛选
    if (!analysis.timestamp_fields.empty()) {
        // 简化：若存在时间戳字段，则优先保留包含时间戳的块
        std::vector<size_t> ts_filtered;
        for (size_t chunk_id : selected_chunks) {
            if (metadata_list[chunk_id].has_timestamp) {
                ts_filtered.push_back(chunk_id);
            }
        }
        if (!ts_filtered.empty()) {
            selected_chunks.swap(ts_filtered);
        }
    }
    
    return selected_chunks;
}

void ChunkSelector::optimizeSelection(ChunkSelection& selection, const FieldAnalysis& analysis) {
    // 如果选择的块太多，考虑优化策略
    if (selection.selection_ratio > 0.5) {
        // 可以按压缩比排序，优先选择压缩比高的块
        std::sort(selection.selected_chunks.begin(), selection.selected_chunks.end(),
                 [&](size_t a, size_t b) {
                     return selection.chunk_metadata[a].compression_ratio > 
                            selection.chunk_metadata[b].compression_ratio;
                 });
        
        // 限制选择的块数量
        if (selection.selected_chunks.size() > 10) {
            selection.selected_chunks.resize(10);
            selection.selection_ratio = static_cast<double>(selection.selected_chunks.size()) / 
                                       selection.total_chunks;
        }
    }
    
    // 如果选择的块太少，可能需要全扫描
    if (selection.selection_ratio < 0.01) {
        selection.needs_full_scan = true;
    }
}

} // namespace query
} // namespace json2
