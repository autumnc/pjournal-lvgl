#include "terminal_screen.h"

#include "app_ui.h"
#include "backlight.h"
#include "font_manager.h"
#include "key_codes.h"
#include "linux_ime.h"
#include "term_keys.h"
#include "term_pty.h"
#include "term_settings.h"
#include "term_settings_ui.h"
#include "term_theme.h"
#include "term_ui.h"
#include "term_vt.h"

#include <lvgl.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace terminal {

namespace {

term::Pty g_pty;
term::Vt *g_vt = nullptr;
term::TermUi g_ui;
term::SettingsUi g_settings_ui;
const lv_font_t *g_ui_font = nullptr;
bool g_created = false;

constexpr int kMenuCount = 4;
const char *const kMenuItems[kMenuCount] = {
    "返回主菜单(关闭会话)",
    "设置",
    "清屏",
    "快捷键说明",
};

lv_obj_t *g_menu = nullptr;
lv_obj_t *g_menu_rows[kMenuCount] = {};
int g_menu_sel = 0;
bool g_menu_visible = false;

lv_obj_t *g_help = nullptr;
bool g_help_visible = false;

std::string g_ime_sig;
std::string g_status_sig;

// wcwidth 看的是 LC_CTYPE:不设成 UTF-8 的话汉字按 1 格算,双宽字会串格。
// 只改 ctype,不动 LC_NUMERIC,JSON 那边的数字格式不受影响。
void ensure_locale() {
    static bool done = false;
    if(done) return;
    if(setlocale(LC_CTYPE, "C.utf8") || setlocale(LC_CTYPE, "en_US.utf8") ||
       setlocale(LC_CTYPE, "en_US.UTF-8"))
        done = true;
}

std::string home_dir() {
    if(const char *home = getenv("HOME"); home && *home) return home;
    if(struct passwd *pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

std::string resolve_font_path(const std::string &p) {
    return p.empty() ? home_dir() + "/.fonts/Go-LavaPropo-NF-R.ttf" : p;
}

int ui_size_for(const term::TermSettings &st) {
    int n = st.ui_font_size ? st.ui_font_size : st.font_size * 3 / 5;
    return n < 8 ? 8 : n;
}

// 粗体/斜体:配置里给了字面就用那个文件(它本身就是设计成粗/斜的,按 Normal 载);
// 没给就拿正文那份让 FreeType 合成。
lv_font_t *load_styled_font(FontStyle style, int size) {
    const term::TermSettings &st = term::term_settings();
    const std::string &p = style == FontStyle::Bold ? st.font_bold_path : st.font_italic_path;
    if(p.empty()) return g_fonts.load_styled(g_fonts.current_path(), size, style);
    return g_fonts.load_styled(p, size, FontStyle::Normal);
}

std::string first_utf8_char(const std::string &s) {
    if(s.empty()) return std::string();
    const unsigned char c = (unsigned char)s[0];
    size_t n = 1;
    if(c >= 0xf0) n = 4;
    else if(c >= 0xe0) n = 3;
    else if(c >= 0xc0) n = 2;
    if(n > s.size()) n = s.size();
    return s.substr(0, n);
}

// ---------------------------------------------------------------- 浮层

void refresh_menu_selection() {
    const term::TermTheme &th = term::theme_current();
    for(int i = 0; i < kMenuCount; ++i) {
        const bool sel = i == g_menu_sel;
        const std::string text = std::string(sel ? "▶ " : "  ") + kMenuItems[i];
        lv_label_set_text(g_menu_rows[i], text.c_str());
        lv_obj_set_style_text_color(g_menu_rows[i],
                                    lv_color_hex(sel ? th.cursor : th.panel_fg), 0);
    }
}

void build_menu() {
    if(!g_ui_font) return;
    const term::TermTheme &th = term::theme_current();
    const int line_h = (int)lv_font_get_line_height(g_ui_font);
    const int pad = 20;
    const int w = 560;
    const int title_h = line_h + 16;
    const int row_h = line_h + 12;
    const int h = 10 + title_h + kMenuCount * row_h + 16;

    g_menu = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_menu, w, h);
    lv_obj_align(g_menu, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(g_menu, 8, 0);
    lv_obj_set_style_border_width(g_menu, 2, 0);
    lv_obj_set_style_border_color(g_menu, lv_color_hex(th.panel_fg), 0);
    lv_obj_set_style_bg_color(g_menu, lv_color_hex(th.panel_bg), 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_menu, 0, 0);
    lv_obj_set_style_shadow_width(g_menu, 0, 0);
    lv_obj_remove_flag(g_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(g_menu);
    lv_label_set_text(title, "终端菜单");
    lv_obj_set_style_text_font(title, g_ui_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(th.status), 0);
    lv_obj_set_pos(title, pad, 10);

    for(int i = 0; i < kMenuCount; ++i) {
        g_menu_rows[i] = lv_label_create(g_menu);
        lv_obj_set_style_text_font(g_menu_rows[i], g_ui_font, 0);
        lv_obj_set_pos(g_menu_rows[i], pad, 10 + title_h + i * row_h);
    }
    g_menu_sel = 0;
    refresh_menu_selection();
}

void build_help() {
    if(!g_ui_font) return;
    const term::TermTheme &th = term::theme_current();
    static const char *const lines[] = {
        "F10        终端菜单",
        "Ctrl+Q     退出程序(有确认)",
        "Ctrl+空格  输入法开关",
        "Shift+↑↓   回看",
        "PgUp/PgDn  发给 shell",
        "Esc        发给 shell",
        "",
        "任意键关闭",
    };
    const int n = (int)(sizeof(lines) / sizeof(lines[0]));
    const int line_h = (int)lv_font_get_line_height(g_ui_font);
    const int pad = 20;
    const int w = 520;
    const int title_h = line_h + 16;
    const int h = 10 + title_h + n * line_h + 16;

    g_help = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_help, w, h);
    lv_obj_align(g_help, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(g_help, 8, 0);
    lv_obj_set_style_border_width(g_help, 2, 0);
    lv_obj_set_style_border_color(g_help, lv_color_hex(th.panel_fg), 0);
    lv_obj_set_style_bg_color(g_help, lv_color_hex(th.panel_bg), 0);
    lv_obj_set_style_bg_opa(g_help, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_help, 0, 0);
    lv_obj_set_style_shadow_width(g_help, 0, 0);
    lv_obj_remove_flag(g_help, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_help, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(g_help);
    lv_label_set_text(title, "快捷键说明");
    lv_obj_set_style_text_font(title, g_ui_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(th.status), 0);
    lv_obj_set_pos(title, pad, 10);

    for(int i = 0; i < n; ++i) {
        lv_obj_t *l = lv_label_create(g_help);
        lv_label_set_text(l, lines[i]);
        lv_obj_set_style_text_font(l, g_ui_font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(th.panel_fg), 0);
        lv_obj_set_pos(l, pad, 10 + title_h + i * line_h);
    }
}

// 字体/主题变了就整块重建:浮层的字号和配色都跟着设置走。
void build_overlays() {
    g_menu_visible = false;
    g_help_visible = false;
    if(g_menu) {
        lv_obj_delete(g_menu);
        g_menu = nullptr;
    }
    if(g_help) {
        lv_obj_delete(g_help);
        g_help = nullptr;
    }
    build_menu();
    build_help();
}

void show_menu() {
    if(!g_menu) return;
    g_menu_visible = true;
    lv_obj_remove_flag(g_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_menu);
}

void hide_menu() {
    if(!g_menu) return;
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);
    g_menu_visible = false;
}

void show_help() {
    if(!g_help) return;
    g_help_visible = true;
    lv_obj_remove_flag(g_help, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_help);
}

void hide_help() {
    if(!g_help) return;
    lv_obj_add_flag(g_help, LV_OBJ_FLAG_HIDDEN);
    g_help_visible = false;
}

void clear_screen() {
    if(!g_vt) return;
    g_vt->feed("\033[2J\033[H", 7);
    g_vt->scroll_to_bottom();
    g_ui.invalidate_all();
}

void activate_menu_item() {
    switch(g_menu_sel) {
    case 0:
        hide_menu();
        app_ui_return_to_main();  // goto_screen 会调 leave(),会话随之关闭
        return;
    case 1:
        hide_menu();
        g_settings_ui.show();
        return;
    case 2:
        hide_menu();
        clear_screen();
        return;
    case 3:
        hide_menu();
        show_help();
        return;
    default: return;
    }
}

void handle_menu_key(int key) {
    switch(key) {
    case KEY_UP:
        g_menu_sel = (g_menu_sel + kMenuCount - 1) % kMenuCount;
        refresh_menu_selection();
        break;
    case KEY_DOWN:
        g_menu_sel = (g_menu_sel + 1) % kMenuCount;
        refresh_menu_selection();
        break;
    case '\n':
    case '\r':
    case KEY_CTRL_ENTER: activate_menu_item(); break;
    case 27:
    case term::kKeyMenu: hide_menu(); break;
    default: break;  // 菜单开着时其它键一律吞掉
    }
}

// ---------------------------------------------------------------- 输入法浮层

void update_ime_bar() {
    LinuxIme &ime = g_linux_ime;
    std::string preedit;
    std::vector<std::string> cands;
    int hi = -1;
    if(ime.active() && ime.composing()) {
        preedit = ime.composition();
        cands = ime.candidates();
        hi = ime.highlight_index();
    }
    // 每圈都 set_text 会白白标脏、每帧重排候选条,先比一遍内容。
    std::string sig = preedit;
    sig += '\x1f';
    for(const auto &c : cands) {
        sig += c;
        sig += '\x1e';
    }
    sig += std::to_string(hi);
    if(sig == g_ime_sig) return;
    g_ime_sig = sig;
    g_ui.set_ime(preedit, cands, hi);
}

void update_status() {
    LinuxIme &ime = g_linux_ime;
    std::string text;
    if(ime.active()) {
        if(ime.english()) {
            text = "[英]";
        } else {
            std::string name = first_utf8_char(ime.schema_name());
            if(name.empty()) name = "中";
            text = "[" + name + "]" + (ime.fullwidth() ? "全" : "半") +
                   (ime.trad() ? "繁" : "简");
        }
    }
    if(text == g_status_sig) return;
    g_status_sig = text;
    g_ui.set_status(text, !text.empty());
}

// ---------------------------------------------------------------- 字体 / 设置

void reload_fonts() {
    term::TermSettings &st = term::term_settings();
    const int size = st.font_size;
    const int ui_size = ui_size_for(st);

    const std::string primary = resolve_font_path(st.font_path);
    const lv_font_t *font = g_fonts.load(primary, size);
    if(!font) {
        const std::string fb = g_fonts.default_font_path();
        if(!fb.empty() && fb != primary) font = g_fonts.load(fb, size);
    }
    if(!font) return;  // 一个都载不动就保持现状,别把屏搞空

    const lv_font_t *ui_font = g_fonts.load(g_fonts.current_path(), ui_size);
    if(!ui_font) ui_font = font;
    lv_font_t *bold = load_styled_font(FontStyle::Bold, size);
    lv_font_t *italic = load_styled_font(FontStyle::Italic, size);

    // 新字号放不下一格时 set_fonts 返回 false,行列数保持原样。
    if(g_ui.set_fonts(font, ui_font, bold, italic)) {
        g_ui_font = ui_font;
        if(g_vt) {
            g_vt->resize(g_ui.cols(), g_ui.rows());
            g_pty.set_winsize(g_ui.cols(), g_ui.rows());
        }
    }
    g_settings_ui.set_font(ui_font);
    g_ui.invalidate_all();
    build_overlays();
    g_ime_sig.clear();
    g_status_sig.clear();
    update_ime_bar();
    update_status();
}

void apply_setting_change(term::SettingChange ch) {
    const term::TermSettings &st = term::term_settings();
    switch(ch) {
    case term::SettingChange::FontFamily:
    case term::SettingChange::FontStyle:
    case term::SettingChange::FontSize: reload_fonts(); break;
    case term::SettingChange::Theme:
        // 格子里存的是调色板索引,换张表屏上已有的内容就跟着变色了。
        g_ui.refresh_theme();
        g_settings_ui.refresh_theme();
        build_overlays();
        break;
    case term::SettingChange::Cursor:
        g_ui.set_cursor_style(st.cursor_style, st.cursor_blink);
        break;
    case term::SettingChange::Scrollback:
        if(g_vt) g_vt->set_scrollback_limit((size_t)st.scrollback);
        break;
    case term::SettingChange::None: break;
    }
}

// ---------------------------------------------------------------- shell

std::string shell_path() {
    if(const char *s = getenv("PJOURNAL_TERMINAL_SHELL"); s && *s) return s;
    if(struct passwd *pw = getpwuid(getuid())) {
        if(pw->pw_shell && *pw->pw_shell) return pw->pw_shell;
    }
    return std::string();
}

void backlight_step(int dir) {
    if(!backlight_available()) return;
    int pct = backlight_current_percent();
    pct += dir > 0 ? 10 : -10;
    if(pct < 5) pct = 5;  // 0 就彻底黑了,这台机器没有亮度键能摸回来
    if(pct > 100) pct = 100;
    backlight_set_percent(pct);
    g_ui.show_osd("亮度 " + std::to_string(pct) + "%", lv_tick_get());
}

}  // namespace

// ------------------------------------------------------------------ 生命周期

void create() {
    if(g_created) leave();
    ensure_locale();

    term::settings_load(term::settings_path());
    term::TermSettings &st = term::term_settings();
    term::theme_set_by_id(st.theme_id);

    lv_obj_t *scr = lv_screen_active();
    // 别 remove_style_all:root 上挂着 app 的主题样式,其中 text_font 是主菜单那些
    // 没显式设字体的标签继承来的;剥掉之后回主菜单,中文和图标全变豆腐块。
    // 终端自己的背景/内边距都用本地样式覆盖,不需要动样式表。
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    g_created = true;  // root 从这一刻归终端管,后面失败也能被 leave() 收干净

    const int w = (int)lv_display_get_horizontal_resolution(nullptr);
    const int h = (int)lv_display_get_vertical_resolution(nullptr);

    const int size = st.font_size;
    const int ui_size = ui_size_for(st);
    const std::string primary = resolve_font_path(st.font_path);
    const lv_font_t *font = g_fonts.load(primary, size);
    if(!font) {
        const std::string fb = g_fonts.default_font_path();
        if(!fb.empty() && fb != primary) font = g_fonts.load(fb, size);
    }
    if(!font) {
        // 一个字体都载不动也得把界面留下来,否则就是一块黑屏、连返回都没法按。
        fprintf(stderr, "terminal: 字体加载失败,退回内置字体\n");
        font = LV_FONT_DEFAULT;
        g_ui_font = font;
    } else {
        g_ui_font = g_fonts.load(g_fonts.current_path(), ui_size);
        if(!g_ui_font) g_ui_font = font;
    }
    lv_font_t *bold = font == LV_FONT_DEFAULT ? nullptr : load_styled_font(FontStyle::Bold, size);
    lv_font_t *italic =
        font == LV_FONT_DEFAULT ? nullptr : load_styled_font(FontStyle::Italic, size);

    if(!g_ui.create(scr, font, g_ui_font, 0, 0, w, h)) {
        fprintf(stderr, "terminal: %d 号字在 %dx%d 上连一格都放不下\n", size, w, h);
        leave();
        return;
    }
    g_ui.set_fonts(font, g_ui_font, bold, italic);
    g_ui.set_cursor_style(st.cursor_style, st.cursor_blink);

    // Vt 的尺寸由格子数决定,只能等画布建完再建。
    g_vt = new term::Vt(g_ui.cols(), g_ui.rows());
    g_ui.attach(g_vt);
    g_vt->set_scrollback_limit((size_t)st.scrollback);
    g_ui.invalidate_all();

    if(!g_settings_ui.create(scr, g_ui_font, w, h))
        fprintf(stderr, "terminal: 设置界面建不起来\n");
    build_overlays();

    // 终端默认英文直通;从别的屏带着中文状态进来时先关掉。
    g_linux_ime.set_active(false);
    g_linux_ime.set_display_width(0);
    g_ime_sig.clear();
    g_status_sig.clear();

    std::string shell = shell_path();
    if(shell.empty() || access(shell.c_str(), X_OK) != 0) shell = "/bin/bash";
    if(access(shell.c_str(), X_OK) != 0) shell = "/bin/sh";

    const bool spawned = g_pty.spawn(shell, g_ui.cols(), g_ui.rows(), home_dir(),
                                     "xterm-256color", "C.utf8");
    if(!spawned) {
        fprintf(stderr, "terminal: 无法启动 %s\n", shell.c_str());
        g_ui.show_osd("无法启动 " + shell + " · F10 返回主菜单", lv_tick_get(), 60000);
    } else {
        g_ui.show_osd("F10 菜单", lv_tick_get(), 2000);
    }
    update_ime_bar();
    update_status();
}

void render() {}

void tick() {
    if(!g_created) return;

    if(g_pty.valid()) {
        std::string in;
        if(!g_pty.read(in)) {
            // 对端关了 = shell 退出了。最后的输出留给用户看,不自动踢回主菜单。
            if(!in.empty() && g_vt) g_vt->feed(in.data(), in.size());
            g_pty.close();
            g_ui.show_osd("shell 已退出 · F10 返回主菜单", lv_tick_get(), 60000);
        } else if(!in.empty() && g_vt) {
            g_vt->feed(in.data(), in.size());
        }
    }

    if(g_vt) {
        std::string reply;
        if(g_vt->take_reply(reply)) g_pty.write(reply);
        g_pty.flush();
        g_ui.invalidate_dirty();
        g_vt->clear_dirty();
    }

    if(!g_settings_ui.visible() && !g_menu_visible && !g_help_visible) {
        update_ime_bar();
        update_status();
    }
    g_ui.tick(lv_tick_get());
}

void handle_key(int key) {
    if(!g_created || key <= 0) return;
    LinuxIme &ime = g_linux_ime;

    // ---- 设置界面开着:键全归它 ----
    if(g_settings_ui.visible()) {
        term::SettingChange ch = term::SettingChange::None;
        if(g_settings_ui.handle_key(key, ch)) {
            apply_setting_change(ch);
            return;
        }
        if(g_settings_ui.dirty()) {
            if(!term::settings_save(term::settings_path()))
                fprintf(stderr, "terminal: 设置写不进 %s\n", term::settings_path().c_str());
            g_settings_ui.clear_dirty();
        }
        g_settings_ui.hide();
        g_ui.invalidate_all();
        update_ime_bar();
        update_status();
        return;
    }

    if(g_help_visible) {
        hide_help();  // 浮层上写着「任意键关闭」,就真的任意键都关
        return;
    }

    if(g_menu_visible) {
        handle_menu_key(key);
        return;
    }

    // F10 打开菜单,不进 shell。
    if(key == term::kKeyMenu) {
        show_menu();
        return;
    }

    if(key == KEY_BACKLIGHT_UP || key == KEY_BACKLIGHT_DOWN) {
        backlight_step(key == KEY_BACKLIGHT_UP ? 1 : -1);
        return;
    }

    // Shift+↑↓ 走回看。
    if(const int lines = term::scroll_lines(key, 0)) {
        if(g_vt) g_vt->scroll_view(lines);
        return;
    }

    if(key == KEY_HELP) {
        show_help();
        return;
    }

    // ---- 输入法自己的开关 ----
    if(key == KEY_IME_TOGGLE) {
        ime.toggle();
        update_status();
        return;
    }
    if(key == KEY_IM_SWITCH) {
        ime.switch_schema();
        update_status();
        return;
    }
    if(ime.active()) {
        if(key == KEY_FULLWIDTH_TOGGLE) {
            ime.toggle_fullwidth();
            update_status();
            return;
        }
        if(key == KEY_TRAD_TOGGLE) {
            ime.toggle_trad();
            update_status();
            return;
        }
        if(key == KEY_LSHIFT_TAP) {
            ime.toggle_english();
            update_status();
            return;
        }
    }

    // ---- 输入法先挑,它不要的才翻译成 VT 序列 ----
    std::string out;
    if(ime.handle_key(key, out)) {
        if(!out.empty()) {
            if(g_vt) g_vt->scroll_to_bottom();
            g_pty.write(out);
        }
        return;
    }

    std::string bytes = term::bytes_for(key, g_vt && g_vt->app_cursor_keys());
    if(bytes.empty()) return;
    if(g_vt) g_vt->scroll_to_bottom();
    g_pty.write(bytes);
}

void insert_utf8(const std::string &text) {
    if(!g_created || text.empty()) return;
    if(g_settings_ui.visible() || g_menu_visible || g_help_visible) return;
    if(g_vt) g_vt->scroll_to_bottom();
    g_pty.write(text);
}

void leave() {
    g_linux_ime.set_active(false);
    g_pty.close();

    // 先扔 UI 的成员(里面挂着 lv_image_dsc_t 和画布指针),再让 LVGL 删对象,
    // 这样不会有谁在对象没了之后还去引用它们。
    g_ui = term::TermUi{};
    g_settings_ui = term::SettingsUi{};
    delete g_vt;
    g_vt = nullptr;

    g_menu = nullptr;
    for(auto &row : g_menu_rows) row = nullptr;
    g_help = nullptr;
    g_menu_visible = false;
    g_help_visible = false;
    g_menu_sel = 0;

    g_ui_font = nullptr;
    g_ime_sig.clear();
    g_status_sig.clear();

    if(g_created) lv_obj_clean(lv_screen_active());
    g_created = false;
}

}  // namespace terminal
