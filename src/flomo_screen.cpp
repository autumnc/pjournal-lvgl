// Flomo 笔记的界面层。
//
// 原 C1ancher/Flomo 的 ui.cpp 是 296×152 的字符画渲染器,这里整套重写成 LVGL:
// 列表改成卡片式两行预览,正文/编辑是真文本控件。数据结构和同步流程(data 层)
// 是逐字移植过来的,不含在这里。
//
// 主题表、状态栏右侧(时间/WiFi/电池)、背光浮层这几块和 Launcher/src/
// launcher_ui.cpp 是同一份东西:pjournal 那边也有一份,主题读的是共享设置
// (~/.pjournal-lvgl/settings/theme),一改要一起变,所以只能抄。
#include "flomo_screen.h"

#include <lvgl.h>

#include "app_ui.h"
#include "backlight.h"
#include "flomo_api.h"
#include "flomo_db.h"
#include "flomo_model.h"
#include "flomo_sync.h"
#include "font_manager.h"
#include "key_codes.h"
#include "linux_ime.h"
#include "safe_file.h"
#include "settings.h"
#include "wifi_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <functional>
#include <net/if.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// LVGL 的折行量和「字符下标 ↔ 字节下标」换算都不在公开头里,但编辑器叠加层要自己
// 排版,非用不可(pjournal 的 app_ui.cpp 也是这么直接引的)。
#include <src/misc/lv_text_private.h>

// ===========================================================================
// 主题:和 pjournal / Launcher 是同一张表
// ===========================================================================

struct Theme {
    lv_color_t bg;
    lv_color_t fg;
    lv_color_t muted;
    lv_color_t accent;
    lv_color_t heading;
};

struct ThemeDef {
    const char *id;
    const char *label;
    uint32_t bg, fg, muted, accent, heading;
};

static const ThemeDef k_themes[] = {
    {"dark", "黑底白字", 0x000000, 0xffffff, 0xaaaaaa, 0xffffff, 0x7fb4ff},
    {"light", "白底黑字", 0xffffff, 0x000000, 0x555555, 0x111111, 0x1f4e79},
    {"contrast", "高对比", 0x000000, 0xffffff, 0xc8c8c8, 0xffffff, 0xffffff},
    {"sepia", "纸质 Sepia", 0xf4ecd8, 0x4a3f35, 0x9c8b70, 0x8a6b3f, 0x6b4a24},
    {"solarized_dark", "Solarized 暗", 0x002b36, 0x839496, 0x586e75, 0x268bd2, 0x2aa198},
    {"solarized_light", "Solarized 亮", 0xfdf6e3, 0x657b83, 0x93a1a1, 0x268bd2, 0x268bd2},
    {"gruvbox_dark", "Gruvbox 暗", 0x282828, 0xebdbb2, 0x928374, 0xfe8019, 0x83a598},
    {"gruvbox_light", "Gruvbox 亮", 0xfbf1c7, 0x3c3836, 0x928374, 0xaf3a03, 0x076678},
    {"nord", "Nord", 0x2e3440, 0xd8dee9, 0x6c7a96, 0x88c0d0, 0x81a1c1},
    {"dracula", "Dracula", 0x282a36, 0xf8f8f2, 0x6272a4, 0xbd93f9, 0xff79c6},
};

static const ThemeDef *theme_def(const std::string &id) {
    for(const ThemeDef &t : k_themes)
        if(id == t.id) return &t;
    return &k_themes[0];
}

static Theme g_theme;
static lv_style_t g_style;
static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_status = nullptr;
static lv_obj_t *g_status_right = nullptr;
// 四支字体:界面一套(标题栏/列表/状态栏),编辑器一套(编辑器/输入框/详情正文)。
// 两套各自有「正文 + 粗体」一对,字号各读各的设置。
static lv_font_t *g_ui_font = nullptr;           // 界面,flomo_ui_font_size
static lv_font_t *g_bold_font = nullptr;         // 界面粗体(标题栏)
static lv_font_t *g_editor_font = nullptr;       // 编辑器/输入框/详情正文,flomo_font_size
static lv_font_t *g_editor_bold_font = nullptr;  // 编辑器里的 **粗体**

// 用户加进 Go-LavaPropo-NF-R.ttf 的 flomo 图标(U+F1AF1,补充平面,UTF-8 F3 B1 AB B1)。
// 粗体字体 Go-Lava-NF-B.ttf 里没有这个字形,标题栏又是粗体 —— 靠 g_bold_font->fallback
// 指回正文字体取到它;fallback 是逐字形的,所以标题里中英文照旧走粗体。
static const char *kIconFlomo = "\xF3\xB1\xAB\xB1";

// 运行时确认字形:用户字体已经补了 U+F1AF1;万一设备上还是旧字体,回退到
// U+F249(便签),再回退 LV_SYMBOL_FILE,标题上不留豆腐块。
static const char *flomo_icon() {
    const lv_font_t *font = g_ui_font ? g_ui_font : LV_FONT_DEFAULT;
    lv_font_glyph_dsc_t dsc {};
    if(lv_font_get_glyph_dsc(font, &dsc, 0xF1AF1, 0)) return kIconFlomo;
    if(lv_font_get_glyph_dsc(font, &dsc, 0xF249, 0)) return "\xef\x89\x89";
    return LV_SYMBOL_FILE;
}

static void apply_theme_values() {
    const ThemeDef &t = *theme_def(g_settings.theme());
    g_theme = {lv_color_hex(t.bg), lv_color_hex(t.fg), lv_color_hex(t.muted),
               lv_color_hex(t.accent), lv_color_hex(t.heading)};
}

// ===========================================================================
// 界面尺寸 / 控件原语
// ===========================================================================
// 写死的像素值是照 40 号界面字量出来的;设备是 1024×600,默认界面 34 / 正文 30,
// 字号一改,纵向尺寸一律过 ui_px() 按同比例换算。横向不缩放 —— 屏宽是定死的。

static constexpr int k_ui_ref_font = 40;

// 浮墨的字号/字体存自己一套键(flomo_ 前缀),不碰 pjournal 和启动器共用的
// ui_font_size / font_size / font_file —— 启动器 launcher_ui.cpp 也读 ui_font_size,
// 共用的话在浮墨里调一次字号会把启动器的网格一起放大。目录还是同一个
// (~/.pjournal-lvgl/settings/,一键一文件),只是键名不一样。
static int setting_int(const char *key, int def, int lo, int hi) {
    int n = atoi(g_settings.get(key, std::to_string(def)).c_str());
    if(n < lo) n = lo;
    if(n > hi) n = hi;
    return n;
}

static int ui_font_size() { return setting_int("flomo_ui_font_size", 34, 24, 60); }
static int editor_font_size() { return setting_int("flomo_font_size", 30, 16, 72); }
static std::string font_file_setting() { return g_settings.get("flomo_font_file", ""); }
static std::string font_bold_setting() { return g_settings.get("flomo_font_bold_file", ""); }

static int ui_px(int v) {
    if(v == 0 || v == LV_SIZE_CONTENT) return v;
    int n = v * ui_font_size();
    return v > 0 ? (n + k_ui_ref_font / 2) / k_ui_ref_font
                 : (n - k_ui_ref_font / 2) / k_ui_ref_font;
}

static int ui_line_h() {
    int lh = lv_font_get_line_height(g_ui_font ? g_ui_font : LV_FONT_DEFAULT);
    return lh < 12 ? 12 : lh;
}

