#pragma once

#include <lvgl.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

struct FontInfo {
    std::string path;
    std::string name;
};

class FontManager {
public:
    void scan(const std::string &dir = "/root/.fonts");
    const std::vector<FontInfo> &fonts() const { return fonts_; }
    std::string default_font_path() const;
    bool supports_cjk(const std::string &path) const;
    bool supports_symbols(const std::string &path) const;
    lv_font_t *load(const std::string &path, int size);
    lv_font_t *current() const { return current_; }
    int current_size() const { return current_size_; }
    std::string current_path() const { return current_path_; }
    std::string fallback_path() const { return fallback_path_; }
    ~FontManager();

private:
    lv_font_t *create(const std::string &path, int size);
    std::string pick_fallback(const std::string &primary);
    std::vector<FontInfo> fonts_;
    std::map<std::pair<std::string, int>, lv_font_t *> loaded_;
    lv_font_t *current_ = nullptr;
    int current_size_ = 0;
    std::string current_path_;
    std::string fallback_path_;
    bool fallback_resolved_ = false;
};

extern FontManager g_fonts;
