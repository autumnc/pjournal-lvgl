#include "term_settings.h"

#include "safe_file.h"

#include <cctype>
#include <cstdlib>
#include <pwd.h>
#include <sstream>
#include <unistd.h>

namespace term {

namespace {

TermSettings g_settings;

std::string home_dir() {
    if(const char *home = getenv("HOME"); home && *home) return home;
    if(struct passwd *pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while(b < e && isspace((unsigned char)s[b])) ++b;
    while(e > b && isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

int clamp_int(int v, int lo, int hi) {
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

const char *style_name(int style) {
    switch(style) {
    case 1: return "underline";
    case 2: return "bar";
    default: return "block";
    }
}

// 认 word 也认数字 —— 文件是给人看的,但手滑写成 1 也该能认。
int parse_style(const std::string &v, int fallback) {
    if(v == "block") return 0;
    if(v == "underline") return 1;
    if(v == "bar") return 2;
    char *end = nullptr;
    const long n = strtol(v.c_str(), &end, 10);
    if(end == v.c_str()) return fallback;  // 既不是已知的词也不是数字
    return clamp_int((int)n, 0, 2);
}

bool truthy(const std::string &v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

// 套一个键。**未知的键直接忽略** —— 文件可能是新版本写的,或者用户手写注释时
// 打错了个词,不该因此整份设置作废。
void apply(TermSettings &s, const std::string &key, const std::string &val) {
    if(key == "font") s.font_path = val;
    else if(key == "font_bold") s.font_bold_path = val;
    else if(key == "font_italic") s.font_italic_path = val;
    else if(key == "font_size") s.font_size = clamp_int(atoi(val.c_str()), 8, 200);
    else if(key == "ui_font_size") s.ui_font_size = clamp_int(atoi(val.c_str()), 0, 200);
    else if(key == "theme") s.theme_id = val;
    else if(key == "cursor_style") s.cursor_style = parse_style(val, 0);
    else if(key == "cursor_blink") s.cursor_blink = truthy(val);
    else if(key == "scrollback") s.scrollback = clamp_int(atoi(val.c_str()), 50, 5000);
}

}  // namespace

TermSettings &term_settings() { return g_settings; }

std::string settings_path() {
    if(const char *p = getenv("PJOURNAL_TERMINAL_CONFIG"); p && *p) return p;
    return home_dir() + "/.pjournal-lvgl/settings/terminal.conf";
}

bool settings_load(const std::string &path) {
    // 先复位:文件里没写到的键必须是默认值,而不是上一次 load 的残留。
    g_settings = TermSettings{};

    const std::string text = read_whole_file(path);
    if(text.empty()) return false;

    std::istringstream in(text);
    std::string line;
    while(std::getline(in, line)) {
        if(!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if(t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if(eq == std::string::npos) continue;
        apply(g_settings, trim(t.substr(0, eq)), trim(t.substr(eq + 1)));
    }
    return true;
}

bool settings_save(const std::string &path) {
    const TermSettings &s = g_settings;
    std::ostringstream out;
    out << "# pjournal 终端设置。界面里改的就是这份,也可以直接编辑。\n"
        << "# 空值 = 用默认(字体留空就是 ~/.fonts/Go-LavaPropo-NF-R.ttf,\n"
        << "# 粗体/斜体留空就用正文那份合成)。\n";
    out << "font=" << s.font_path << "\n";
    out << "font_bold=" << s.font_bold_path << "\n";
    out << "font_italic=" << s.font_italic_path << "\n";
    out << "font_size=" << s.font_size << "\n";
    out << "ui_font_size=" << s.ui_font_size << "\n";
    out << "theme=" << s.theme_id << "\n";
    out << "cursor_style=" << style_name(s.cursor_style) << "\n";
    out << "cursor_blink=" << (s.cursor_blink ? 1 : 0) << "\n";
    out << "scrollback=" << s.scrollback << "\n";
    return safe_write_file(path, out.str());
}

}  // namespace term
