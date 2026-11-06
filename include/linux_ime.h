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

    // —— 内置输入法新能力(fcitx5/yong 后端返回空串/false/无操作) ——
    std::string mode_label() const;      // "[删]" / "[联]" / "[两]"
    bool delete_mode() const;
    bool predicting() const;
    bool letter_select_armed() const;    // 字母选字待选(候选编号画 q./w./e. …)
    std::string letter_sel_keys() const; // 1..9 对应的字母,内置后端返回 "qwertyuio"
    std::string take_status_message();   // 取走一次性状态提示("已删除 xx" 等)
    void set_document_context(const std::string &text);
    void handle_host_backspace();
    void toggle_delete_mode();
    void reload_settings();
    // 空闲/退出时把用户词典落盘(按键路径只标脏,见 IME::flushUserDictSavesIdle)
    void flush_user_dict_idle();
    void flush_user_dict_now();
};

extern LinuxIme g_linux_ime;