// 纵坐标已经是实际像素(从 chrome_bottom()/screen_h() 算出来的)时用 label_raw,
// 它不再缩放,否则会和调用方的计算叠一层。
static lv_obj_t *label_raw(lv_obj_t *parent, const std::string &text, int x, int y,
                           int w = LV_SIZE_CONTENT, int h = LV_SIZE_CONTENT) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text.c_str());
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(obj, x, y);
    if(h != LV_SIZE_CONTENT) {
        int min_h = ui_line_h() + 2;
        if(h < min_h) h = min_h;
    }
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_color(obj, g_theme.fg, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static int screen_w() {
    lv_display_t *disp = lv_display_get_default();
    return disp ? lv_display_get_horizontal_resolution(disp) : 1024;
}

static int screen_h() {
    lv_display_t *disp = lv_display_get_default();
    return disp ? lv_display_get_vertical_resolution(disp) : 600;
}

static lv_obj_t *box_raw(lv_obj_t *parent, int x, int y, int w, int h, bool selected = false) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(obj, g_theme.fg, 0);
    lv_obj_set_style_bg_color(obj, selected ? g_theme.fg : g_theme.bg, 0);
    lv_obj_set_style_text_color(obj, selected ? g_theme.bg : g_theme.fg, 0);
    lv_obj_set_style_pad_all(obj, 4, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void set_status_left(const std::string &left) {
    if(g_status) lv_label_set_text(g_status, left.c_str());
}

static int status_bar_h() {
    int lh = lv_font_get_line_height(g_ui_font ? g_ui_font : LV_FONT_DEFAULT);
    if(lh < 16) lh = 16;
    return lh + 8;
}
static int status_bar_y() { return screen_h() - 4 - status_bar_h(); }
static int chrome_bottom() { return status_bar_y() - 4; }

static void draw_status_bar(const std::string &left, const std::string &right) {
    int y = status_bar_y();
    int h = status_bar_h();
    const lv_font_t *f = g_ui_font ? g_ui_font : LV_FONT_DEFAULT;

    // 顶线单独画、撑满宽度。挂在文字标签的 border 上会被标签自己的宽度截短。
    lv_obj_t *rule = box_raw(g_root, 8, y, screen_w() - 16, 1, false);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, g_theme.muted, 0);

    g_status_right = nullptr;
    if(!right.empty()) {
        g_status_right = label_raw(g_root, right, 8, y, screen_w() - 16, h);
        lv_obj_set_style_text_align(g_status_right, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_pad_top(g_status_right, 4, 0);
    }

    // 左边得给右边(时钟 / 输入法 / WiFi / 电量)留出它实际占的像素,再留一条缝。
    // 两个都是透明底的标签,框重叠不要紧,要命的是**文字**撞上。
    int right_w = 0;
    if(!right.empty() && f)
        right_w = (int)lv_text_get_width(right.c_str(), (uint32_t)right.size(), f, 0);
    int left_w = screen_w() - 16 - right_w - ui_px(20);
    if(left_w < ui_px(120)) left_w = ui_px(120);

    g_status = label_raw(g_root, left, 8, y, left_w, h);
    lv_obj_set_style_text_align(g_status, LV_TEXT_ALIGN_LEFT, 0);
    // 提示语放不下就截成省略号。**绝不能让它折行** —— 这行只有一行的高度,折出来的
    // 第二行会掉到屏幕外面,还会跟右边的时钟/电量叠在一起。
    lv_label_set_long_mode(g_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_top(g_status, 4, 0);
}

// ===========================================================================
// 状态栏右侧:时间 / WiFi / 电池(和 pjournal 主页右边完全一致)
// ===========================================================================

static std::string trim_copy(std::string s) {
    while(!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t start = 0;
    while(start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r'))
        ++start;
    return start ? s.substr(start) : s;
}

// 沿用 tmux 状态栏的 Nerd Font 电池图标:F702(满)…F70B(空)是电量格,
// F701 闪电表示充电、F700 表示放电。
static const char *k_bat_level[10] = {
    "\xef\x9c\x8b", "\xef\x9c\x8a", "\xef\x9c\x89", "\xef\x9c\x88", "\xef\x9c\x87",
    "\xef\x9c\x86", "\xef\x9c\x85", "\xef\x9c\x84", "\xef\x9c\x83", "\xef\x9c\x82",
};

static std::string battery_percent_label() {
    static std::string cache;
    static uint32_t next_ms = 0;
    uint32_t now = lv_tick_get();
    if(!cache.empty() && (int32_t)(now - next_ms) < 0) return cache;

    const char *base = "/sys/class/power_supply";
    DIR *d = opendir(base);
    if(!d) return cache = std::string(k_bat_level[0]) + "\xef\x9c\x80--%";
    std::string cap, status;
    dirent *de = nullptr;
    while((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if(name == "." || name == "..") continue;
        std::string dir = std::string(base) + "/" + name;
        std::string c = trim_copy(read_whole_file(dir + "/capacity"));
        if(c.empty()) continue;
        std::string type = trim_copy(read_whole_file(dir + "/type"));
        if(type == "Mains" || type == "USB" || type == "USB_C") continue;
        cap = c;
        status = trim_copy(read_whole_file(dir + "/status"));
        if(type == "Battery") break;
    }
    closedir(d);
    int pct = cap.empty() ? -1 : atoi(cap.c_str());
    int level = pct < 0 ? 0 : (pct - 5) / 10;
    if(level < 0) level = 0;
    if(level > 9) level = 9;
    const char *sign = status == "Charging" ? "\xef\x9c\x81" : "\xef\x9c\x80";
    cache = (pct < 0 ? std::string(k_bat_level[0]) : std::string(k_bat_level[level])) + sign +
            (pct < 0 ? "--" : std::to_string(pct)) + "%";
    next_ms = now + 30000;
    return cache;
}

// U+3237 满格 = 已连接,U+F1EB 线框 = 接口在但没连上,U+3239 斜杠 = WiFi 关闭。
static std::string wifi_status_label() {
    static std::string cache;
    static uint32_t next_ms = 0;
    uint32_t now = lv_tick_get();
    if(!cache.empty() && (int32_t)(now - next_ms) < 0) return cache;

    std::string sys = "/sys/class/net/" + g_settings.wlan_interface();
    struct stat st {};
    bool link_up = false;
    if(stat(sys.c_str(), &st) == 0) {
        std::string flags = trim_copy(read_whole_file(sys + "/flags"));
        link_up = (strtoul(flags.c_str(), nullptr, 0) & IFF_UP) != 0;
    }
    if(!link_up) cache = "\xe3\x88\xb9";
    else if(g_wifi.status().connected) cache = "\xe3\x88\xb7";
    else cache = "\xef\x87\xab";
    next_ms = now + 5000;
    return cache;
}

static std::string clock_label() {
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char buf[48];
    strftime(buf, sizeof(buf), "%H:%M", &local);
    return buf;
}

static std::string today_label() {
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char buf[48];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
    return buf;
}

static std::string status_right_text() {
    return clock_label() + " " + wifi_status_label() + " " + battery_percent_label();
}

// ===========================================================================
// 字体
// ===========================================================================
// 默认:界面/正文用 Go-LavaPropo-NF-R.ttf,粗体用 Go-Lava-NF-B.ttf,40 号。
// 路径优先取浮墨自己的设置 flomo_font_file / flomo_font_bold_file(空则回退到
// $HOME/.fonts 下同名文件),再退 FontManager 的默认字体。**不读共享的 font_file** ——
// 那是 pjournal / 启动器在用的。

static std::string find_font_path(const std::string &configured, const char *name, bool need_cjk) {
    auto usable = [&](const std::string &p) {
        if(p.empty() || access(p.c_str(), R_OK) != 0) return false;
        return !need_cjk || g_fonts.supports_cjk(p);
    };
    if(usable(configured)) return configured;
    const char *home = getenv("HOME");
    if(home && *home) {
        std::string p = std::string(home) + "/.fonts/" + name;
        if(usable(p)) return p;
    }
    for(const auto &f : g_fonts.fonts())
        if(f.name == name && usable(f.path)) return f.path;
    return std::string();
}

static std::string ui_font_path() {
    std::string p = find_font_path(font_file_setting(), "Go-LavaPropo-NF-R.ttf", true);
    if(p.empty()) p = g_fonts.default_font_path();
    return p;
}

// 可重复调用:改完字号/字体当场重来一遍。g_fonts.load() 按 (路径,字号,样式) 缓存,
// 参数没变时拿回的是同一个对象,不会一次一次地漏字体。
static void reload_fonts() {
    std::string path = ui_font_path();
    if(!g_fonts.supports_cjk(path)) path = g_fonts.default_font_path();
    g_ui_font = g_fonts.load(path, ui_font_size());

    // 粗体不查 CJK 覆盖:它对字体没有「必须含中文」的要求,缺字由下面的 fallback 兜。
    // (设备上这两个 Go-Lava 都是 19MB 的全量 CJK 字体,混排没问题;这一层是给用户
    //  以后换成纯拉丁粗体留的。)
    std::string bold = find_font_path(font_bold_setting(), "Go-Lava-NF-B.ttf", false);
    g_bold_font = nullptr;
    if(!bold.empty()) {
        g_bold_font = g_fonts.load(bold, ui_font_size());
        if(g_bold_font && g_bold_font != g_ui_font) g_bold_font->fallback = g_ui_font;
    }

    // 编辑器那一套:同一对字体,另一个字号
    g_editor_font = g_fonts.load(path, editor_font_size());
    g_editor_bold_font = nullptr;
    if(!bold.empty()) {
        g_editor_bold_font = g_fonts.load(bold, editor_font_size());
        if(g_editor_bold_font && g_editor_bold_font != g_editor_font)
            g_editor_bold_font->fallback = g_editor_font;
    }

    // 先把旧样式整个摘掉再叠新的:lv_style_init 不重置属性,重复 init 会在这份
    // style 上累加,最后一套字号会盖住前面所有取值。
    lv_style_reset(&g_style);
    lv_style_init(&g_style);
    lv_style_set_bg_color(&g_style, g_theme.bg);
    lv_style_set_bg_opa(&g_style, LV_OPA_COVER);
    lv_style_set_text_color(&g_style, g_theme.fg);
    if(g_ui_font) lv_style_set_text_font(&g_style, g_ui_font);
    if(g_root) {
        lv_obj_remove_style_all(g_root);
        lv_obj_add_style(g_root, &g_style, 0);
    }
}

static void use_bold(lv_obj_t *lbl) {
    if(g_bold_font) lv_obj_set_style_text_font(lbl, g_bold_font, 0);
}

// ===========================================================================
// 数据与界面状态
// ===========================================================================

static Store g_store;            // 本地库(token / last_sync 直接从这儿落盘)
static AppState g_app;           // 界面状态,正文 + 过滤 + 选中都在这里

static std::string g_editText;   // 编辑器里的正文(进入编辑时填,保存时读回)
// 正在编辑哪条。用 slug 而不是下标:同步结果回来时 memos 会被整份替换、重排,
// 下标会指到别的笔记上去,保存就写错了对象。空串 = 新建。
static std::string g_editSlug;
static int g_listTop = 0;        // 列表窗口的首行(手动窗口化)

// 详情页从哪儿进来(Esc 就回哪儿):列表 / 今日回顾 / 相关笔记。
static View g_detailReturn = View::List;

// 今日回顾:后台拉回来的 slug 列表,渲染时再按 slug 查正文。
static std::vector<std::string> g_reviewSlugs;
static int g_reviewSel = 0;
static int g_reviewTop = 0;
static bool g_reviewLoading = false;

// 相关笔记:当前结果的 slug 列表 + 来源笔记(Related 页 Esc 回它的详情)。
static std::vector<std::string> g_relatedSlugs;
static std::string g_relatedOrigin;
static int g_relatedSel = 0;
static int g_relatedTop = 0;

// 标签列表(含父前缀),以及改名输入框里被改的那个。
static std::vector<std::string> g_tagList;
static int g_tagSel = 0;
static int g_tagTop = 0;
static std::string g_tagRenameOld;

static std::string g_loginToken;   // Token 登录输入框的初值

static lv_obj_t *g_ta = nullptr;           // 当前输入界面的 textarea
static lv_obj_t *g_detail_body = nullptr;  // 详情正文的可滚容器

// 浮层句柄。它们的实现分散在后面各自的章节里,但 render() 要清指针、hide_modal() 要
// 删对象,所以统一定义在这里。
static lv_obj_t *g_modal_dlg = nullptr;
static int g_modalKind = 0;          // 0 无,1 删除确认,2 编辑退出
static lv_obj_t *g_bl_osd = nullptr;
static lv_obj_t *g_bl_osd_fill = nullptr;
static lv_obj_t *g_bl_osd_pct = nullptr;
static uint32_t g_bl_osd_until = 0;
static int g_bl_osd_track_w = 0;
static int g_bl_osd_bar_h = 0;

// 前向声明:绘制/按键处理互相调用,而且浮层的实现排在很后面。
static void render();
static void goto_view(View v);
static void open_detail();
static void start_edit(const std::string &slug);
static void move_sel(int delta);
static void request_sync(const char *label);
static void request_review();
static void request_related();
static void open_tags();
static void do_tag_rename(const std::string &newTag);
static bool select_slug(const std::string &slug);
static bool commit_editor();
static void delete_current();
static void leave_editor(int action);
static void hide_modal();
static void draw_modal();
static void show_delete_confirm();
static void show_edit_exit();
static void backlight_osd_hide();
static void draw_backlight_osd();
static void backlight_osd_step(int dir);
static void open_settings();
static void settings_key(int key);
static void draw_setting_pick();

// 落盘:把界面里的正文并回 Store。g_store.memos 不参与界面编辑,只有这里更新。
static void save_db() {
    Store s = g_store;
    s.memos = g_app.memos;
    flomo_db_save(s);
}

// ===========================================================================
// 视图辅助
// ===========================================================================

static const Memo *selected_memo() {    if(g_app.selected < 0 || g_app.selected >= (int)g_app.filtered.size()) return nullptr;
    int idx = g_app.filtered[g_app.selected];
    if(idx < 0 || idx >= (int)g_app.memos.size()) return nullptr;
    return &g_app.memos[idx];
}

// "YYYY-MM-DD HH:MM:SS" → "MM-DD HH:MM"。短于这个长度就原样返回。
static std::string short_stamp(const std::string &s) {
    if(s.size() < 16) return s;
    return s.substr(5, 11);
}

static std::string memo_title_line(const Memo &m) {
    std::string out = m.pinned() ? "置顶  " : "";
    out += short_stamp(m.updatedAt.empty() ? m.createdAt : m.updatedAt);
    if(m.dirty) out += " *";
    for(const auto &t : m.tags) out += "  #" + t;
    return out;
}

static std::string memo_body_line(const Memo &m) {
    std::string body;
    for(char c : m.contentText) {
        if(c == '\n' || c == '\r' || c == '\t') {
            if(!body.empty() && body.back() != ' ') body += ' ';
            continue;
        }
        body += c;
    }
    while(!body.empty() && body.back() == ' ') body.pop_back();
    return body;
}

static std::string hint_for_view() {
    // 状态栏左边只有 ~700px(右边要让给时钟/WiFi/电量),40 号字下大约 16 个汉字。
    // 用 pjournal 的 `键:说明` 紧凑写法,别用「 · 」——那圈空格一个就要 60px。
    switch(g_app.view) {
    case View::List: return "/:搜索 t:标签 y:回顾 s:设置 h:帮助";
    case View::Detail: return "↑↓:滚动 e:编辑 d:删除 p:置顶 r:相关 Esc:返回";
    case View::Edit: return "Ctrl+S:保存 Esc:返回";
    case View::Search: return "Enter:确定 Esc:返回";
    case View::Tags: return "Enter:筛选 r:改名 f:输入 Esc:返回";
    case View::TagFilter: return "Enter:筛选 Esc:返回";
    case View::TagRename: return "Enter:改名 Esc:返回";
    case View::Review: return "Enter:打开 r:刷新 Esc:返回";
    case View::Related: return "Enter:打开 r:刷新 Esc:返回";
    case View::Settings: return "↑↓:选择 Enter:确定 Esc:返回";
    case View::LoginEmail:
    case View::LoginPassword:
    case View::LoginToken: return "Enter:下一步 Esc:返回";
    case View::Help: return "Esc:返回";
    }
    return "";
}

// ===========================================================================
// 编辑器 Markdown 叠加层
// ===========================================================================
// lv_textarea 仍是唯一的数据源和输入控件(按键、IME、commit_editor() 全不动);
// 在它正上方盖一个不透明的容器(w、h、边框、内边距都和它一样),里面放按标记
// 排版好的碎片,把原文整个盖住。
//
// 只画浮墨同步协议认得的标记(flomo_api.cpp 的 apply_inline_formats / text_to_html):
//   **粗体**  __下划线__  ==高亮==  行首 "- " / "* " → •
// 标题 #、反引号、~~ 这些协议会原样当普通文字发到 flomo,所以这里也不渲染 ——
// 屏幕上看着是标题、同步回来还是普通一行,那是骗人。
//
// 一套排版(md_layout)同时供三件事用:碎片摆放、光标定位、输入法候选条锚点 ——
// 三者出自同一个结果,天然对得上,不会各算各的。

enum { MF_BOLD = 1, MF_UL = 2, MF_HL = 4 };

struct MdRun {
    int lo = 0, hi = 0;   // disp 字节区间
    uint8_t flags = 0;
};

struct MdPiece {
    int row = 0, x = 0, w = 0, lo = 0, hi = 0;
    const lv_font_t *font = nullptr;
    uint8_t flags = 0;
};

// 一行的解析结果。map 是「源字节偏移 → 显示字节偏移」(长度 = 行长 + 1):
// 被隐藏的标记字节映射到它后面那个可见位置,所以光标贴上去正好落在标记边界上。
struct MdLine {
    std::string disp;
    std::vector<MdRun> runs;
    std::vector<int> map;
};

// 点击落点用:每源行占的一段纵向区间 + 它的排版结果
struct MdTapLine {
    int y0 = 0, y1 = 0, src_start = 0;
    MdLine line;
};

static void update_ime_bar();

static lv_obj_t *g_md_view = nullptr;
static lv_obj_t *g_md_content = nullptr;
static lv_obj_t *g_md_caret = nullptr;
static std::vector<lv_obj_t *> g_md_labels;      // 标签池,反复重排不重建对象
static std::vector<lv_area_t> g_md_hl_rects;     // ==高亮== 的色块,内容层坐标
static std::vector<MdTapLine> g_md_tap_lines;
static int g_md_max_w = 0;                       // 正文可排宽度
static int g_md_line_h = 0;
static int g_md_letter_h = 0;
static int g_md_caret_x = -1, g_md_caret_y = -1; // 叠加层内容坐标(含内边距)
static uint32_t g_md_blink_next = 0;

static int md_pad() { return 2 + ui_px(6); }     // 和 textarea 的 border + pad 对齐
static const int kMdBorder = 2;

static int md_utf8_step(const std::string &s, size_t i) {
    if(i >= s.size()) return (int)s.size();
    unsigned char c = (unsigned char)s[i];
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if(i + (size_t)n > s.size()) n = 1;
    return (int)(i + (size_t)n);
}

static const lv_font_t *md_font_for(uint8_t flags) {
    const lv_font_t *base = g_editor_font ? g_editor_font : LV_FONT_DEFAULT;
    if((flags & MF_BOLD) && g_editor_bold_font) return g_editor_bold_font;
    return base;
}

static uint8_t md_style_at(const std::vector<MdRun> &runs, int at) {
    for(const MdRun &r : runs) {
        if(at < r.lo) break;
        if(at < r.hi) return r.flags;
    }
    return 0;
}

// 镜像 apply_inline_formats():"**" → "__" → "==" 依次扫,每个标记找下一个同样的标记
// 配对,不嵌套。光标落在某对标记**里面**时这一对不隐藏 —— 否则用户既看不到也删不掉它。
static MdLine md_parse_line(const std::string &line, int caret_rel) {
    std::vector<char> hidden(line.size(), 0);
    std::vector<uint8_t> st(line.size(), 0);
    static const struct { const char *m; uint8_t f; } kFmt[] = {
        {"**", MF_BOLD}, {"__", MF_UL}, {"==", MF_HL},
    };
    for(const auto &fmt : kFmt) {
        size_t pos = 0;
        for(;;) {
            size_t ps = line.find(fmt.m, pos);
            if(ps == std::string::npos) break;
            size_t pe = line.find(fmt.m, ps + 2);
            if(pe == std::string::npos) break;
            if(!(caret_rel > (int)ps && caret_rel < (int)(pe + 2))) {
                hidden[ps] = hidden[ps + 1] = 1;
                hidden[pe] = hidden[pe + 1] = 1;
            }
            for(size_t k = ps + 2; k < pe; ++k) st[k] |= fmt.f;
            pos = pe + 2;
        }
    }

    // 行首 "- " / "* " 换成 •。text_to_html 是先 trim 掉前导空白再看标号的,这里照做。
    int bullet = -1;
    size_t from = line.find_first_not_of(" \t");
    if(from != std::string::npos && line.size() >= from + 2 && line[from + 1] == ' ' &&
       (line[from] == '-' || line[from] == '*')) {
        if(!(caret_rel > (int)from && caret_rel < (int)from + 2)) {
            hidden[from] = hidden[from + 1] = 1;
            bullet = (int)from;
        }
    }

    MdLine out;
    out.map.assign(line.size() + 1, 0);
    std::vector<uint8_t> dstyle;
    size_t i = 0;
    while(i < line.size()) {
        out.map[i] = (int)out.disp.size();
        if((int)i == bullet) {
            out.disp += "\xE2\x80\xA2";   // •(3 字节,占位宽和 "- " 不同没关系,map 兜着)
            dstyle.push_back(0);
            dstyle.push_back(0);
            dstyle.push_back(0);
            out.map[i + 1] = (int)out.disp.size();
            out.map[i + 2] = (int)out.disp.size();
            i += 2;
            continue;
        }
        if(!hidden[i]) {
            out.disp += line[i];
            dstyle.push_back(st[i]);
        }
        ++i;
    }
    out.map[line.size()] = (int)out.disp.size();

    for(size_t k = 0; k < dstyle.size(); ++k) {
        if(!dstyle[k]) continue;
        if(!out.runs.empty() && out.runs.back().hi == (int)k && out.runs.back().flags == dstyle[k])
            out.runs.back().hi = (int)k + 1;
        else
            out.runs.push_back({(int)k, (int)k + 1, dstyle[k]});
    }
    return out;
}

// 唯一的排版逻辑:横排、按 max_w 折行、每个片段量一次宽度。
static void md_layout(const std::string &disp, const std::vector<MdRun> &runs, int letter_space,
                      int max_w, std::vector<MdPiece> &out) {
    out.clear();
    if(max_w < 1) max_w = 1;
    int n = (int)disp.size();
    int row = 0, x = 0, i = 0;
    while(i < n) {
        uint8_t flags = md_style_at(runs, i);
        const lv_font_t *f = md_font_for(flags);
        int seg_end = n;
        for(const MdRun &r : runs) {
            if(r.lo > i && r.lo < seg_end) seg_end = r.lo;
            if(r.hi > i && r.hi < seg_end) seg_end = r.hi;
        }
        while(i < seg_end) {
            int avail = max_w - x;
            if(avail < 1) { ++row; x = 0; avail = max_w; }
            uint32_t len = lv_text_get_next_line(disp.c_str() + i, f, letter_space, (uint32_t)avail,
                                                 NULL, LV_TEXT_FLAG_NONE);
            if(len == 0 || (int)len > seg_end - i) len = (uint32_t)(seg_end - i);
            if(len == 0) len = (uint32_t)(md_utf8_step(disp, (size_t)i) - i);
            if(len == 0) break;
            int wpx = (int)lv_text_get_width(disp.c_str() + i, len, f, letter_space);
            if(wpx > avail && x > 0) { ++row; x = 0; continue; }
            MdPiece p;
            p.row = row;
            p.x = x;
            p.w = wpx;
            p.lo = i;
            p.hi = i + (int)len;
            p.font = f;
            p.flags = flags;
            out.push_back(p);
            i += (int)len;
            x += wpx;
            if(x >= max_w) { ++row; x = 0; }
        }
    }
}

static void md_caret_from_pieces(const std::string &disp, const std::vector<MdPiece> &p, int byte_off,
                                 int letter_space, int line_h, int &out_x, int &out_y) {
    out_x = 0;
    out_y = 0;
    if(p.empty()) return;
    if(byte_off <= p.front().lo) {
        out_x = p.front().x;
        out_y = p.front().row * line_h;
        return;
    }
    // 取**最后一个**匹配的碎片:光标落在换行边界上时,前一碎片的末尾和后一碎片的
    // 开头是同一个字节偏移,取后者才落在下一行的行首(LVGL 自己也是这么摆的)。
    bool found = false;
    for(const MdPiece &q : p) {
        if(byte_off < q.lo || byte_off > q.hi) continue;
        out_x = q.x + (int)lv_text_get_width(disp.c_str() + q.lo, (uint32_t)(byte_off - q.lo),
                                             q.font, letter_space);
        out_y = q.row * line_h;
        found = true;
    }
    if(found) return;
    const MdPiece &q = p.back();
    out_x = q.x + q.w;
    out_y = q.row * line_h;
}

// ==高亮== 的色块画在内容层自己的绘制回调里(先于子标签),文字正好压在色块上。
// 回调里的坐标是屏幕绝对坐标,矩形是内容层局部坐标,要加上层原点。
static void md_draw_highlights(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    if(!layer || !o || g_md_hl_rects.empty()) return;
    lv_area_t org {};
    lv_obj_get_coords(o, &org);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.bg_color = g_theme.accent;
    dsc.bg_opa = LV_OPA_COVER;
    for(const lv_area_t &a : g_md_hl_rects) {
        lv_area_t r {a.x1 + org.x1, a.y1 + org.y1, a.x2 + org.x1, a.y2 + org.y1};
        lv_draw_rect(layer, &dsc, &r);
    }
}

static lv_obj_t *md_label_in(int idx, const lv_font_t *font, int letter_space) {
    lv_obj_t *o = idx < (int)g_md_labels.size() ? g_md_labels[idx] : nullptr;
    if(!o) {
        o = lv_label_create(g_md_content);
        // 排版已经算好了,标签只负责把这一小段画出来,不能再让它自己折行
        lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_shadow_width(o, 0, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        g_md_labels.push_back(o);
    }
    // 字体随片段变,复用的标签必须重设,否则上一帧的字形会留在原地
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_letter_space(o, letter_space, 0);
    lv_obj_set_style_text_line_space(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void md_create(lv_obj_t *parent, int x, int y, int w, int h) {
    int pad = md_pad();
    g_md_max_w = w - 2 * pad - 2;
    if(g_md_max_w < 1) g_md_max_w = 1;

    g_md_view = lv_obj_create(parent);
    lv_obj_set_pos(g_md_view, x, y);
    lv_obj_set_size(g_md_view, w, h);
    lv_obj_set_style_radius(g_md_view, 0, 0);
    lv_obj_set_style_pad_all(g_md_view, 0, 0);
    lv_obj_set_style_shadow_width(g_md_view, 0, 0);
    lv_obj_set_style_border_color(g_md_view, g_theme.fg, 0);
    lv_obj_set_style_border_width(g_md_view, kMdBorder, 0);
    lv_obj_set_style_bg_color(g_md_view, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(g_md_view, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(g_md_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(g_md_view, LV_DIR_VER);

    g_md_content = lv_obj_create(g_md_view);
    lv_obj_set_pos(g_md_content, 0, 0);
    lv_obj_set_size(g_md_content, w, h);
    lv_obj_set_style_radius(g_md_content, 0, 0);
    lv_obj_set_style_border_width(g_md_content, 0, 0);
    lv_obj_set_style_pad_all(g_md_content, 0, 0);
    lv_obj_set_style_shadow_width(g_md_content, 0, 0);
    lv_obj_set_style_bg_opa(g_md_content, LV_OPA_TRANSP, 0);
    // 这一层只负责盛放排版好的碎片,滚动由 g_md_view 做。它自己可滚动的话,
    // 碎片超出 1px(下划线)就会触发 AUTO 滚动条,在文字底部画一条横贯整屏的灰线。
    lv_obj_remove_flag(g_md_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_md_content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(g_md_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_md_content, md_draw_highlights, LV_EVENT_DRAW_MAIN, nullptr);

    g_md_caret = lv_obj_create(g_md_content);
    lv_obj_set_pos(g_md_caret, 0, 0);
    lv_obj_set_size(g_md_caret, 2, 20);
    lv_obj_set_style_radius(g_md_caret, 0, 0);
    lv_obj_set_style_border_width(g_md_caret, 0, 0);
    lv_obj_set_style_pad_all(g_md_caret, 0, 0);
    lv_obj_set_style_shadow_width(g_md_caret, 0, 0);
    lv_obj_set_style_bg_color(g_md_caret, g_theme.accent, 0);
    lv_obj_set_style_bg_opa(g_md_caret, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_md_caret, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_md_caret, LV_OBJ_FLAG_CLICKABLE);
}

static void md_refresh() {
    if(!g_md_view || !g_md_content || !g_ta) return;
    const lv_font_t *font = g_editor_font ? g_editor_font : LV_FONT_DEFAULT;
    int letter_space = (int)lv_obj_get_style_text_letter_space(g_ta, 0);
    int line_space = (int)lv_obj_get_style_text_line_space(g_ta, 0);
    g_md_letter_h = lv_font_get_line_height(font);
    g_md_line_h = g_md_letter_h + line_space;
    const int pad = md_pad();

    const char *ctxt = lv_textarea_get_text(g_ta);
    std::string text = ctxt ? ctxt : "";
    int caret_byte = (int)lv_text_encoded_get_byte_id(ctxt ? ctxt : "",
                                                      lv_textarea_get_cursor_pos(g_ta));

    g_md_hl_rects.clear();
    g_md_tap_lines.clear();

    std::vector<std::string> lines;
    std::vector<int> starts;
    size_t s = 0;
    for(size_t i = 0; i <= text.size(); ++i) {
        if(i == text.size() || text[i] == '\n') {
            starts.push_back((int)s);
            lines.push_back(text.substr(s, i - s));
            s = i + 1;
        }
    }

    int lbi = 0;
    int y = pad;
    int caret_x = -1, caret_y = -1;
    for(size_t li = 0; li < lines.size(); ++li) {
        int start = starts[li];
        int len = (int)lines[li].size();
        int caret_rel = (caret_byte >= start && caret_byte <= start + len) ? caret_byte - start : -1;
        MdLine ml = md_parse_line(lines[li], caret_rel);

        std::vector<MdPiece> pieces;
        md_layout(ml.disp, ml.runs, letter_space, g_md_max_w, pieces);
        int rows = 1;
        for(const MdPiece &q : pieces)
            if(q.row + 1 > rows) rows = q.row + 1;
        int h = rows * g_md_line_h;

        for(const MdPiece &q : pieces) {
            if(!(q.flags & MF_HL)) continue;
            int px = pad + q.x;
            int py = y + q.row * g_md_line_h;
            lv_area_t a {px, py, px + q.w - 1, py + g_md_letter_h - 1};
            g_md_hl_rects.push_back(a);
        }
        for(const MdPiece &q : pieces) {
            lv_obj_t *o = md_label_in(lbi++, q.font, letter_space);
            lv_obj_set_pos(o, pad + q.x, y + q.row * g_md_line_h);
            // 高度比行高多给一点:__下划线__ 画在基线下方,贴着行高会被裁掉
            lv_obj_set_size(o, g_md_max_w - q.x + 2, g_md_letter_h + 3);
            lv_label_set_text(o, ml.disp.substr((size_t)q.lo, (size_t)(q.hi - q.lo)).c_str());
            lv_obj_set_style_text_decor(o, (q.flags & MF_UL) ? LV_TEXT_DECOR_UNDERLINE
                                                             : LV_TEXT_DECOR_NONE, 0);
            lv_obj_set_style_text_color(o, (q.flags & MF_HL) ? g_theme.bg : g_theme.fg, 0);
        }

        if(caret_rel >= 0) {
            md_caret_from_pieces(ml.disp, pieces, ml.map[caret_rel], letter_space, g_md_line_h,
                                 caret_x, caret_y);
            caret_x += pad;
            caret_y += y;
        }
        MdTapLine tl;
        tl.y0 = y;
        tl.y1 = y + h;
        tl.src_start = start;
        tl.line = std::move(ml);
        g_md_tap_lines.push_back(std::move(tl));
        y += h;
    }

    for(int k = lbi; k < (int)g_md_labels.size(); ++k) lv_obj_add_flag(g_md_labels[k], LV_OBJ_FLAG_HIDDEN);

    int view_w = (int)lv_obj_get_width(g_md_view);
    int view_h = (int)lv_obj_get_height(g_md_view);
    int content_h = (y - pad) + 2 * pad;
    if(content_h < view_h) content_h = view_h;
    lv_obj_set_size(g_md_content, view_w, content_h);

    if(g_md_caret) {
        if(caret_x >= 0) {
            if(caret_x > pad + g_md_max_w - 2) caret_x = pad + g_md_max_w - 2;
            lv_obj_remove_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(g_md_caret, caret_x, caret_y);
            lv_obj_set_size(g_md_caret, 2, g_md_letter_h);
            lv_obj_move_foreground(g_md_caret);   // 标签是后建的,把自己提到它们上面
            g_md_caret_x = caret_x;
            g_md_caret_y = caret_y;
        } else {
            lv_obj_add_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
            g_md_caret_x = -1;
            g_md_caret_y = -1;
        }
    }

    // 滚动范围是按子对象坐标算的,先把这一层排完版再滚动
    lv_obj_update_layout(g_md_view);
    if(caret_y >= 0) {
        int scroll = (int)lv_obj_get_scroll_y(g_md_view);
        if(caret_y - scroll < pad) scroll = caret_y - pad;
        else if(caret_y + g_md_line_h - scroll > view_h - pad)
            scroll = caret_y + g_md_line_h - view_h + pad;
        lv_obj_scroll_to_y(g_md_view, scroll, LV_ANIM_OFF);
    }

    // 刚打完字光标要亮着:强制显形再重新计时 —— 否则正好赶在灭的那半秒上,打完一个字
    // 屏幕上什么都没变,要再等半秒光标才回来。
    if(g_md_caret && caret_x >= 0) {
        lv_obj_remove_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
        g_md_blink_next = lv_tick_get() + 500;
    }

    update_ime_bar();
}

// ===========================================================================
// 候选条(横排:上行编码、分隔线、下行候选),逐字移植自 pjournal 的 app_ui.cpp
// ===========================================================================

static lv_obj_t *g_ime_bar = nullptr;
static lv_obj_t *g_ime_head = nullptr;
static lv_obj_t *g_ime_cands = nullptr;
static lv_obj_t *g_ime_rule = nullptr;

static const lv_font_t *ime_font() { return g_ui_font ? g_ui_font : LV_FONT_DEFAULT; }

// rime 的 menu/page_size 是 5,fcitx5 5.1 的 InputContext1 没有 SetPageSize,
// 客户端改不了这个数,照着显示就行。
static constexpr int kImeCandPerPage = 5;
static constexpr int kImePad = 4;
static constexpr int kImeRuleGap = 2;
static constexpr int kImeRuleH = 1;

static int ime_line_h() {
    int lh = lv_font_get_line_height(ime_font());
    return lh < 12 ? 12 : lh;
}

static int ime_bar_h(int cand_h = 0) {
    if(cand_h <= 0) cand_h = ime_line_h();
    return kImePad * 2 + ime_line_h() + kImeRuleGap * 2 + kImeRuleH + cand_h;
}

static void create_ime_bar() {
    if(g_ime_bar) return;
    int h = ime_bar_h();
    g_ime_bar = box_raw(g_root, 8, chrome_bottom() - h, 120, h);
    lv_obj_set_style_pad_all(g_ime_bar, kImePad, 0);

    g_ime_head = lv_label_create(g_ime_bar);
    g_ime_cands = lv_label_create(g_ime_bar);
    for(lv_obj_t *l : {g_ime_head, g_ime_cands}) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(l, g_theme.fg, 0);
        lv_obj_set_style_text_font(l, ime_font(), 0);
        lv_obj_set_style_text_letter_space(l, 0, 0);
        lv_obj_set_style_text_line_space(l, 0, 0);
        lv_obj_set_style_pad_all(l, 0, 0);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    }

    g_ime_rule = lv_obj_create(g_ime_bar);
    lv_obj_set_style_radius(g_ime_rule, 0, 0);
    lv_obj_set_style_border_width(g_ime_rule, 0, 0);
    lv_obj_set_style_pad_all(g_ime_rule, 0, 0);
    lv_obj_set_style_bg_color(g_ime_rule, g_theme.fg, 0);
    lv_obj_set_style_bg_opa(g_ime_rule, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_ime_rule, LV_OBJ_FLAG_SCROLLABLE);
}

static void ime_bar_place(int content_w, int cand_h) {
    int lh = ime_line_h();
    lv_obj_set_pos(g_ime_head, 0, 0);
    lv_obj_set_size(g_ime_head, content_w, lh);
    int ry = lh + kImeRuleGap;
    lv_obj_set_pos(g_ime_rule, 0, ry);
    lv_obj_set_size(g_ime_rule, content_w, kImeRuleH);
    lv_obj_set_pos(g_ime_cands, 0, ry + kImeRuleH + kImeRuleGap);
    lv_obj_set_size(g_ime_cands, content_w, cand_h);
}

// 框宽按两行里宽的那行收缩,再把 x 往左挪到整个框落在屏内。
static void ime_bar_layout_horizontal(int x, int y, int max_w) {
    const lv_font_t *f = ime_font();
    LinuxIme &ime = g_linux_ime;
    ime.set_display_width(0);   // 0 = 让后端把整页候选都交出来

    std::string head = ime.composition();
    const auto &c = ime.candidates();
    int hi = ime.highlight_index();
    std::string s;
    for(size_t i = 0; i < c.size(); ++i) {
        std::string part = std::to_string((int)i + 1) + "." + c[i];
        if((int)i == hi) part = "[" + part + "]";
        if(!s.empty()) s += " ";
        s += part;
    }

    int head_w = f ? (int)lv_text_get_width(head.c_str(), (uint32_t)head.size(), f, 0) : 0;
    int cand_w = f ? (int)lv_text_get_width(s.c_str(), (uint32_t)s.size(), f, 0) : 0;
    int box_w = (head_w > cand_w ? head_w : cand_w) + kImePad * 2 + 2;   // 2 = 左右边框
    if(box_w < 120) box_w = 120;
    if(box_w > max_w) box_w = max_w;
    if(x + box_w > screen_w() - 8) x = screen_w() - 8 - box_w;
    if(x < 8) x = 8;

    lv_label_set_text(g_ime_head, head.c_str());
    lv_label_set_text(g_ime_cands, s.c_str());
    ime_bar_place(box_w - kImePad * 2 - 2, ime_line_h());
    lv_obj_set_pos(g_ime_bar, x, y);
    lv_obj_set_size(g_ime_bar, box_w, ime_bar_h());
}

// LVGL 的 textarea 把「把光标滚进可视区」整个做成 LV_ANIM_ON,要一百多毫秒才走完;
// 候选条位置紧接着按键算,读到的是滚动之前的坐标。照 LVGL 那两条纵向判断一次性钉到终点。
static void textarea_snap_cursor_scroll(lv_obj_t *ta) {
    lv_obj_t *label = lv_textarea_get_label(ta);
    if(!label) return;
    lv_point_t p {};
    lv_label_get_letter_pos(label, lv_textarea_get_cursor_pos(ta), &p);
    const lv_font_t *f = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
    int fh = f ? lv_font_get_line_height(f) : 0;
    int content_h = (int)lv_obj_get_content_height(ta);
    if(p.y < (int)lv_obj_get_scroll_top(ta)) {
        lv_obj_scroll_to_y(ta, p.y, LV_ANIM_OFF);
    }
    if(p.y + fh - (int)lv_obj_get_scroll_top(ta) > content_h) {
        lv_obj_scroll_to_y(ta, p.y - content_h + fh, LV_ANIM_OFF);
    }
}

// 候选条贴着光标那一行摆:下面放得下就让框顶对齐下一行的行顶,放不下整条翻上去。
static void update_ime_bar() {
    if(!g_ta) return;
    create_ime_bar();
    if(!g_ime_bar) return;
    if(!g_linux_ime.active() || !g_linux_ime.composing()) {
        lv_obj_add_flag(g_ime_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(g_ime_bar, LV_OBJ_FLAG_HIDDEN);

    int x = 12;
    int y = chrome_bottom() - ime_bar_h();
    int line_top = -1;
    int line_pitch = 0;
    int fh = g_editor_font ? lv_font_get_line_height(g_editor_font) : editor_font_size();

    if(g_md_view && g_md_caret_x >= 0) {
        // 叠加层开着:textarea 的原文整个被盖住了,候选条得按叠加层的排版摆 ——
        // 走 lv_textarea_get_label() 那套是按**原文**量宽的,和看到的字对不上。
        lv_area_t va {};
        lv_obj_get_coords(g_md_view, &va);
        int sc = (int)lv_obj_get_scroll_y(g_md_view);
        x = va.x1 + g_md_caret_x;
        line_top = va.y1 + g_md_caret_y - sc;
        line_pitch = g_md_line_h > 0 ? g_md_line_h : fh;
        // 光标行滚出可视区时把候选条夹回来,别飘到状态栏上去
        if(line_top < va.y1) line_top = va.y1;
        if(line_top > va.y2 - line_pitch) line_top = va.y2 - line_pitch;
    } else {
        textarea_snap_cursor_scroll(g_ta);
        lv_obj_t *ta_label = lv_textarea_get_label(g_ta);
        if(ta_label) {
            // 必须用 lv_obj_get_coords() 的**绝对**坐标,lv_obj_get_y() 会把自己父对象的
            // 滚动量加回去,量出来的是文档坐标,正文一长就跑到屏幕外面去了。
            lv_area_t ta_area {}, la_area {};
            lv_obj_get_coords(g_ta, &ta_area);
            lv_obj_get_coords(ta_label, &la_area);
            lv_point_t p {};
            lv_label_get_letter_pos(ta_label, lv_textarea_get_cursor_pos(g_ta), &p);
            x = la_area.x1 + p.x;
            line_top = la_area.y1 + p.y;
            line_pitch = fh + (int)lv_obj_get_style_text_line_space(g_ta, 0);
        }
    }
    if(x < 8) x = 8;
    if(x > 360) x = 360;

    const int bh = ime_bar_h();
    const int cb = chrome_bottom();
    if(line_top >= 0 && line_pitch > 0) {
        int below = line_top + line_pitch;
        if(below + bh <= cb) y = below;
        else {
            y = line_top - bh;
            if(y < 8) y = 8;
        }
    } else {
        if(y + bh > cb) y = cb - bh;
        if(y < 8) y = 8;
    }
    ime_bar_layout_horizontal(x, y, screen_w() - 16);
    lv_obj_move_foreground(g_ime_bar);
}

// ===========================================================================
// 文本控件
// ===========================================================================

static void style_textarea(lv_obj_t *ta) {
    lv_obj_set_style_bg_color(ta, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, g_theme.fg, 0);
    lv_obj_set_style_border_color(ta, g_theme.fg, 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_outline_width(ta, 0, 0);
    lv_obj_set_style_pad_all(ta, ui_px(6), 0);
    lv_obj_set_style_bg_color(ta, g_theme.accent, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    if(g_editor_font) lv_obj_set_style_text_font(ta, g_editor_font, 0);
    lv_textarea_set_cursor_click_pos(ta, true);
}

static lv_obj_t *new_textarea(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, h);
    style_textarea(ta);
    return ta;
}

static void focus_ta(lv_obj_t *ta) {
    g_ta = ta;
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_group_focus_obj(ta);
}

// ===========================================================================
// 各个视图的绘制
// ===========================================================================

static int title_h() { return ui_px(34); }
static int title_y() { return ui_px(8); }
static int content_top() { return title_y() + title_h() + ui_px(6); }
static int card_h() { return 2 * ui_line_h() + ui_px(10); }

static int visible_count() {
    int n = 0;
    for(const auto &m : g_app.memos)
        if(!m.deleted) ++n;
    return n;
}

static void draw_title(const std::string &text, const std::string &right) {
    // 左右两块都是单行,右边的日期/条数不能被左边挤掉 —— 宽度按右边实际量出来的像素分,
    // 和状态栏同一个做法。字号能调到 60,这里不能写死 640/656。
    int right_w = 0;
    if(!right.empty()) {
        const lv_font_t *f = g_ui_font ? g_ui_font : LV_FONT_DEFAULT;
        right_w = (int)lv_text_get_width(right.c_str(), (uint32_t)right.size(), f, 0) + ui_px(8);
        if(right_w > screen_w() / 2) right_w = screen_w() / 2;
    }
    int left_w = screen_w() - ui_px(16) - (right.empty() ? 0 : right_w + ui_px(8));
    if(left_w < ui_px(120)) left_w = ui_px(120);

    lv_obj_t *l = label_raw(g_root, text, ui_px(8), title_y(), left_w, title_h());
    use_bold(l);
    lv_obj_set_style_text_color(l, g_theme.heading, 0);
    // 标题带只有一行高,任何一个标签折行都会掉到屏外盖住正文。宁可截成省略号。
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    if(!right.empty()) {
        lv_obj_t *r = label_raw(g_root, right, screen_w() - ui_px(8) - right_w, title_y(),
                                right_w, title_h());
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(r, g_theme.muted, 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
    }
}

static lv_obj_t *draw_card(int x, int y, int w, const Memo &m, bool selected) {
    int h = card_h();
    lv_obj_t *card = box_raw(g_root, x, y, w, h, selected);
    lv_obj_t *top = label_raw(card, memo_title_line(m), 0, 0, w - 8, ui_line_h() + 2);
    lv_obj_set_style_text_color(top, selected ? g_theme.bg : g_theme.muted, 0);
    lv_obj_t *body = label_raw(card, memo_body_line(m), 0, ui_line_h() + ui_px(4), w - 8,
                               ui_line_h() + 2);
    lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(body, selected ? g_theme.bg : g_theme.fg, 0);
    return card;
}

static void draw_list() {
    // 筛选生效时把日期换成筛选条件 —— 列表被筛过却没有任何提示的话,用户会以为
    // 笔记丢了(空结果那条提示只在一条都不剩时才出现)。
    std::string right = today_label() + "  " + std::to_string(visible_count()) + " 条";
    if(!g_app.query.empty())
        right = "筛选“" + g_app.query + "”  " + std::to_string(visible_count()) + " 条";
    else if(!g_app.tag.empty())
        right = "#" + g_app.tag + "  " + std::to_string(visible_count()) + " 条";
    draw_title(std::string(flomo_icon()) + " 浮墨", right);

    const int top = content_top();
    int per = (chrome_bottom() - top) / card_h();
    const int total = (int)g_app.filtered.size();

    if(per <= 0) return;
    if(total == 0) {
        std::string msg = g_app.memos.empty() ? "还没有笔记 · 按 n 新建" : "没有匹配的笔记 · 按 Esc 清除筛选";
        lv_obj_t *l = label_raw(g_root, msg, ui_px(8), top + ui_px(20), screen_w() - 16, ui_line_h() + 2);
        lv_obj_set_style_text_color(l, g_theme.muted, 0);
        return;
    }

    if(g_app.selected < g_listTop) g_listTop = g_app.selected;
    if(g_app.selected >= g_listTop + per) g_listTop = g_app.selected - per + 1;
    if(g_listTop > total - per) g_listTop = total - per;
    if(g_listTop < 0) g_listTop = 0;

    for(int row = 0; row < per; ++row) {
        int i = g_listTop + row;
        if(i >= total) break;
        int y = top + row * card_h();
        draw_card(8, y, screen_w() - 16, g_app.memos[g_app.filtered[i]], i == g_app.selected);
    }

    // 选中位置 / 总数,替掉滚动条
    if(total > per) {
        std::string pos = std::to_string(g_app.selected + 1) + "/" + std::to_string(total);
        lv_obj_t *l = label_raw(g_root, pos, screen_w() - 240, top, 232, ui_line_h() + 2);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(l, g_theme.muted, 0);
    }
}

static int memo_index_by_slug(const std::string &slug);

static void draw_slug_list(const std::string &title, const std::string &right,
                           const std::vector<std::string> &slugs, int sel, int &top,
                           const std::string &empty) {
    draw_title(title, right);
    const int top_y = content_top();
    int per = (chrome_bottom() - top_y) / card_h();
    const int total = (int)slugs.size();
    if(per <= 0) return;
    if(total == 0) {
        lv_obj_t *l = label_raw(g_root, empty, ui_px(8), top_y + ui_px(20), screen_w() - 16,
                                ui_line_h() + 2);
        lv_obj_set_style_text_color(l, g_theme.muted, 0);
        return;
    }
    if(sel < top) top = sel;
    if(sel >= top + per) top = sel - per + 1;
    if(top > total - per) top = total - per;
    if(top < 0) top = 0;

    for(int row = 0; row < per; ++row) {
        int i = top + row;
        if(i >= total) break;
        int idx = memo_index_by_slug(slugs[i]);
        if(idx < 0) continue;
        draw_card(8, top_y + row * card_h(), screen_w() - 16, g_app.memos[idx], i == sel);
    }
    if(total > per) {
        std::string pos = std::to_string(sel + 1) + "/" + std::to_string(total);
        lv_obj_t *l = label_raw(g_root, pos, screen_w() - 240, top_y, 232, ui_line_h() + 2);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(l, g_theme.muted, 0);
    }
}

static void draw_review() {
    draw_slug_list(std::string(flomo_icon()) + " 今日回顾",
                   std::to_string(g_reviewSlugs.size()) + " 条", g_reviewSlugs, g_reviewSel,
                   g_reviewTop, "今天没有回顾 · 按 Esc 返回");
}

static void draw_related() {
    draw_slug_list(std::string(flomo_icon()) + " 相关笔记",
                   std::to_string(g_relatedSlugs.size()) + " 条", g_relatedSlugs, g_relatedSel,
                   g_relatedTop, "没有相关笔记 · 按 Esc 返回");
}

static int tag_row_h() { return ui_line_h() + ui_px(8); }

static int tag_memo_count(const std::string &tag) {
    int n = 0;
    for(const auto &m : g_app.memos)
        if(!m.deleted && memo_has_tag(m, tag)) ++n;
    return n;
}

static void draw_tags() {
    draw_title("标签", std::to_string(g_tagList.size()) + " 个");
    const int top_y = content_top();
    const int row_h = tag_row_h();
    int per = (chrome_bottom() - top_y) / row_h;
    if(per < 1) per = 1;
    const int total = (int)g_tagList.size();
    if(total == 0) {
        lv_obj_t *l = label_raw(g_root, "还没有标签 · 正文里写 #标签", ui_px(8), top_y + ui_px(20),
                                screen_w() - 16, ui_line_h() + 2);
        lv_obj_set_style_text_color(l, g_theme.muted, 0);
        return;
    }
    if(g_tagSel < g_tagTop) g_tagTop = g_tagSel;
    if(g_tagSel >= g_tagTop + per) g_tagTop = g_tagSel - per + 1;
    if(g_tagTop > total - per) g_tagTop = total - per;
    if(g_tagTop < 0) g_tagTop = 0;

    const int row_w = screen_w() - 16;
    const int inner = row_w - 8;
    for(int i = g_tagTop; i < total && i < g_tagTop + per; ++i) {
        const bool sel = (i == g_tagSel);
        lv_obj_t *row = box_raw(g_root, 8, top_y + (i - g_tagTop) * row_h, row_w,
                                row_h - ui_px(3), sel);
        const std::string &tag = g_tagList[i];
        size_t depth = 0;
        for(char c : tag)
            if(c == '/') ++depth;
        std::string label = std::string(depth * 2, ' ') + "#" + tag;
        lv_obj_t *l = label_raw(row, label, 0, 0, inner - ui_px(90), ui_line_h() + 2);
        lv_obj_set_style_text_color(l, sel ? g_theme.bg : g_theme.fg, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_t *cnt = label_raw(row, std::to_string(tag_memo_count(tag)),
                                  inner - ui_px(90), 0, ui_px(90), ui_line_h() + 2);
        lv_obj_set_style_text_align(cnt, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(cnt, sel ? g_theme.bg : g_theme.muted, 0);
    }
    if(total > per) {
        std::string pos = std::to_string(g_tagSel + 1) + "/" + std::to_string(total);
        lv_obj_t *l = label_raw(g_root, pos, screen_w() - 240, top_y, 232, ui_line_h() + 2);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(l, g_theme.muted, 0);
    }
}

static void draw_detail() {
    const Memo *m = selected_memo();
    if(!m) {
        g_app.view = View::List;
        return;
    }
    std::string right;
    if(m->pinned()) right = "置顶";
    if(m->dirty) right += right.empty() ? "未同步 *" : " · 未同步 *";
    draw_title(short_stamp(m->updatedAt.empty() ? m->createdAt : m->updatedAt), right);

    int y = content_top();
    if(!m->tags.empty()) {
        std::string tags;
        for(const auto &t : m->tags) tags += "#" + t + "  ";
        lv_obj_t *l = label_raw(g_root, tags, ui_px(8), y, screen_w() - 16, ui_line_h() + 2);
        lv_obj_set_style_text_color(l, g_theme.accent, 0);
        y += ui_line_h() + ui_px(6);
    }

    g_detail_body = lv_obj_create(g_root);
    lv_obj_set_pos(g_detail_body, 8, y);
    lv_obj_set_size(g_detail_body, screen_w() - 16, chrome_bottom() - y);
    lv_obj_set_style_bg_opa(g_detail_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_detail_body, 0, 0);
    lv_obj_set_style_radius(g_detail_body, 0, 0);
    lv_obj_set_style_pad_all(g_detail_body, 0, 0);
    lv_obj_set_scroll_dir(g_detail_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_detail_body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *body = label_raw(g_detail_body, m->contentText, 0, 0, screen_w() - 16 - 28);
    lv_obj_set_style_text_color(body, g_theme.fg, 0);
    if(g_editor_font) lv_obj_set_style_text_font(body, g_editor_font, 0);
}

static void draw_edit() {
    draw_title(g_editSlug.empty() ? "新建笔记" : "编辑笔记", g_app.status);
    int x = 8, y = content_top(), w = screen_w() - 16, h = chrome_bottom() - content_top();
    lv_obj_t *ta = new_textarea(g_root, x, y, w, h);
    lv_textarea_set_text(ta, g_editText.c_str());
    focus_ta(ta);
    // 叠加层建在 textarea 之后 —— 同父同级,后建的画在上面,正好把原文盖住
    md_create(g_root, x, y, w, h);
    md_refresh();
}

static void draw_help() {
    draw_title("快捷键", "");
    static const char *kHelp[] = {
        "↑ ↓ / j k       上下移动",
        "PgUp / PgDn     翻页",
        "Home / End      跳到首尾",
        "Enter           打开 / 确定",
        "n  e  d         新建 / 编辑 / 删除",
        "p               置顶 / 取消置顶",
        "/               搜索",
        "t               标签列表",
        "y               今日回顾",
        "r               列表:同步 · 详情:相关笔记",
        "s               设置",
        "l / L           邮箱登录 / Token 登录",
        "h               本页帮助",
        "Ctrl+Space      切换输入法",
        "Ctrl+S          保存",
        "Esc             返回上一级",
        "Esc / q         列表页回到主菜单",
        "Ctrl+Q          退出程序",
    };
    int y = content_top();
    for(const char *line : kHelp) {
        label_raw(g_root, line, ui_px(12), y, screen_w() - 24, ui_line_h() + 2);
        y += ui_line_h() + ui_px(6);
        if(y > chrome_bottom()) break;
    }
}

// ===========================================================================
// 设置界面
// ===========================================================================
// 交互照搬 pjournal-lvgl-typix/src/app_ui.cpp 的那一套:行列表 → Enter 开选项浮层
// → ↑↓ 选 → Enter 应用 → Esc 返回。不引 LVGL 滚动容器,和列表一样手动窗口化。
//
// 键名一律带 flomo_ 前缀(存在同一个 ~/.pjournal-lvgl/settings/ 目录里,一键一文件),
// 只有**主题**是共享键:主题本来就是三个 app 一起变的 —— pjournal / 启动器 / 浮墨
// 读的是同一份设置、同一张表,单独给浮墨一份反而会把整台设备的配色拆散。

struct SetItem {
    std::string key;
    std::string label;
    std::string value;
};

static int g_set_sel = 0;        // 设置页选中的行
static int g_set_top = 0;        // 行列表的窗口首行
static bool g_set_pick = false;  // 选项浮层开着
static lv_obj_t *g_set_pick_dlg = nullptr;
static int g_set_pick_sel = 0;
static int g_set_pick_top = 0;
static std::string g_set_pick_key;
static std::string g_set_pick_label;
static std::vector<std::pair<std::string, std::string>> g_set_pick_opts;

// 设置行和选项里只显示文件名:全路径 37 个字符,40 号字下会把整行挤爆。
// 存进去的永远是完整路径。
static std::string font_basename(const std::string &path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

// 设置行显示的是**真正在用的**那支字体:正文要过 CJK 检查(见 flomo_setting_options),
// 存进去但用不上的取值这里会显示成实际生效的那支,不会出现「选了 NF-Mono,行里也写
// NF-Mono,但屏幕上还是 Go-Lava」这种骗人的显示。
static std::string effective_font_name() {
    return font_basename(ui_font_path());
}

static std::string bold_font_name() {
    std::string p = font_bold_setting();
    if(p.empty() || access(p.c_str(), R_OK) != 0) return "(默认)";
    return font_basename(p);
}

static std::vector<SetItem> flomo_setting_items() {
    return {
        {"theme", "主题", theme_def(g_settings.theme())->label},
        {"flomo_ui_font_size", "界面字号", std::to_string(ui_font_size())},
        {"flomo_font_size", "编辑字号", std::to_string(editor_font_size())},
        {"flomo_font_file", "正文字体", effective_font_name()},
        {"flomo_font_bold_file", "粗体字体", bold_font_name()},
    };
}

static std::vector<std::pair<std::string, std::string>> flomo_setting_options(const std::string &key) {
    std::vector<std::pair<std::string, std::string>> o;
    if(key == "theme") {
        for(const ThemeDef &t : k_themes) o.push_back({t.id, t.label});
        return o;
    }
    if(key == "flomo_ui_font_size" || key == "flomo_font_size") {
        int lo = key == "flomo_ui_font_size" ? 24 : 16;
        int hi = key == "flomo_ui_font_size" ? 60 : 72;
        for(int n = lo; n <= hi; n += 2) {
            std::string v = std::to_string(n);
            o.push_back({v, v});
        }
        return o;
    }
    if(key == "flomo_font_file" || key == "flomo_font_bold_file") {
        // 空值 = 不指定,回退到 $HOME/.fonts 下那支同名默认字体
        o.push_back({"", "(默认)"});
        for(const auto &f : g_fonts.fonts()) {
            // 正文自己必须含中文:不含的字体塞进设置只会被 find_font_path 静默换成默认
            // 字体,用户选了没效果。粗体没这条要求 —— 缺的字由 fallback 逐字形兜。
            if(key == "flomo_font_file" && !g_fonts.supports_cjk(f.path)) continue;
            o.push_back({f.path, font_basename(f.path)});
        }
        return o;
    }
    return o;
}

static std::string flomo_setting_value(const std::string &key) {
    if(key == "theme") return g_settings.theme();
    if(key == "flomo_ui_font_size") return std::to_string(ui_font_size());
    if(key == "flomo_font_size") return std::to_string(editor_font_size());
    if(key == "flomo_font_file") return font_file_setting();
    if(key == "flomo_font_bold_file") return font_bold_setting();
    return g_settings.get(key, "");
}

static void flomo_setting_apply(const std::string &key, const std::string &value) {
    g_settings.set(key, value);
    // 主题换了连 g_style 里的底色/正文色都得重挂,所以和字体走同一条路;
    // 顺便让主面板也重载配色(app_ui_reload_theme 在浮墨屏会转回 flomo::render)。
    if(key == "theme") {
        apply_theme_values();
        reload_fonts();
        app_ui_reload_theme();
        return;
    }
    reload_fonts();
    render();
}

static int set_row_h() { return ui_line_h() + ui_px(10); }

static void close_setting_pick() {
    g_set_pick = false;
    if(g_set_pick_dlg) {
        lv_obj_delete(g_set_pick_dlg);
        g_set_pick_dlg = nullptr;
    }
}

static void open_setting_pick() {
    std::vector<SetItem> items = flomo_setting_items();
    if(g_set_sel < 0 || g_set_sel >= (int)items.size()) return;
    const SetItem &it = items[g_set_sel];
    std::vector<std::pair<std::string, std::string>> opts = flomo_setting_options(it.key);
    if(opts.empty()) return;
    g_set_pick = true;
    g_set_pick_key = it.key;
    g_set_pick_label = it.label;
    g_set_pick_opts = std::move(opts);
    g_set_pick_sel = 0;
    g_set_pick_top = 0;
    const std::string cur = flomo_setting_value(it.key);
    for(size_t i = 0; i < g_set_pick_opts.size(); ++i)
        if(g_set_pick_opts[i].first == cur) { g_set_pick_sel = (int)i; break; }
    render();
}

static void open_settings() {
    g_app.status.clear();
    g_linux_ime.set_active(false);
    g_set_sel = 0;
    g_set_top = 0;
    close_setting_pick();
    g_app.view = View::Settings;
    render();
}

static void draw_settings() {
    draw_title("设置", "");
    const int top = content_top();
    const int row_h = set_row_h();
    int per = (chrome_bottom() - top) / row_h;
    if(per < 1) per = 1;

    std::vector<SetItem> items = flomo_setting_items();
    const int n = (int)items.size();
    if(g_set_sel >= n) g_set_sel = n - 1;
    if(g_set_sel < 0) g_set_sel = 0;
    if(g_set_sel < g_set_top) g_set_top = g_set_sel;
    if(g_set_sel >= g_set_top + per) g_set_top = g_set_sel - per + 1;

    const int row_w = screen_w() - 16;
    const int inner = row_w - 8;             // box_raw 的 pad 是 4
    const int label_w = inner * 45 / 100;
    const int end = std::min(n, g_set_top + per);
    for(int i = g_set_top; i < end; ++i) {
        const bool sel = (i == g_set_sel);
        lv_obj_t *row = box_raw(g_root, 8, top + (i - g_set_top) * row_h, row_w,
                                row_h - ui_px(4), sel);
        lv_obj_t *l = label_raw(row, items[i].label, 0, 0, label_w, ui_line_h() + 2);
        lv_obj_set_style_text_color(l, sel ? g_theme.bg : g_theme.fg, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);

        lv_obj_t *v = label_raw(row, items[i].value, label_w, 0, inner - label_w, ui_line_h() + 2);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(v, sel ? g_theme.bg : g_theme.muted, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    }

    if(n > per) {
        std::string pos = std::to_string(g_set_sel + 1) + "/" + std::to_string(n);
        lv_obj_t *p = label_raw(g_root, pos, screen_w() - 240, top, 232, ui_line_h() + 2);
        lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(p, g_theme.muted, 0);
    }
}

static void draw_setting_pick() {
    if(!g_set_pick) {
        if(g_set_pick_dlg) { lv_obj_delete(g_set_pick_dlg); g_set_pick_dlg = nullptr; }
        return;
    }
    if(g_set_pick_dlg) {
        lv_obj_delete(g_set_pick_dlg);
        g_set_pick_dlg = nullptr;
    }

    const int row_h = set_row_h();
    const int n = (int)g_set_pick_opts.size();
    int per = (chrome_bottom() - content_top()) / row_h - 1;   // 留一行给标题
    if(per < 3) per = 3;
    if(per > n) per = n;
    if(g_set_pick_sel < g_set_pick_top) g_set_pick_top = g_set_pick_sel;
    if(g_set_pick_sel >= g_set_pick_top + per) g_set_pick_top = g_set_pick_sel - per + 1;
    if(g_set_pick_top < 0) g_set_pick_top = 0;

    const int pad = ui_px(14);
    const int head_h = ui_line_h() + ui_px(6);
    int w = ui_px(720);
    if(w < 460) w = 460;
    if(w > screen_w() - ui_px(40)) w = screen_w() - ui_px(40);
    int h = pad * 2 + head_h + per * row_h;

    lv_obj_t *dlg = box_raw(g_root, (screen_w() - w) / 2, (chrome_bottom() - h) / 2, w, h, false);
    lv_obj_set_style_bg_color(dlg, g_theme.bg, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_t *head = label_raw(dlg, g_set_pick_label, 0, 0, w - pad * 2, ui_line_h() + 2);
    use_bold(head);
    lv_obj_set_style_text_color(head, g_theme.heading, 0);
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);

    const int list_y = head_h;
    const int inner = w - pad * 2;
    const int end = std::min(n, g_set_pick_top + per);
    for(int i = g_set_pick_top; i < end; ++i) {
        const bool sel = (i == g_set_pick_sel);
        lv_obj_t *row = box_raw(dlg, 0, list_y + (i - g_set_pick_top) * row_h, inner,
                                row_h - ui_px(4), sel);
        lv_obj_t *t = label_raw(row, g_set_pick_opts[i].second, 0, 0, inner - 8, ui_line_h() + 2);
        lv_obj_set_style_text_color(t, sel ? g_theme.bg : g_theme.fg, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    }

    lv_obj_move_foreground(dlg);
    g_set_pick_dlg = dlg;
}

static void settings_key(int key) {
    if(g_set_pick) {
        const int n = (int)g_set_pick_opts.size();
        const int per = (chrome_bottom() - content_top()) / set_row_h() - 1;
        switch(key) {
        case KEY_UP: case 'k':
            if(n) g_set_pick_sel = (g_set_pick_sel + n - 1) % n;
            render();
            return;
        case KEY_DOWN: case 'j':
            if(n) g_set_pick_sel = (g_set_pick_sel + 1) % n;
            render();
            return;
        case KEY_PAGE_UP:
            g_set_pick_sel -= per > 1 ? per : 1;
            if(g_set_pick_sel < 0) g_set_pick_sel = 0;
            render();
            return;
        case KEY_PAGE_DOWN:
            g_set_pick_sel += per > 1 ? per : 1;
            if(g_set_pick_sel >= n) g_set_pick_sel = n - 1;
            render();
            return;
        case '\n': case '\r': case KEY_RIGHT:
            if(n) {
                const std::string k = g_set_pick_key;
                const std::string v = g_set_pick_opts[g_set_pick_sel].first;
                // 先收浮层:apply 里的 render() 才不会再把它画出来
                close_setting_pick();
                flomo_setting_apply(k, v);
            }
            return;
        case 27: case KEY_LEFT:
            close_setting_pick();
            render();
            return;
        default: return;
        }
    }

    const int n = (int)flomo_setting_items().size();
    const int per = (chrome_bottom() - content_top()) / set_row_h();
    switch(key) {
    case KEY_UP: case 'k':
        if(g_set_sel > 0) { g_set_sel--; render(); }
        return;
    case KEY_DOWN: case 'j':
        if(g_set_sel + 1 < n) { g_set_sel++; render(); }
        return;
    case KEY_PAGE_UP:
        g_set_sel -= per > 1 ? per : 1;
        if(g_set_sel < 0) g_set_sel = 0;
        render();
        return;
    case KEY_PAGE_DOWN:
        g_set_sel += per > 1 ? per : 1;
        if(g_set_sel >= n) g_set_sel = n - 1;
        render();
        return;
    case KEY_HOME:
        g_set_sel = 0;
        render();
        return;
    case KEY_END:
        g_set_sel = n - 1;
        render();
        return;
    case '\n': case '\r': case KEY_RIGHT: open_setting_pick(); return;
    case 27: case KEY_LEFT: goto_view(View::List); return;
    default: return;
    }
}

// 单行输入界面:标题 + 一行输入框 + 候选条
static void draw_input_view(const std::string &title, const std::string &hint,
                            const std::string &initial, bool password) {
    draw_title(title, "");
    int y = content_top() + ui_px(24);
    int h = ui_line_h() + ui_px(24);
    int lim = chrome_bottom() - ime_bar_h() - ui_px(6);
    if(y + h > lim) h = lim - y;
    if(h < ui_px(30)) h = ui_px(30);
    if(y + h > lim) y = lim - h;
    if(y < ui_px(8)) y = ui_px(8);

    lv_obj_t *ta = new_textarea(g_root, 8, y, screen_w() - 16, h);
    lv_textarea_set_one_line(ta, true);
    if(password) lv_textarea_set_password_mode(ta, true);
    if(!hint.empty()) lv_textarea_set_placeholder_text(ta, hint.c_str());
    if(!initial.empty()) lv_textarea_set_text(ta, initial.c_str());
    focus_ta(ta);
    update_ime_bar();
}

static void render() {
    // lv_obj_clean 会把 g_root 的**所有**子对象删掉,浮层也在其中。指针必须先作废,
    // 否则后面 draw_modal()/backlight_osd_hide() 会去删已经释放的对象(野指针崩溃)。
    g_status = nullptr;
    g_status_right = nullptr;
    g_ta = nullptr;
    g_detail_body = nullptr;
    g_ime_bar = nullptr;
    g_ime_head = nullptr;
    g_ime_cands = nullptr;
    g_ime_rule = nullptr;
    g_modal_dlg = nullptr;
    g_set_pick_dlg = nullptr;
    g_bl_osd = nullptr;
    g_bl_osd_fill = nullptr;
    g_bl_osd_pct = nullptr;
    g_md_view = nullptr;
    g_md_content = nullptr;
    g_md_caret = nullptr;
    g_md_labels.clear();
    g_md_hl_rects.clear();
    g_md_tap_lines.clear();
    g_md_caret_x = -1;
    g_md_caret_y = -1;
    bool osd_was_up = g_bl_osd_until != 0;

    lv_obj_clean(g_root);

    switch(g_app.view) {
    case View::List: draw_list(); break;
    case View::Detail: draw_detail(); break;
    case View::Edit: draw_edit(); break;
    case View::Search: draw_input_view("搜索", "输入关键词", g_app.query, false); break;
    case View::Tags: draw_tags(); break;
    case View::TagFilter:
        draw_input_view("按标签筛选", "标签名(不带 #)", g_app.tag, false);
        break;
    case View::TagRename:
        draw_input_view("重命名标签", "新标签名(不带 #)", g_tagRenameOld, false);
        break;
    case View::Review: draw_review(); break;
    case View::Related: draw_related(); break;
    case View::Help: draw_help(); break;
    case View::Settings: draw_settings(); break;
    case View::LoginEmail: draw_input_view("登录 · 邮箱", "邮箱地址", g_app.loginEmail, false); break;
    case View::LoginPassword: draw_input_view("登录 · 密码", "密码", "", true); break;
    case View::LoginToken: draw_input_view("登录 · Token", "粘贴 web token", g_loginToken, true); break;
    }

    draw_status_bar(g_app.status.empty() ? hint_for_view() : g_app.status, status_right_text());
    draw_modal();
    if(g_set_pick) draw_setting_pick();
    if(osd_was_up) draw_backlight_osd();
}

// ===========================================================================
// 模态框(删除确认 / 编辑退出)
// ===========================================================================
// 浮层不重绘整个界面:只在 g_root 上叠一个框,关掉时单独删掉。整片重绘会把编辑器
// 的 textarea 重建一遍,光标和正在输的内容全丢。

static void hide_modal() {
    g_modalKind = 0;
    if(g_modal_dlg) {
        lv_obj_delete(g_modal_dlg);
        g_modal_dlg = nullptr;
    }
}

static void draw_modal() {
    if(g_modalKind == 0) {
        hide_modal();
        return;
    }
    if(g_modal_dlg) {
        lv_obj_delete(g_modal_dlg);
        g_modal_dlg = nullptr;
    }

    std::vector<std::pair<std::string, bool>> lines;
    if(g_modalKind == 1) {
        const Memo *m = selected_memo();
        std::string preview = m ? memo_body_line(*m) : "";
        if(preview.size() > 30) preview = preview.substr(0, 30) + "…";
        lines = {{"删除这条笔记?", false},
                 {preview, true},
                 {"Enter 删除 · Esc 取消", false}};
    } else {
        lines = {{"正文有改动", false},
                 {"Ctrl+S 保存 · d 放弃 · Esc 取消", false}};
    }

    int w = ui_px(640);
    if(w < 460) w = 460;
    if(w > screen_w() - ui_px(40)) w = screen_w() - ui_px(40);
    int lh = ui_line_h();
    int gap = ui_px(8);
    int pad = ui_px(14);
    int h = pad * 2 + (int)lines.size() * (lh + gap);
    lv_obj_t *dlg = box_raw(g_root, (screen_w() - w) / 2, (chrome_bottom() - h) / 2, w, h, false);
    lv_obj_set_style_bg_color(dlg, g_theme.bg, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    for(size_t i = 0; i < lines.size(); ++i) {
        lv_obj_t *t = label_raw(dlg, lines[i].first, ui_px(8), pad + (int)i * (lh + gap),
                                w - ui_px(16), lh + 2);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        if(i == 0) use_bold(t);
        else if(lines[i].second) lv_obj_set_style_text_color(t, g_theme.muted, 0);
    }
    lv_obj_move_foreground(dlg);
    g_modal_dlg = dlg;
}

static void show_delete_confirm() {
    g_modalKind = 1;
    draw_modal();
}

static void show_edit_exit() {
    g_modalKind = 2;
    draw_modal();
}

// ===========================================================================
// 背光浮层(顶排 ◯/⬠):与 pjournal / Launcher 逐字一致
// ===========================================================================

// 最低 5%:背光是 raw PWM,又没有独立亮度键,调到全黑就再也摸不回来了。
static const int kBacklightLevels[] = {5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
static constexpr int kBacklightLevelCount =
    (int)(sizeof(kBacklightLevels) / sizeof(kBacklightLevels[0]));

static int backlight_display_percent() {
    int pct = g_settings.backlight_percent();
    if(pct > 0) return pct;
    int cur = backlight_current_percent();
    return cur > 0 ? cur : 100;
}

static int backlight_level_index() {
    int cur = backlight_display_percent();
    int best = 0;
    for(int i = 0; i < kBacklightLevelCount; ++i) {
        int a = kBacklightLevels[i] - cur, b = kBacklightLevels[best] - cur;
        if(a < 0) a = -a;
        if(b < 0) b = -b;
        if(a < b) best = i;
    }
    return best;
}

static void backlight_osd_apply(int idx) {
    if(idx < 0) idx = 0;
    if(idx >= kBacklightLevelCount) idx = kBacklightLevelCount - 1;
    if(g_settings.backlight_percent() == kBacklightLevels[idx]) return;
    g_settings.set("backlight", std::to_string(kBacklightLevels[idx]));
    backlight_set_percent(kBacklightLevels[idx]);
}

static constexpr uint32_t kBacklightOsdMs = 1600;

static void backlight_osd_hide() {
    g_bl_osd_until = 0;
    if(g_bl_osd) {
        lv_obj_delete(g_bl_osd);
        g_bl_osd = nullptr;
    }
    g_bl_osd_fill = nullptr;
    g_bl_osd_pct = nullptr;
}

static void draw_backlight_osd() {
    if(g_bl_osd) {
        lv_obj_delete(g_bl_osd);
        g_bl_osd = nullptr;
    }
    g_bl_osd_fill = nullptr;
    g_bl_osd_pct = nullptr;

    int w = ui_px(300);
    if(w < 240) w = 240;
    if(w > screen_w() - ui_px(40)) w = screen_w() - ui_px(40);
    int pad = ui_px(12);
    int lh = ui_line_h();
    int th = lh + 2;
    int bar_h = ui_px(16);
    if(bar_h < 8) bar_h = 8;
    int gap = ui_px(10);
    int h = pad * 2 + th + gap + bar_h + 8;

    int x = (screen_w() - w) / 2;
    int y = chrome_bottom() - h - ui_px(24);
    if(y < ui_px(8)) y = ui_px(8);

    lv_obj_t *osd = box_raw(g_root, x, y, w, h, false);
    lv_obj_set_style_bg_color(osd, g_theme.bg, 0);
    lv_obj_set_style_border_width(osd, 2, 0);

    label_raw(osd, "背光", pad, pad, w - pad * 2 - 8, th);
    g_bl_osd_pct = label_raw(osd, "", pad, pad, w - pad * 2 - 8, th);
    lv_obj_set_style_text_align(g_bl_osd_pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(g_bl_osd_pct, g_theme.muted, 0);

    int bar_y = pad + th + gap;
    int bar_w = w - pad * 2 - 8;
    box_raw(osd, pad, bar_y, bar_w, bar_h, false);
    g_bl_osd_track_w = bar_w - 2;
    g_bl_osd_bar_h = bar_h - 2;
    g_bl_osd_fill = box_raw(osd, pad + 1, bar_y + 1, 1, g_bl_osd_bar_h, false);
    lv_obj_set_style_bg_color(g_bl_osd_fill, g_theme.accent, 0);
    lv_obj_set_style_border_width(g_bl_osd_fill, 0, 0);

    lv_obj_move_foreground(osd);
    g_bl_osd = osd;

    int pct = backlight_display_percent();
    int fill = g_bl_osd_track_w * pct / 100;
    if(fill < 1) fill = 1;
    lv_obj_set_width(g_bl_osd_fill, fill);
    lv_label_set_text(g_bl_osd_pct, (std::to_string(pct) + "%").c_str());
    g_bl_osd_until = lv_tick_get() + kBacklightOsdMs;
}

static void backlight_osd_step(int dir) {
    int idx = backlight_level_index() + (dir > 0 ? 1 : -1);
    backlight_osd_apply(idx);
    draw_backlight_osd();
}

// ===========================================================================
// 视图切换 / 动作
// ===========================================================================

static void goto_view(View v) {
    g_app.view = v;
    g_app.status.clear();
    render();
}

static void open_detail() {
    if(!selected_memo()) return;
    g_app.view = View::Detail;
    g_app.status.clear();
    render();
}

static int memo_index_by_slug(const std::string &slug) {
    if(slug.empty()) return -1;
    for(size_t i = 0; i < g_app.memos.size(); ++i)
        if(g_app.memos[i].slug == slug) return (int)i;
    return -1;
}

// 把某条笔记放进当前筛选结果并选中它(详情页只能从 filtered 里取)。
static bool select_slug(const std::string &slug) {
    int idx = memo_index_by_slug(slug);
    if(idx < 0) return false;
    for(size_t k = 0; k < g_app.filtered.size(); ++k) {
        if(g_app.filtered[k] == idx) {
            g_app.selected = (int)k;
            return true;
        }
    }
    g_app.query.clear();
    g_app.tag.clear();
    rebuild_filter(g_app);
    for(size_t k = 0; k < g_app.filtered.size(); ++k) {
        if(g_app.filtered[k] == idx) {
            g_app.selected = (int)k;
            return true;
        }
    }
    return false;
}

static void start_edit(const std::string &slug) {
    g_editSlug = slug;
    int idx = memo_index_by_slug(slug);
    g_editText = idx >= 0 ? g_app.memos[idx].contentText : std::string();
    g_linux_ime.set_active(true);
    g_app.view = View::Edit;
    g_app.status.clear();
    render();
}

static void move_sel(int delta) {
    int n = (int)g_app.filtered.size();
    if(n <= 0) return;
    int v = g_app.selected + delta;
    if(v < 0) v = 0;
    if(v >= n) v = n - 1;
    if(v == g_app.selected) return;
    g_app.selected = v;
    g_app.status.clear();
    render();
}

static void move_sel_page(int dir) {
    int per = (chrome_bottom() - content_top()) / card_h();
    if(per < 1) per = 1;
    move_sel(dir * per);
}

static void scroll_detail(int delta) {
    if(!g_detail_body) return;
    // scroll_by 不夹边界;内容不足一屏时也能被推到无限远,必须用 bounded。
    lv_obj_scroll_by_bounded(g_detail_body, 0, delta, LV_ANIM_OFF);
}

static void request_sync(const char *label) {
    Store s = g_store;
    s.memos = g_app.memos;
    if(s.token.empty()) {
        g_app.status = "未登录 · 按 l 登录";
        set_status_left(g_app.status);
        return;
    }
    if(sync_start(s)) {
        g_app.status = label;
        set_status_left(g_app.status);
    }
}

static void toggle_pin() {
    const Memo *sel = selected_memo();
    if(!sel) return;
    const int idx = g_app.filtered[g_app.selected];
    Memo &m = g_app.memos[idx];
    m.pin = m.pinned() ? 0 : 1;
    m.dirty = true;
    if(m.pendingOp != "create") m.pendingOp = "update";
    save_db();
    g_app.status = m.pinned() ? "已置顶" : "已取消置顶";
    render();
    request_sync("同步中…");
}

static void request_review() {
    if(g_store.token.empty()) {
        g_app.status = "未登录 · 按 l 登录";
        set_status_left(g_app.status);
        return;
    }
    if(review_start(g_store.token)) {
        g_reviewLoading = true;
        g_app.status = "今日回顾加载中…";
    } else {
        g_app.status = "回顾正在加载,稍等";
    }
    set_status_left(g_app.status);
}

static void request_related() {
    const Memo *sel = selected_memo();
    if(!sel) return;
    if(g_store.token.empty()) {
        g_app.status = "未登录 · 按 l 登录";
        render();
        return;
    }
    if(isLocalOnly(*sel) || sel->slug.empty()) {
        g_app.status = "这条还没同步,先按 r 同步";
        render();
        return;
    }
    const std::string origin = sel->slug;
    g_relatedOrigin = origin;
    g_app.status = "相关笔记加载中…";
    render();
    lv_refr_now(nullptr);   // 单次请求,阻塞几秒可以接受,先把「加载中」推上屏

    FlomoApi api(g_store.token);
    ApiResult r = api.related(origin);
    if(!r.ok) {
        g_app.status = "相关笔记失败: " + r.message;
        render();
        return;
    }
    g_relatedSlugs.clear();
    const JsonValue &arr = r.data;
    if(arr.isArray()) {
        for(size_t i = 0; i < arr.size(); ++i) {
            const JsonValue &item = arr[i];
            Memo rm = memo_from_json(item["memo"]);
            if(rm.slug.empty()) rm = memo_from_json(item);
            if(rm.slug.empty() || rm.deleted || rm.slug == origin) continue;
            upsert_memo(g_app.memos, rm);
            if(std::find(g_relatedSlugs.begin(), g_relatedSlugs.end(), rm.slug) == g_relatedSlugs.end())
                g_relatedSlugs.push_back(rm.slug);
        }
    }
    sort_memos(g_app.memos);
    g_app.query.clear();
    g_app.tag.clear();
    rebuild_filter(g_app);
    g_relatedSel = 0;
    g_relatedTop = 0;
    g_app.status = "相关笔记 " + std::to_string(g_relatedSlugs.size()) + " 条";
    g_app.view = View::Related;
    render();
}

// ---- 标签 ----

static void add_tag_with_parents(const std::string &tag, std::vector<std::string> &out) {
    if(tag.empty()) return;
    out.push_back(tag);
    for(size_t p = tag.find('/'); p != std::string::npos; p = tag.find('/', p + 1))
        out.push_back(tag.substr(0, p));
}

static void collect_tag_tree(const JsonValue &v, std::vector<std::string> &out, int depth) {
    if(depth > 8 || out.size() > 2000) return;
    if(v.isArray()) {
        for(size_t i = 0; i < v.size(); ++i) collect_tag_tree(v[i], out, depth + 1);
        return;
    }
    if(!v.isObject()) return;
    std::string name;
    for(const char *k : {"tag", "name", "tag_name", "text"}) {
        const JsonValue &n = v[k];
        if(n.isString() && !n.asString().empty()) {
            name = n.asString();
            break;
        }
    }
    if(!name.empty()) add_tag_with_parents(name, out);
    for(const char *k : {"children", "child", "list", "tags"}) {
        const JsonValue &kids = v[k];
        if(kids.isArray() || kids.isObject()) collect_tag_tree(kids, out, depth + 1);
    }
}

static void open_tags() {
    std::vector<std::string> names;
    for(const auto &m : g_app.memos) {
        if(m.deleted) continue;
        for(const auto &t : m.tags) add_tag_with_parents(t, names);
    }
    g_app.status = g_store.token.empty() ? "" : "标签加载中…";
    g_app.view = View::Tags;
    g_tagSel = 0;
    g_tagTop = 0;
    render();
    if(g_store.token.empty()) return;
    lv_refr_now(nullptr);

    FlomoApi api(g_store.token);
    ApiResult r = api.tag_tree();
    if(r.ok) collect_tag_tree(r.data, names, 0);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    g_tagList = std::move(names);
    g_app.status.clear();
    render();
}

static std::string trim_spaces(const std::string &s) {
    size_t b = 0, e = s.size();
    while(b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while(e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

static std::string strip_tag_hash(std::string s) {
    s = trim_spaces(s);
    if(!s.empty() && s[0] == '#') s.erase(0, 1);
    return trim_spaces(s);
}

// 本地把所有 old 及 old/… 前缀改成 new,正文里的 #old 也一起换。
static void apply_tag_rename_local(const std::string &oldTag, const std::string &newTag) {
    const std::string oldPrefix = oldTag + "/";
    const std::string oldHash = "#" + oldTag;
    for(auto &m : g_app.memos) {
        bool changed = false;
        for(auto &t : m.tags) {
            if(t == oldTag) {
                t = newTag;
                changed = true;
            } else if(t.compare(0, oldPrefix.size(), oldPrefix) == 0) {
                t = newTag + t.substr(oldTag.size());
                changed = true;
            }
        }
        size_t pos = 0;
        while((pos = m.contentText.find(oldHash, pos)) != std::string::npos) {
            size_t after = pos + oldHash.size();
            char c = after < m.contentText.size() ? m.contentText[after] : ' ';
            if(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '#') {
                m.contentText.replace(pos, oldHash.size(), "#" + newTag);
                pos += newTag.size() + 1;
                changed = true;
            } else {
                pos = after;
            }
        }
        if(!changed) continue;
        m.contentHtml = text_to_html(m.contentText);
        m.updatedAt = now_stamp();
        m.dirty = true;
        if(m.pendingOp != "create") m.pendingOp = "update";
    }
}

static void do_tag_rename(const std::string &rawNew) {
    const std::string newTag = strip_tag_hash(rawNew);
    const std::string oldTag = g_tagRenameOld;
    if(newTag.empty() || oldTag.empty() || newTag == oldTag) {
        g_app.status = newTag == oldTag ? "标签名没有变化" : "标签名不能为空";
        goto_view(View::Tags);
        return;
    }

    if(!g_store.token.empty()) {
        g_app.status = "改名中…";
        goto_view(View::Tags);
        lv_refr_now(nullptr);
        FlomoApi api(g_store.token);
        ApiResult r = api.rename_tag(oldTag, newTag);
        if(!r.ok) {
            g_app.status = "改名失败: " + r.message;
            render();
            return;
        }
    }
    apply_tag_rename_local(oldTag, newTag);
    save_db();
    rebuild_filter(g_app);
    open_tags();   // 重新汇总标签列表(会再拉一次 tag/tree),顺便切回标签页
    request_sync("标签已改名 · 同步中…");
}

static void do_token_login(const std::string &raw) {
    const std::string token = trim_spaces(raw);
    g_linux_ime.set_active(false);
    if(token.empty()) {
        g_app.status = "Token 不能为空";
        goto_view(View::List);
        return;
    }
    g_app.status = "校验 Token…";
    g_app.view = View::List;
    render();
    lv_refr_now(nullptr);

    FlomoApi api(token);
    ApiResult r = api.user_me();
    if(!r.ok) {
        g_app.status = "Token 无效: " + r.message;
        render();
        return;
    }
    g_store.token = token;
    g_settings.set("flomo_token", token);   // 和旧的 Ctrl+F 发送共用
    save_db();
    g_app.status = "登录成功";
    render();
    request_sync("登录成功 · 同步中…");
}

// 本地新建的笔记还没有远端 slug,先给个占位。必须**每次都不一样**:同步回填用的是
// slug 查找,两条笔记撞 slug 会让后一条永远找不回来(静默丢笔记)。
static std::string make_local_slug() {
    static unsigned counter = 0;
    return "local-" + std::to_string((long long)std::time(nullptr)) + "-" +
           std::to_string(++counter);
}

// 保存编辑器。返回是否真的写入了东西(空内容不建笔记)。
static bool commit_editor() {
    if(g_editText.empty()) return false;
    int idx = memo_index_by_slug(g_editSlug);
    Memo m;
    if(idx >= 0) m = g_app.memos[idx];
    else {
        m.slug = make_local_slug();
        m.createdAt = now_stamp();
    }
    m.contentText = g_editText;
    m.contentHtml = text_to_html(g_editText);
    m.tags = extract_tags(g_editText);
    m.updatedAt = now_stamp();
    m.dirty = true;
    m.pendingOp = idx >= 0 ? "update" : "create";
    if(idx >= 0) g_app.memos[idx] = m;
    else g_app.memos.insert(g_app.memos.begin(), m);
    sort_memos(g_app.memos);
    save_db();
    rebuild_filter(g_app);
    return true;
}

static void delete_current() {
    const Memo *m = selected_memo();
    if(!m) {
        hide_modal();
        render();
        return;
    }
    int idx = g_app.filtered[g_app.selected];
    if(isLocalOnly(g_app.memos[idx])) {
        // 本地新建、还没拿到远端 slug 的:删了就是删了,不用惊动服务器
        g_app.memos.erase(g_app.memos.begin() + idx);
        g_app.status = "已删除(本地)";
    } else {
        Memo &target = g_app.memos[idx];
        target.deleted = true;
        target.dirty = true;
        target.pendingOp = "delete";
        g_app.status = "已标记删除 · 按 r 同步";
    }
    hide_modal();
    save_db();
    rebuild_filter(g_app);
    render();
}

// 从编辑器退出到列表。save 决定改动是保存、放弃还是取消。
static void leave_editor(int action) {   // 0 = 取消,1 = 保存,2 = 放弃
    const char *cur = g_ta ? lv_textarea_get_text(g_ta) : nullptr;
    if(cur) g_editText = cur;
    hide_modal();
    g_linux_ime.set_active(false);

    if(action == 0) {
        g_app.view = View::Edit;
        render();
        return;
    }
    // g_editSlug 必须活到 commit_editor() 里面 —— 它是「这次在改哪一条」的唯一依据。
    // 提前清掉的话每次编辑都会走成新建,原地多出一条笔记。
    bool saved = action == 1 && commit_editor();
    g_editSlug.clear();
    if(saved) {
        g_app.status = "已保存";
        g_app.view = View::List;
        render();
        request_sync("同步中…");
        return;
    }
    g_app.status = action == 1 ? "内容为空,未保存" : "改动已放弃";
    g_app.view = View::List;
    render();
}

static void quit_to_launcher() {
    // 编辑器里有内容就先并回 store,再落盘 —— 返回主菜单不弹确认框。
    // 提交后把编辑态收掉,免得 leave() 再提交一次(本地新建会被插成两条)。
    if(g_app.view == View::Edit) {
        const char *cur = g_ta ? lv_textarea_get_text(g_ta) : nullptr;
        if(cur) g_editText = cur;
        commit_editor();   // 空内容时什么都不做
        g_editSlug.clear();
        g_app.view = View::List;
    }
    save_db();
    app_ui_return_to_main();
}

// ===========================================================================
// 输入按键
// ===========================================================================

// 返回 true 表示这个键被输入法吃掉了(上屏串已经写进 textarea)。
static bool ime_key(int key, lv_obj_t *ta) {
    if(!ta) return false;
    if(key == KEY_IME_TOGGLE) {
        g_linux_ime.toggle();
        update_ime_bar();
        return true;
    }
    std::string out;
    if(g_linux_ime.handle_key(key, out)) {
        if(!out.empty()) lv_textarea_add_text(ta, out.c_str());
        update_ime_bar();
        return true;
    }
    return false;
}

// 光标/删除/可打印字符。multiline 决定 Enter 是换行(返回 true)还是交给调用方。
static bool plain_text_key(int key, lv_obj_t *ta, bool multiline) {
    if(!ta) return false;
    if(key == KEY_LEFT) { lv_textarea_cursor_left(ta); return true; }
    if(key == KEY_RIGHT) { lv_textarea_cursor_right(ta); return true; }
    if(key == KEY_UP) { if(multiline) lv_textarea_cursor_up(ta); return true; }
    if(key == KEY_DOWN) { if(multiline) lv_textarea_cursor_down(ta); return true; }
    if(key == KEY_HOME) { lv_textarea_set_cursor_pos(ta, 0); return true; }
    if(key == KEY_END) { lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); return true; }
    if(key == 8) { lv_textarea_delete_char(ta); return true; }
    if(key == 127) { lv_textarea_delete_char_forward(ta); return true; }
    if(key == '\n' || key == '\r') {
        if(multiline) {
            lv_textarea_add_char(ta, '\n');
            return true;
        }
        return false;
    }
    if(key >= 32 && key < 127) {
        lv_textarea_add_char(ta, (uint32_t)key);
        return true;
    }
    return false;
}

static bool editor_modified() {
    if(!g_ta) return false;
    const char *cur = lv_textarea_get_text(g_ta);
    return cur && g_editText != cur;
}

static void edit_key(int key) {
    if(key == 27) {   // Esc
        if(editor_modified()) show_edit_exit();
        else leave_editor(2);
        return;
    }
    if(key == 19) {   // Ctrl+S
        leave_editor(1);
        return;
    }
    if(ime_key(key, g_ta)) return;
    if(plain_text_key(key, g_ta, true)) return;
}

// 单行输入界面(搜索/标签/登录)。Enter 由 submit 决定下一步。
static void input_key(int key, const std::function<void(const std::string &)> &submit) {
    if(key == 27) {
        g_linux_ime.set_active(false);
        // 标签筛选/改名是从标签列表进来的,Esc 回标签列表而不是直接回笔记列表。
        if(g_app.view == View::TagFilter || g_app.view == View::TagRename)
            goto_view(View::Tags);
        else if(g_app.view == View::LoginPassword)
            goto_view(View::LoginEmail);
        else
            goto_view(View::List);
        return;
    }
    if(ime_key(key, g_ta)) return;
    if(key == '\n' || key == '\r') {
        const char *cur = lv_textarea_get_text(g_ta);
        submit(cur ? cur : "");
        return;
    }
    if(plain_text_key(key, g_ta, false)) return;
}

static void do_login(const std::string &password) {
    g_linux_ime.set_active(false);
    g_app.status = "登录中…";
    g_app.view = View::List;
    render();
    // 登录是一次性的显式动作,阻塞几秒可以接受;先把已画好的画面推上屏,别看着像死机。
    // (curl 子进程自己有 40s 看门狗,不会无限等。)
    lv_refr_now(nullptr);

    ApiResult r = FlomoApi::login(g_app.loginEmail, password);
    if(!r.ok) {
        g_app.status = "登录失败: " + r.message;
        render();
        return;
    }
    std::string token = r.data["access_token"].asString();
    if(token.empty()) {
        g_app.status = "登录响应里没有 access_token";
        render();
        return;
    }
    g_store.token = token;
    g_settings.set("flomo_token", token);   // 和旧的 Ctrl+F 发送共用
    save_db();
    g_app.status = "登录成功";
    render();
    request_sync("登录成功 · 同步中…");
}

// ===========================================================================
// 主按键分发
// ===========================================================================

static void tags_key(int key) {
    const int n = (int)g_tagList.size();
    int per = (chrome_bottom() - content_top()) / tag_row_h();
    if(per < 1) per = 1;
    switch(key) {
    case KEY_UP: case 'k':
        if(n) g_tagSel = (g_tagSel + n - 1) % n;
        render();
        return;
    case KEY_DOWN: case 'j':
        if(n) g_tagSel = (g_tagSel + 1) % n;
        render();
        return;
    case KEY_PAGE_UP:
        g_tagSel -= per;
        if(g_tagSel < 0) g_tagSel = 0;
        render();
        return;
    case KEY_PAGE_DOWN:
        g_tagSel += per;
        if(g_tagSel >= n) g_tagSel = n > 0 ? n - 1 : 0;
        render();
        return;
    case KEY_HOME: g_tagSel = 0; render(); return;
    case KEY_END: g_tagSel = n > 0 ? n - 1 : 0; render(); return;
    case '\n': case '\r':
        if(n) {
            g_app.tag = g_tagList[g_tagSel];
            g_app.query.clear();
            rebuild_filter(g_app);
            g_app.selected = 0;
            g_listTop = 0;
            g_app.status = "按 #" + g_app.tag + " 筛选";
            goto_view(View::List);
            set_status_left(g_app.status);
        }
        return;
    case 'r': case 'R':
        if(n) {
            g_tagRenameOld = g_tagList[g_tagSel];
            g_linux_ime.set_active(true);
            g_app.status.clear();
            g_app.view = View::TagRename;
            render();
        }
        return;
    case 'f': case 'F': case '/':
        g_linux_ime.set_active(true);
        g_app.status.clear();
        g_app.view = View::TagFilter;
        render();
        return;
    case 27: case KEY_LEFT: goto_view(View::List); return;
    default: return;
    }
}

static void slug_list_key(int key, std::vector<std::string> &slugs, int &sel, int &top, View self) {
    const int n = (int)slugs.size();
    int per = (chrome_bottom() - content_top()) / card_h();
    if(per < 1) per = 1;
    switch(key) {
    case KEY_UP: case 'k':
        if(n) sel = (sel + n - 1) % n;
        render();
        return;
    case KEY_DOWN: case 'j':
        if(n) sel = (sel + 1) % n;
        render();
        return;
    case KEY_PAGE_UP:
        sel -= per;
        if(sel < 0) sel = 0;
        render();
        return;
    case KEY_PAGE_DOWN:
        sel += per;
        if(sel >= n) sel = n > 0 ? n - 1 : 0;
        render();
        return;
    case KEY_HOME: sel = 0; render(); return;
    case KEY_END: sel = n > 0 ? n - 1 : 0; render(); return;
    case '\n': case '\r':
        if(n && select_slug(slugs[sel])) {
            g_detailReturn = self;
            g_app.status.clear();
            g_app.view = View::Detail;
            render();
        }
        return;
    case 'r': case 'R':
        if(self == View::Review) request_review();
        else request_related();
        return;
    case 27: case KEY_LEFT:
        if(self == View::Related && !g_relatedOrigin.empty() && select_slug(g_relatedOrigin)) {
            g_detailReturn = View::Related;
            g_app.status.clear();
            g_app.view = View::Detail;
            render();
        } else {
            goto_view(View::List);
        }
        return;
    default: return;
    }
}

static void handle_view_key(int key) {
    switch(g_app.view) {
    // 编辑器每按一个键都要重排一次叠加层。edit_key() 里可能已经离开编辑器
    // (Esc/Ctrl+S → render()),那时 g_md_view 已经被置空,md_refresh() 自己会跳过。
    case View::Edit: edit_key(key); md_refresh(); return;
    case View::Settings: settings_key(key); return;
    case View::Tags: tags_key(key); return;
    case View::Review: slug_list_key(key, g_reviewSlugs, g_reviewSel, g_reviewTop, View::Review); return;
    case View::Related: slug_list_key(key, g_relatedSlugs, g_relatedSel, g_relatedTop, View::Related); return;
    case View::Search:
        input_key(key, [](const std::string &s) {
            g_linux_ime.set_active(false);
            g_app.query = s;
            rebuild_filter(g_app);
            g_app.selected = 0;
            g_listTop = 0;
            goto_view(View::List);
        });
        return;
    case View::TagFilter:
        input_key(key, [](const std::string &s) {
            g_linux_ime.set_active(false);
            g_app.tag = strip_tag_hash(s);
            g_app.query.clear();
            rebuild_filter(g_app);
            g_app.selected = 0;
            g_listTop = 0;
            goto_view(View::List);
        });
        return;
    case View::TagRename:
        input_key(key, [](const std::string &s) { do_tag_rename(s); });
        return;
    case View::LoginEmail:
        input_key(key, [](const std::string &s) {
            g_app.loginEmail = s;
            g_app.view = View::LoginPassword;
            g_app.status.clear();
            render();
        });
        return;
    case View::LoginPassword:
        input_key(key, [](const std::string &s) { do_login(s); });
        return;
    case View::LoginToken:
        input_key(key, [](const std::string &s) { do_token_login(s); });
        return;
    default: break;
    }

    // 详情
    if(g_app.view == View::Detail) {
        switch(key) {
        case KEY_UP: case 'k': scroll_detail(-ui_line_h()); return;
        case KEY_DOWN: case 'j': scroll_detail(ui_line_h()); return;
        case KEY_PAGE_UP: scroll_detail(-(chrome_bottom() - content_top())); return;
        case KEY_PAGE_DOWN: scroll_detail(chrome_bottom() - content_top()); return;
        case KEY_HOME: scroll_detail(-100000); return;
        case KEY_END: scroll_detail(100000); return;
        case 'e': case 'E':
            if(!g_app.filtered.empty())
                start_edit(g_app.memos[g_app.filtered[g_app.selected]].slug);
            return;
        case 'd': case 'D':
            if(!g_app.filtered.empty()) {
                g_app.status.clear();
                show_delete_confirm();
            }
            return;
        case 'p': case 'P': toggle_pin(); return;
        case 'r': case 'R': request_related(); return;
        case 'h': case 'H': case KEY_HELP: goto_view(View::Help); return;
        case 27: case KEY_LEFT: goto_view(g_detailReturn); return;
        default: return;
        }
    }
    if(g_app.view == View::Help) {
        if(key == 27 || key == KEY_LEFT || key == 'h' || key == '\n' || key == '\r')
            goto_view(View::List);
        return;
    }

    // 列表。hjkl 里只留 j/k(上下);h 是帮助、l 是登录 —— 和原 Flomo 一致。
    switch(key) {
    case KEY_UP: case 'k': move_sel(-1); return;
    case KEY_DOWN: case 'j': move_sel(1); return;
    case KEY_LEFT: case KEY_RIGHT: return;   // 列表是一维的,左右不做事
    case KEY_PAGE_UP: move_sel_page(-1); return;
    case KEY_PAGE_DOWN: move_sel_page(1); return;
    case KEY_HOME:
        if(!g_app.filtered.empty()) { g_app.selected = 0; g_listTop = 0; render(); }
        return;
    case KEY_END:
        if(!g_app.filtered.empty()) {
            g_app.selected = (int)g_app.filtered.size() - 1;
            render();
        }
        return;
    case 27:
        // 有筛选时先清筛选;再按一次回主菜单。
        if(!g_app.query.empty() || !g_app.tag.empty()) {
            g_app.query.clear();
            g_app.tag.clear();
            rebuild_filter(g_app);
            g_app.selected = 0;
            g_listTop = 0;
            g_app.status = "筛选已清除";
            render();
        } else {
            quit_to_launcher();
        }
        return;
    case 'q': case 'Q': quit_to_launcher(); return;
    case '\n': case '\r':
        g_detailReturn = View::List;
        open_detail();
        return;
    case KEY_SEARCH: case '/':
        g_app.status.clear();
        g_linux_ime.set_active(true);
        g_app.view = View::Search;
        render();
        return;
    case 's': case 'S':
        open_settings();
        return;
    case 'n': case 'N': start_edit(std::string()); return;
    case 'e': case 'E':
        if(!g_app.filtered.empty()) start_edit(g_app.memos[g_app.filtered[g_app.selected]].slug);
        return;
    case 'd': case 'D':
        if(!g_app.filtered.empty()) {
            g_app.status.clear();
            show_delete_confirm();
        }
        return;
    case 'p': case 'P': toggle_pin(); return;
    case 'y': case 'Y': request_review(); return;
    case 't': case 'T':
        open_tags();
        return;
    case 'r': case 'R': request_sync("同步中…"); return;
    case 'l':
        g_linux_ime.set_active(false);
        g_app.status.clear();
        g_app.view = View::LoginEmail;
        render();
        return;
    case 'L':
        g_linux_ime.set_active(false);
        g_loginToken.clear();
        g_app.status.clear();
        g_app.view = View::LoginToken;
        render();
        return;
    case 'h': case 'H': case KEY_HELP: goto_view(View::Help); return;
    default: return;
    }
}

// 状态栏右侧(时间/WiFi/电池)每 5 秒刷一次。tick 每 5ms 跑一次,这里自带节流。
static void status_right_refresh() {
    static std::string last_right;
    static uint32_t next_ms = 0;
    uint32_t now = lv_tick_get();
    if(next_ms != 0 && (int32_t)(now - next_ms) < 0) return;
    next_ms = now + 5000;
    std::string right = status_right_text();
    if(right != last_right && g_status_right) lv_label_set_text(g_status_right, right.c_str());
    last_right = right;
}

// ===========================================================================
// 对外接口(include/flomo_screen.h)
// ===========================================================================

namespace flomo {

void create() {
    apply_theme_values();
    g_root = lv_screen_active();
    lv_obj_remove_style_all(g_root);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    reload_fonts();

    g_app = AppState {};
    g_app.view = View::List;
    g_editSlug.clear();
    g_editText.clear();
    g_reviewLoading = false;
    g_reviewSlugs.clear();
    g_relatedSlugs.clear();
    g_relatedOrigin.clear();
    g_tagList.clear();

    flomo_db_load(g_store);
    g_app.memos = g_store.memos;
    rebuild_filter(g_app);

    render();
}

void render() {
    ::render();
}

void handle_key(int key) {
    if(key == KEY_BACKLIGHT_UP || key == KEY_BACKLIGHT_DOWN) {
        backlight_osd_step(key == KEY_BACKLIGHT_UP ? 1 : -1);
        return;
    }
    // 背光浮层是非模态的:任意键先把它收掉,再照常处理;Esc 只收浮层不往下走。
    if(g_bl_osd_until) {
        backlight_osd_hide();
        if(key == 27) return;
    }
    // Ctrl+Shift 轻点:只有 fcitx5 后端认(切 rime 方案)。
    if(key == KEY_IM_SWITCH) {
        if(g_linux_ime.switch_schema()) update_ime_bar();
        return;
    }
    if(g_linux_ime.active()) {
        if(key == KEY_FULLWIDTH_TOGGLE) {
            g_linux_ime.toggle_fullwidth();
            update_ime_bar();
            return;
        }
        if(key == KEY_TRAD_TOGGLE) {
            g_linux_ime.toggle_trad();
            update_ime_bar();
            return;
        }
        if(key == KEY_LSHIFT_TAP) {
            g_linux_ime.toggle_english();
            update_ime_bar();
            return;
        }
    }

    if(g_modalKind != 0) {
        if(g_modalKind == 1) {   // 删除确认
            if(key == '\n' || key == '\r' || key == 'y' || key == 'Y') delete_current();
            else if(key == 27 || key == 'n' || key == 'N') hide_modal();
            return;
        }
        // 编辑退出三选一
        if(key == 19 || key == '\n' || key == '\r') leave_editor(1);        // 保存
        else if(key == 'd' || key == 'D') leave_editor(2);                  // 放弃
        else if(key == 27) hide_modal();
        return;
    }

    handle_view_key(key);
}

void tick() {
    // 背光浮层停手一会儿就自己消失
    if(g_bl_osd_until && (int32_t)(lv_tick_get() - g_bl_osd_until) >= 0) backlight_osd_hide();

    // 编辑器里 textarea 的光标被叠加层盖住了,由叠加层自己画一根 2px 竖条 —— 让它闪。
    // 只闪「有位置」的光标:g_md_caret_x < 0 表示叠加层主动把它藏了,别再翻出来。
    if(g_md_caret && g_md_caret_x >= 0) {
        uint32_t now = lv_tick_get();
        if((int32_t)(now - g_md_blink_next) >= 0) {
            g_md_blink_next = now + 500;
            if(lv_obj_has_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN))
                lv_obj_remove_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 同步线程的结果:整份 Store 拿回来,落盘 + 重绘
    Store done;
    std::string status;
    if(sync_take(done, status)) {
        g_store = std::move(done);
        g_app.memos = g_store.memos;
        rebuild_filter(g_app);
        g_app.status = status;
        save_db();   // Ctrl+Q 直接退出时不会走 leave(),同步结果得马上落盘
        // 输入界面不能整片重绘 —— 那会把 textarea 重建一遍,正在打的字全没了。
        // 数据已经更新,等用户离开这个界面自然就看到了。
        if(g_ta) set_status_left(g_app.status);
        else render();
    }

    // 今日回顾线程的结果:合并进本地库(本地 dirty 优先),切到回顾列表。
    std::vector<Memo> review_memos;
    std::string review_status;
    if(review_take(review_memos, review_status)) {
        for(const Memo &m : review_memos) upsert_memo(g_app.memos, m);
        sort_memos(g_app.memos);
        rebuild_filter(g_app);
        g_reviewSlugs.clear();
        for(const Memo &m : review_memos) g_reviewSlugs.push_back(m.slug);
        g_reviewSel = 0;
        g_reviewTop = 0;
        g_reviewLoading = false;
        g_app.status = review_status;
        // 结果回来时用户可能已经走开(打字/设置/帮助),别把人从当前界面拽走。
        if(!g_ta && (g_app.view == View::List || g_app.view == View::Review)) {
            g_app.view = View::Review;
            render();
        }
    }

    // 状态栏右侧:时间 / WiFi / 电池
    status_right_refresh();
}

void insert_utf8(const std::string &text) {
    if(!g_ta || text.empty()) return;
    lv_textarea_add_text(g_ta, text.c_str());
    md_refresh();
    update_ime_bar();
}

void leave() {
    g_linux_ime.set_active(false);
    // 编辑器里还有内容的话先并回 store,别丢字。
    if(g_app.view == View::Edit) {
        const char *cur = g_ta ? lv_textarea_get_text(g_ta) : nullptr;
        if(cur) g_editText = cur;
        commit_editor();
        g_editSlug.clear();
        g_app.view = View::List;
    }
    save_db();
    g_root = nullptr;
    g_status = nullptr;
    g_status_right = nullptr;
    g_ta = nullptr;
    g_md_view = nullptr;
    g_md_caret = nullptr;
    if(lv_obj_t *scr = lv_screen_active()) lv_obj_clean(scr);
}

}  // namespace flomo
