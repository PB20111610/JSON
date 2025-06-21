#pragma once

#include <string>
#include <vector>
#include <memory>
#include "dictionary.h"

namespace json2 {

class JsonParser {
public:
    // 解析记录集合，填充字典并返回有序字段
    static void analyzeAndSortFields(const std::vector<std::string>& records, Dictionary& dict, std::vector<std::string>& ordered_fields);
};

} // namespace json2
