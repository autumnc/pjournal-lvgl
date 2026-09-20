#pragma once

#include <cstdint>
#include <string>
#include <vector>

class LinuxIme {
public:
    using WidthFn = int (*)(const char *text);

    bool begin();
    bool active() const;
    bool composing() const;
    void toggle();
    void set_active(bool on);
    bool handle_key(int key, std::string &out);
    bool switch_schema();
    std::string composition() const;
    const std::vector<std::string> &candidates() const;
    // 当前输入法的显示名(雾凇拼音 / 永码 …),给状态栏显示首字用。内置输入法没有方案
    // 概念,返回空串(调用方退回「中」)。
    std::string schema_name() const;
    int current_page() const;
    int total_pages() const;
    int total_candidates() const;
    int page_size() const;
    void set_page_size(int n);
    int highlight_index() const;
    bool fullwidth() const;
    bool trad() const;
    bool english() const;
    void toggle_fullwidth();
    void toggle_trad();
    void toggle_english();
    void set_display_width(int px);
    void set_width_fn(WidthFn fn);
};

extern LinuxIme g_linux_ime;
