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

// 这个字体认下 probes 里几个码位。挑回退字体要靠「认下几个」来排序,
// 不是简单的是/否——同一档里覆盖广的该排在前面。
static int face_cover_count(const std::string &path, const uint32_t *codepoints, size_t count) {
    if(path.empty()) return 0;
    FT_Library lib = nullptr;
    if(FT_Init_FreeType(&lib) != 0) return 0;
    FT_Face face = nullptr;
    int hits = 0;
    if(FT_New_Face(lib, path.c_str(), 0, &face) == 0) {
        for(size_t i = 0; i < count; ++i) {
            if(FT_Get_Char_Index(face, codepoints[i]) != 0) ++hits;
        }
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
    return hits;
}

static bool face_covers_any(const std::string &path, const uint32_t *codepoints, size_t count) {
    return face_cover_count(path, codepoints, count) > 0;
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

// 生僻字探针。CJK 扩展区横跨好几个平面,字体厂商也就按平面分文件——设备上的
// 中华书宋就是 Plane02 管 02 平面、Plane00 管 03 平面。一个文件补不全,所以按
// 平面分组,每组各挑一个:组内认下的探针多的优先,一样多取文件小的。
static const uint32_t kRarePlane2[] = {
    0x20000,  // 扩展 B
    0x2A700,  // 扩展 C
    0x2B740,  // 扩展 D
    0x2EBF0,  // 扩展 I
    0x2F800,  // 兼容汉字补充
};
static const uint32_t kRarePlane3[] = {
    0x30000,  // 扩展 G
    0x31350,  // 扩展 H
};

// 拆字输入法的字表里大半是这些字(设备上 radical_pinyin 的 59020 条编码,43973 条
// 的候选含非 BMP 字,其中 02 平面 7 万条、03 平面 1.5 万条),候选条漏了回退就是
// 一片豆腐。
std::vector<std::string> FontManager::pick_wide(const std::string &primary) const {
    static const uint32_t *const groups[] = {kRarePlane2, kRarePlane3};
    static const size_t group_n[] = {sizeof kRarePlane2 / sizeof *kRarePlane2,
                                     sizeof kRarePlane3 / sizeof *kRarePlane3};
    std::vector<std::string> picked;
    for(size_t g = 0; g < sizeof groups / sizeof *groups; ++g) {
        std::string best;
        int best_hits = 0;
        long best_size = 0;
        for(const auto &f : fonts_) {
            if(f.path == primary) continue;
            bool used = false;
            for(const auto &p : picked) used = used || p == f.path;
            if(used) continue;
            int hits = face_cover_count(f.path, groups[g], group_n[g]);
            if(hits == 0) continue;
            struct stat st {};
            long size = stat(f.path.c_str(), &st) == 0 ? (long)st.st_size : 0;
            if(best.empty() || hits > best_hits || (hits == best_hits && size < best_size)) {
                best = f.path;
                best_hits = hits;
                best_size = size;
            }
        }
        if(!best.empty()) picked.push_back(best);
    }
    return picked;
}

std::string FontManager::pick_symbol(const std::string &primary) const {
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

// 回退链按「先补生僻字、后补符号」排。每一级挑不到就少挂一跳,全挑不到返回
// 空链,create() 就一个 fallback 也不挂(和没有回退一个样)。
std::vector<std::string> FontManager::pick_fallbacks(const std::string &primary) const {
    std::vector<std::string> chain = pick_wide(primary);
    std::string sym = pick_symbol(primary);
    if(!sym.empty()) {
        bool used = false;
        for(const auto &p : chain) used = used || p == sym;
        if(!used) chain.push_back(sym);
    }
    return chain;
}

lv_font_t *FontManager::create(const std::string &path, int size, FontStyle style) {
    if(path.empty()) return nullptr;
    if(!fallback_resolved_) {
        fallback_resolved_ = true;
        fallback_chain_ = pick_fallbacks(path);
    }
    lv_font_t *font = create_one(path, size, style);
    if(!font) return nullptr;
    // 整条链在这里一次挂完,create_one 自己不再往下挂:两跳互相指(宽字库
    // 指向符号字体、符号字体又指回去)的话,lv_font_get_glyph_dsc 那个
    // while(f) f = f->fallback 就转不出来了。倒着串,先把链尾接好。
    lv_font_t *tail = nullptr;
    for(size_t i = fallback_chain_.size(); i-- > 0; ) {
        const std::string &p = fallback_chain_[i];
        if(p == path) continue;
        lv_font_t *fb = create_one(p, size, FontStyle::Normal);
        if(!fb || fb == font) continue;
        fb->fallback = tail;
        tail = fb;
    }
    // tail 为空(没有可挂的回退)时别动 font->fallback:粗体/斜体那两跳是
    // app_ui_reload_font 挂的,不能被这里抹掉。
    if(tail) font->fallback = tail;
    return font;
}

lv_font_t *FontManager::create_one(const std::string &path, int size, FontStyle style) {
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
