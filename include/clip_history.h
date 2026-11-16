#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// 剪贴板历史:最多 20 条,单条 64 KB,只存在内存里(重启清空)。
// 新条目在 0 号;剪贴板写入由 clipboard_set() 统一调用 cliphist_record()。
#define CLIPHIST_MAX_ENTRIES 20
#define CLIPHIST_MAX_BYTES (64u * 1024u)

void cliphist_record(const std::string &text, bool linewise);
int cliphist_count();
// 拷出一份条目:历史会被 UI 线程和网络剪贴板线程同时改,直接返回内部 c_str()
// 会在解锁后失效(其它线程替换/删除该条时变成悬垂指针)。越界返回 false。
bool cliphist_get(int i, std::string &data, bool *linewise = nullptr);
void cliphist_remove(int i);  // 越界 no-op

// 网络剪贴板用:直接以原始缓冲区增/改条目,避免 std::string 中转拷贝。
// 返回 false 表示内容为空/超长截断为空/内存不足。
bool cliphist_add_raw(const char *data, size_t len, bool linewise);
bool cliphist_replace_raw(int i, const char *data, size_t len);
