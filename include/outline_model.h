#pragma once

#include "json_parser.h"

#include <set>
#include <string>
#include <vector>

// 大纲节点数组上的纯逻辑:树形前缀、折叠/筛选、文件名安全化。
// 与 UI/全局状态解耦,便于单独单元测试。
// nodes 为 { level, title, tags, ... } 的对象数组。

// 保证 data 至少含 nodes/bookmarks/tags 三个数组字段。
void outline_normalize_data(JsonValue &data);

bool outline_has_children(const JsonValue &nodes, int idx);
bool outline_is_last_at_level(const JsonValue &nodes, int idx, int lvl);

// 树形前缀:每层祖先是否为同级最后一个决定画竖线还是空格。
std::string outline_tree_prefix(const JsonValue &nodes, int idx);

// 折叠 + 标签筛选 + 文本筛选后的可见节点下标。
std::vector<int> outline_filter_nodes(const JsonValue &nodes, const std::set<int> &folded,
                                      const std::vector<std::string> &filterTags,
                                      const std::string &filterText);

// 标题 → 安全文件名(替换非法字符、限长 40 字节并保持 UTF-8 边界、补 .txt)。
std::string outline_safe_filename(const std::string &title);
