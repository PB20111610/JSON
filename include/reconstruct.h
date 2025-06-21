#pragma once
#include <string>
#include <vector>
#include "trie.h"
#include "dictionary.h"

namespace json2 {
// 重建JSON字符串（每行一个对象）
std::string reconstructJsonFromTrie(const Trie& trie, const Dictionary& dict);
} 