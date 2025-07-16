#pragma once
#include <string>

namespace json2 {

enum class FieldType {
    Int = 0,
    Double = 1,
    Bool = 2,
    String = 3,
    Timestamp = 4,
    LogType = 5,
    Null = 6
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

} // namespace json2

namespace std {
template<>
struct hash<json2::FieldKey> {
    std::size_t operator()(const json2::FieldKey& k) const {
        return std::hash<std::string>()(k.name) ^ (std::hash<int>()(static_cast<int>(k.type)) << 1);
    }
};
} 