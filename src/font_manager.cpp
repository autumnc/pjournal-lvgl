#include "font_manager.h"

#include <lvgl.h>
#include <src/libs/freetype/lv_freetype.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>

FontManager g_fonts;

static bool font_ext(const std::string &p) {
    size_t dot = p.rfind('.');
    if(dot == std::string::npos) return false;
    std::string e = p.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)tolower(c); });
    return e == ".ttf" || e == ".otf" || e == ".ttc";
}

void FontManager::scan(const std::string &dir) {
    fonts_.clear();
    std::vector<std::string> dirs{dir};
    for(size_t di = 0; di < dirs.size(); ++di) {
        DIR *d = opendir(dirs[di].c_str());
        if(!d) continue;
        dirent *de = nullptr;
        while((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if(name.empty() || name[0] == '.') continue;
            std::string path = dirs[di] + "/" + name;
            struct stat st {};
            if(stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                dirs.push_back(path);
            } else if(font_ext(path)) {
                fonts_.push_back({path, name});
            }
        }
        closedir(d);
    }
    std::sort(fonts_.begin(), fonts_.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
}

std::string FontManager::default_font_path() const {
    for(const auto &f : fonts_) {
        if(supports_cjk(f.path)) return f.path;
    }
    if(!fonts_.empty()) return fonts_.front().path;
    return "";
}

static bool face_covers_any(const std::string &path, const uint32_t *codepoints, size_t count) {
    if(path.empty()) return false;
    FT_Library lib = nullptr;
    if(FT_Init_FreeType(&lib) != 0) return false;
    FT_Face face = nullptr;
    bool ok = false;
    if(FT_New_Face(lib, path.c_str(), 0, &face) == 0) {
        for(size_t i = 0; i < count && !ok; ++i) ok = FT_Get_Char_Index(face, codepoints[i]) != 0;
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
    return ok;
}

bool FontManager::supports_cjk(const std::string &path) const {
    static const uint32_t probe[] = {0x4E2D, 0x3002};  // 中 。
    return face_covers_any(path, probe, 2);
}

// A fallback font only earns its keep if it carries shapes the text fonts
// drop; U+2713 CHECK MARK is the one users actually notice going missing.
bool FontManager::supports_symbols(const std::string &path) const {
    static const uint32_t probe[] = {0x2713, 0x2714, 0x2717};  // ✓ ✔ ✗
    return face_covers_any(path, probe, 3);
}

static bool nerd_font_name(const std::string &name) {
    std::string lower;
    lower.reserve(name.size());
    for(char c : name) lower += (char)tolower((unsigned char)c);
    return lower.find("nerd") != std::string::npos || lower.find("nf-") != std::string::npos ||
           lower.find("-nf") != std::string::npos;
}

std::string FontManager::pick_fallback(const std::string &primary) {
    std::string best;
    long best_size = 0;
    bool best_nerd = false;
    for(const auto &f : fonts_) {
        if(f.path == primary || !supports_symbols(f.path)) continue;
        struct stat st {};
        long size = stat(f.path.c_str(), &st) == 0 ? (long)st.st_size : 0;
        bool nerd = nerd_font_name(f.name);
        if(best.empty() || (nerd != best_nerd ? nerd : size < best_size)) {
            best = f.path;
            best_size = size;
            best_nerd = nerd;
        }
    }
    return best;
}

lv_font_t *FontManager::create(const std::string &path, int size, FontStyle style) {
    if(path.empty()) return nullptr;
    auto key = std::make_tuple(path, size, (int)style);
    auto it = loaded_.find(key);
    if(it != loaded_.end()) return it->second;
    lv_freetype_font_style_t fs = LV_FREETYPE_FONT_STYLE_NORMAL;
    if(style == FontStyle::Bold) fs = LV_FREETYPE_FONT_STYLE_BOLD;
    else if(style == FontStyle::Italic) fs = LV_FREETYPE_FONT_STYLE_ITALIC;
    lv_font_t *font = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                            (uint32_t)size, fs);
    if(!font) return nullptr;
    loaded_[key] = font;
    if(!fallback_resolved_) {
        fallback_resolved_ = true;
        fallback_path_ = pick_fallback(path);
    }
    // LVGL walks lv_font_t::fallback once the primary font reports a codepoint
    // as a placeholder, which FreeType does for glyph index 0. Chaining a
    // symbol font keeps text the chosen font lacks (e.g. U+2713) visible
    // instead of blank. 挂在 create 里,三种 style 的字体对象才都有回退。
    if(!fallback_path_.empty() && fallback_path_ != path) {
        lv_font_t *fb = create(fallback_path_, size, FontStyle::Normal);
        if(fb && fb != font) font->fallback = fb;
    }
    return font;
}

lv_font_t *FontManager::load(const std::string &path, int size, FontStyle style) {
    lv_font_t *font = create(path, size, style);
    if(!font) return nullptr;
    current_ = font;
    current_path_ = path;
    current_size_ = size;
    return current_;
}

FontManager::~FontManager() {
    for(auto &p : loaded_) {
        if(p.second) lv_freetype_font_delete(p.second);
    }
}
