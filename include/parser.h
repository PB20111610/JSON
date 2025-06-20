#pragma once

#include <string>
#include <vector>
#include <memory>
#include "dictionary.h"

namespace json2 {

class JsonParser {
public:
    // 解析并收集字段信息，统计并排序字段，输出字段顺序
    static void parseAndCollect(const std::string& filename, Dictionary& dict, std::vector<std::string>& ordered_fields);
    // 解析日志文件，返回所有记录
    static std::vector<std::shared_ptr<void>> parseLogFile(const std::string& filename);
};

} // namespace json2
