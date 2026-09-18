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
    std::string composition() const;
    const std::vector<std::string> &candidates() const;
    int current_page() const;
    int total_pages() const;
    int total_candidates() const;
    int page_size() const;
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
