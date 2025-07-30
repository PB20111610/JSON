#pragma once
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>

namespace json2 {

enum class FieldType {
    Int = 0,
    Double = 1,
    Bool = 2,
    String = 3,
    Timestamp = 4,
    LogType = 5,
    Null = 6,
    UnstructuredArray = 7   // 非结构化数组（序列化为字符串）
};

struct FieldKey {
    std::string name;
    FieldType type;
    
    bool operator==(const FieldKey& other) const {
        return name == other.name && type == other.type;
    }
    bool operator<(const FieldKey& other) const {
        return name < other.name || (name == other.name && type < other.type);
    }
};

// 字段列表的序列化/反序列化 - 更高效的批量操作
namespace field_utils {
    // 序列化字段列表（基础版本）
    inline void serializeFieldList(const std::vector<FieldKey>& fields, std::ostream& out) {
        size_t field_count = fields.size();
        out.write(reinterpret_cast<const char*>(&field_count), sizeof(field_count));
        
        for (const auto& field : fields) {
            size_t name_len = field.name.length();
            out.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
            out.write(field.name.c_str(), name_len);
            int type_int = static_cast<int>(field.type);
            out.write(reinterpret_cast<const char*>(&type_int), sizeof(type_int));
        }
    }
    
    // 反序列化字段列表（基础版本）
    inline void deserializeFieldList(std::vector<FieldKey>& fields, std::istream& in) {
        fields.clear();
        size_t field_count;
        in.read(reinterpret_cast<char*>(&field_count), sizeof(field_count));
        
        fields.reserve(field_count);
        for (size_t i = 0; i < field_count; ++i) {
            FieldKey field;
            size_t name_len;
            in.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
            field.name.resize(name_len);
            in.read(&field.name[0], name_len);
            int type_int;
            in.read(reinterpret_cast<char*>(&type_int), sizeof(type_int));
            field.type = static_cast<FieldType>(type_int);
            fields.push_back(field);
        }
    }
    
    // 序列化字段列表（压缩版本 - 去重字段名）
    inline void serializeFieldListCompressed(const std::vector<FieldKey>& fields, std::ostream& out) {
        // 收集所有唯一的字段名
        std::vector<std::string> unique_names;
        std::unordered_map<std::string, size_t> name_to_index;
        
        for (const auto& field : fields) {
            if (name_to_index.find(field.name) == name_to_index.end()) {
                name_to_index[field.name] = unique_names.size();
                unique_names.push_back(field.name);
            }
        }
        
        // 序列化唯一字段名列表
        size_t unique_count = unique_names.size();
        out.write(reinterpret_cast<const char*>(&unique_count), sizeof(unique_count));
        for (const auto& name : unique_names) {
            size_t name_len = name.length();
            out.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
            out.write(name.c_str(), name_len);
        }
        
        // 序列化字段列表（使用索引引用字段名）
        size_t field_count = fields.size();
        out.write(reinterpret_cast<const char*>(&field_count), sizeof(field_count));
        for (const auto& field : fields) {
            size_t name_index = name_to_index[field.name];
            out.write(reinterpret_cast<const char*>(&name_index), sizeof(name_index));
            int type_int = static_cast<int>(field.type);
            out.write(reinterpret_cast<const char*>(&type_int), sizeof(type_int));
        }
    }
    
    // 反序列化字段列表（压缩版本）
    inline void deserializeFieldListCompressed(std::vector<FieldKey>& fields, std::istream& in) {
        fields.clear();
        
        // 读取唯一字段名列表
        size_t unique_count;
        in.read(reinterpret_cast<char*>(&unique_count), sizeof(unique_count));
        std::vector<std::string> unique_names;
        unique_names.reserve(unique_count);
        
        for (size_t i = 0; i < unique_count; ++i) {
            size_t name_len;
            in.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
            std::string name(name_len, '\0');
            in.read(&name[0], name_len);
            unique_names.push_back(name);
        }
        
        // 读取字段列表
        size_t field_count;
        in.read(reinterpret_cast<char*>(&field_count), sizeof(field_count));
        fields.reserve(field_count);
        
        for (size_t i = 0; i < field_count; ++i) {
            FieldKey field;
            size_t name_index;
            in.read(reinterpret_cast<char*>(&name_index), sizeof(name_index));
            field.name = unique_names[name_index];
            int type_int;
            in.read(reinterpret_cast<char*>(&type_int), sizeof(type_int));
            field.type = static_cast<FieldType>(type_int);
            fields.push_back(field);
        }
    }
}

} // namespace json2

namespace std {
template<>
struct hash<json2::FieldKey> {
    std::size_t operator()(const json2::FieldKey& k) const {
        return std::hash<std::string>()(k.name) ^ (std::hash<int>()(static_cast<int>(k.type)) << 1);
    }
};
} 