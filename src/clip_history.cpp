#include "clip_history.h"

#include <cstring>
#include <deque>
#include <mutex>

namespace {

struct ClipEntry {
    std::string data;
    bool linewise = false;
};

// deque 从头部增删不会搬动已有元素,网络剪贴板正在发一条大条目时插入新条目也安全。
std::deque<ClipEntry> g_entries;
std::mutex g_mutex;

// 截到上限,并保证不把多字节 UTF-8 字符劈开。
size_t capped_len(const char *data, size_t size) {
    if(size <= CLIPHIST_MAX_BYTES) return size;
    size_t n = CLIPHIST_MAX_BYTES;
    // 先退掉末尾的续字节;此时 n-1 是某个字符的首字节(或 ASCII)。
    while(n > 0 && ((unsigned char)data[n - 1] & 0xC0) == 0x80) --n;
    if(n == 0) return 0;
    unsigned char lead = (unsigned char)data[n - 1];
    size_t need = 1;
    if((lead & 0xE0) == 0xC0) need = 2;
    else if((lead & 0xF0) == 0xE0) need = 3;
    else if((lead & 0xF8) == 0xF0) need = 4;
    // 该字符在截断点内没有放全,连首字节一起丢掉。
    if(need > CLIPHIST_MAX_BYTES - (n - 1)) --n;
    return n;
}

void push_front_locked(const char *data, size_t n, bool linewise) {
    // 去重:内容相同就把旧条目移到最前,不重新分配。
    for(auto it = g_entries.begin(); it != g_entries.end(); ++it) {
        if(it->data.size() == n && memcmp(it->data.data(), data, n) == 0) {
            ClipEntry hit = std::move(*it);
            hit.linewise = linewise;
            g_entries.erase(it);
            g_entries.push_front(std::move(hit));
            return;
        }
    }
    if((int)g_entries.size() >= CLIPHIST_MAX_ENTRIES) g_entries.pop_back();
    ClipEntry e;
    e.data.assign(data, n);
    e.linewise = linewise;
    g_entries.push_front(std::move(e));
}

bool append_locked(const char *data, size_t len, bool linewise) {
    if(!data || len == 0) return false;
    const size_t n = capped_len(data, len);
    if(n == 0) return false;
    push_front_locked(data, n, linewise);
    return true;
}

}  // namespace

void cliphist_record(const std::string &text, bool linewise) {
    if(text.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    append_locked(text.data(), text.size(), linewise);
}

int cliphist_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (int)g_entries.size();
}

bool cliphist_get(int i, std::string &data, bool *linewise) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if(i < 0 || i >= (int)g_entries.size()) return false;
    const ClipEntry &e = g_entries[(size_t)i];
    data = e.data;
    if(linewise) *linewise = e.linewise;
    return true;
}

bool cliphist_add_raw(const char *data, size_t len, bool linewise) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return append_locked(data, len, linewise);
}

bool cliphist_replace_raw(int i, const char *data, size_t len) {
    if(!data || len == 0) return false;
    const size_t n = capped_len(data, len);
    if(n == 0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if(i < 0 || i >= (int)g_entries.size()) return false;
    g_entries[(size_t)i].data.assign(data, n);
    return true;
}

void cliphist_remove(int i) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if(i < 0 || i >= (int)g_entries.size()) return;
    g_entries.erase(g_entries.begin() + i);
}
