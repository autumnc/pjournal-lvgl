#pragma once

#include <lvgl.h>

#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct FontInfo {
    std::string path;
    std::string name;
};

enum class FontStyle { Normal, Bold, Italic };

class FontManager {
public:
    void scan(const std::string &dir = "/root/.fonts");
    const std::vector<FontInfo> &fonts() const { return fonts_; }
    std::string default_font_path() const;
    bool supports_cjk(const std::string &path) const;
    bool supports_symbols(const std::string &path) const;
    lv_font_t *load(const std::string &path, int size, FontStyle style = FontStyle::Normal);
    lv_font_t *current() const { return current_; }
    int current_size() const { return current_size_; }
    std::string current_path() const { return current_path_; }
    ~FontManager();

private:
    lv_font_t *create(const std::string &path, int size, FontStyle style);
    lv_font_t *create_one(const std::string &path, int size, FontStyle style);
    std::vector<std::string> pick_fallbacks(const std::string &primary) const;
    std::vector<std::string> pick_wide(const std::string &primary) const;
    std::string pick_symbol(const std::string &primary) const;
    std::vector<FontInfo> fonts_;
    std::map<std::tuple<std::string, int, int>, lv_font_t *> loaded_;
    lv_font_t *current_ = nullptr;
    int current_size_ = 0;
    std::string current_path_;
    std::vector<std::string> fallback_chain_;
    bool fallback_resolved_ = false;
};

extern FontManager g_fonts;
