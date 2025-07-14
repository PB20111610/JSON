#pragma once
#include <string>
#include <vector>
#include "trie.h"
#include "field_dictionary_manager.h"

namespace json2 {
// 重建JSON字符串（每行一个对象）
std::string reconstructJsonFromTrie(const Trie& trie, const FieldDictionaryManager& manager);
} 