#include "term_settings_ui.h"

#include "font_manager.h"
#include "term_keys.h"
#include "term_settings.h"
#include "term_theme.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace term {

namespace {

const char *const kRowNames[] = {
    "字体", "字号", "粗体", "斜体", "主题", "光标", "光标闪烁", "回看行数", "界面字号",
};

const char *const kCursorNames[] = {"块", "下划线", "竖线"};

// 回看行数只给这几档。手填个 347 没有意义,而档位少按几下就到头了。
const int kScrollback[] = {200, 500, 1000, 2000, 5000};

const char *bool_name(bool v) { return v ? "开" : "关"; }

std::string basename_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 只列含中文的字体:终端里的输出大半是中文,NF-Mono 这类纯拉丁字体会让 BMP 汉字
// 全成豆腐(FontManager 的回退链只补生僻平面和符号,不含 BMP 中文)。
std::vector<const FontInfo *> visible_fonts() {
    std::vector<const FontInfo *> out;
    for(const auto &f : g_fonts.fonts())
        if(g_fonts.supports_cjk(f.path)) out.push_back(&f);
    return out;
}

}  // namespace

// ------------------------------------------------------------------ 取值

int SettingsUi::font_choice_count() const { return 1 + (int)visible_fonts().size(); }

int SettingsUi::font_choice_index(int row) const {
    const TermSettings &s = term_settings();
    const std::string &p =
        row == RowFont ? s.font_path : row == RowBold ? s.font_bold_path : s.font_italic_path;
    if(p.empty()) return 0;
    const std::vector<const FontInfo *> fonts = visible_fonts();
    for(size_t i = 0; i < fonts.size(); ++i)
        if(fonts[i]->path == p) return (int)i + 1;
    return 0;  // 设了个不在字体表里的路径(手改的配置):照 0 处理,但标签照实显示
}

std::string SettingsUi::font_label(int row) const {
    const int idx = font_choice_index(row);
    if(idx > 0) return visible_fonts()[idx - 1]->name;

    // 显示「默认/合成」之前先看一眼是不是真没设 —— 配置里写了个扫描目录之外的路径时
    // 界面说「默认」而实际用的不是,那对不上。
    const TermSettings &s = term_settings();
    const std::string &p =
        row == RowFont ? s.font_path : row == RowBold ? s.font_bold_path : s.font_italic_path;
    if(!p.empty()) return basename_of(p) + "(列表外)";
    return row == RowFont ? "默认" : "合成";
}

void SettingsUi::set_font_choice(int row, int idx, SettingChange &changed) {
    const int n = font_choice_count();
    if(n <= 0) return;
    idx %= n;
    if(idx < 0) idx += n;

    TermSettings &s = term_settings();
    std::string *slot = row == RowFont     ? &s.font_path
                        : row == RowBold   ? &s.font_bold_path
                                           : &s.font_italic_path;
    const std::string want = idx == 0 ? std::string() : visible_fonts()[idx - 1]->path;
    if(want == *slot) return;
    *slot = want;
    _dirty = true;
    changed = row == RowFont ? SettingChange::FontFamily : SettingChange::FontStyle;
    refresh_row(row);
}

int SettingsUi::scrollback_index() const {
    const int n = (int)(sizeof kScrollback / sizeof *kScrollback);
    const int cur = term_settings().scrollback;
    int best = 0, best_d = -1;
    for(int i = 0; i < n; ++i) {
        const int d = kScrollback[i] > cur ? kScrollback[i] - cur : cur - kScrollback[i];
        if(best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// 「自动」那档对应的字号:和 main.cpp 启动时算的一样(正文的 3/5),夹到档位范围内
// 并对齐到步进 2,这样从「自动」往下按一下不会跳到一个奇怪的数。
int SettingsUi::auto_ui_size() const {
    int s = term_settings().font_size * 3 / 5;
    if(s < 12) s = 12;
    if(s > 60) s = 60;
    return s & ~1;
}

// ------------------------------------------------------------------ 建与排

bool SettingsUi::create(lv_obj_t *parent, const lv_font_t *ui_font, int w, int h) {
    if(!parent || !ui_font || w <= 0 || h <= 0) return false;
    _ui_font = ui_font;
    _w = w;
    _h = h;

    _panel = lv_obj_create(parent);
    // lv_obj 不给尺寸就是「包住内容」,而里面每一行都是按绝对坐标摆的 —— 面板会缩成
    // 一小块,把下面的行全裁掉。满屏浮层必须显式给坐标和尺寸。
    lv_obj_set_pos(_panel, 0, 0);
    lv_obj_set_size(_panel, w, h);
    lv_obj_set_style_radius(_panel, 0, 0);
    lv_obj_set_style_border_width(_panel, 0, 0);
    lv_obj_set_style_pad_all(_panel, 0, 0);
    lv_obj_set_style_shadow_width(_panel, 0, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_panel, LV_OBJ_FLAG_HIDDEN);

    _title = lv_label_create(_panel);
    lv_label_set_text(_title, "设置");

    for(int i = 0; i < RowCount; ++i) {
        // 一行一个容器:高亮条是它的第一个孩子,两个标签在后 —— 绘制顺序就是
        // 条在下、字在上,而收掉条不会连字一起收掉。
        _rows[i] = lv_obj_create(_panel);
        lv_obj_set_style_radius(_rows[i], 0, 0);
        lv_obj_set_style_border_width(_rows[i], 0, 0);
        lv_obj_set_style_pad_all(_rows[i], 0, 0);
        lv_obj_set_style_shadow_width(_rows[i], 0, 0);
        lv_obj_set_style_bg_opa(_rows[i], LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(_rows[i], LV_OBJ_FLAG_SCROLLABLE);

        _bars[i] = lv_obj_create(_rows[i]);
        lv_obj_set_style_radius(_bars[i], 0, 0);
        lv_obj_set_style_border_width(_bars[i], 0, 0);
        lv_obj_set_style_pad_all(_bars[i], 0, 0);
        lv_obj_set_style_shadow_width(_bars[i], 0, 0);
        lv_obj_set_style_bg_opa(_bars[i], LV_OPA_30, 0);
        lv_obj_remove_flag(_bars[i], LV_OBJ_FLAG_SCROLLABLE);

        _names[i] = lv_label_create(_rows[i]);
        lv_label_set_text(_names[i], kRowNames[i]);
        _values[i] = lv_label_create(_rows[i]);
    }

    _hint = lv_label_create(_panel);
    lv_label_set_text(_hint, "▲▼ 选择   ◀▶ 修改   Esc/F10 返回");

    layout();
    refresh_theme();
    refresh_all();
    select(0);
    return true;
}

void SettingsUi::layout() {
    if(!_panel || !_ui_font) return;
    const int line_h = (int)lv_font_get_line_height(_ui_font);
    _top = line_h + 28;
    const int pad = 32;

    // 九行加起来的行高必须落在标题和底部提示之间。界面字号调到 50 往上时,按「行高+12」
    // 会溢出屏底、把最后几行裁掉 —— 那就把行距压到刚好放得下,挤一点也比看不见强。
    const int room = _h - _top - (line_h + 20);
    int row_h = line_h + 12;
    const int max_row_h = room / RowCount;
    if(row_h > max_row_h) row_h = max_row_h;
    _row_h = row_h > 0 ? row_h : 1;

    lv_obj_set_style_text_font(_title, _ui_font, 0);
    lv_obj_align(_title, LV_ALIGN_TOP_LEFT, pad, 10);

    for(int i = 0; i < RowCount; ++i) {
        lv_obj_set_pos(_rows[i], 0, _top + i * _row_h);
        lv_obj_set_size(_rows[i], _w, _row_h);
        lv_obj_set_pos(_bars[i], 0, 0);
        lv_obj_set_size(_bars[i], _w, _row_h);
        lv_obj_set_style_text_font(_names[i], _ui_font, 0);
        lv_obj_set_style_text_font(_values[i], _ui_font, 0);
        lv_obj_align(_names[i], LV_ALIGN_LEFT_MID, pad, 0);
        lv_obj_align(_values[i], LV_ALIGN_RIGHT_MID, -pad, 0);
    }

    lv_obj_set_style_text_font(_hint, _ui_font, 0);
    lv_obj_align(_hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void SettingsUi::set_font(const lv_font_t *ui_font) {
    if(!ui_font) return;
    _ui_font = ui_font;
    layout();
}

void SettingsUi::refresh_theme() {
    if(!_panel) return;
    const TermTheme &th = theme_current();
    lv_obj_set_style_bg_color(_panel, lv_color_hex(th.palette[0]), 0);
    lv_obj_set_style_text_color(_title, lv_color_hex(th.status), 0);
    lv_obj_set_style_text_color(_hint, lv_color_hex(th.status), 0);
    for(int i = 0; i < RowCount; ++i) {
        lv_obj_set_style_bg_color(_bars[i], lv_color_hex(th.cursor), 0);
        lv_obj_set_style_text_color(_names[i], lv_color_hex(th.palette[7]), 0);
        lv_obj_set_style_text_color(_values[i], lv_color_hex(th.palette[7]), 0);
    }
}

// ------------------------------------------------------------------ 显示

void SettingsUi::show() {
    if(!_panel) return;
    refresh_all();  // 进来看到的得是当前值(可能被配置文件或环境变量改过)
    lv_obj_remove_flag(_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_panel);
    _visible = true;
    select(_sel);
}

void SettingsUi::hide() {
    if(!_panel) return;
    lv_obj_add_flag(_panel, LV_OBJ_FLAG_HIDDEN);
    _visible = false;
}

void SettingsUi::refresh_row(int row) {
    if(row < 0 || row >= RowCount) return;
    const TermSettings &s = term_settings();
    std::string v;
    switch(row) {
    case RowFont: v = font_label(RowFont); break;
    case RowSize: v = std::to_string(s.font_size); break;
    case RowBold: v = font_label(RowBold); break;
    case RowItalic: v = font_label(RowItalic); break;
    case RowTheme: v = theme_current().label; break;
    case RowCursor: v = kCursorNames[s.cursor_style < 0   ? 0
                                     : s.cursor_style > 2 ? 2
                                                          : s.cursor_style];
        break;
    case RowBlink: v = bool_name(s.cursor_blink); break;
    case RowScrollback: v = std::to_string(s.scrollback); break;
    case RowUiSize: v = s.ui_font_size == 0 ? "自动" : std::to_string(s.ui_font_size); break;
    default: return;
    }
    lv_label_set_text(_values[row], v.c_str());
}

void SettingsUi::refresh_all() {
    for(int i = 0; i < RowCount; ++i) refresh_row(i);
}

void SettingsUi::select(int row) {
    if(row < 0 || row >= RowCount) return;
    _sel = row;
    for(int i = 0; i < RowCount; ++i) {
        if(i == row) lv_obj_remove_flag(_bars[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(_bars[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void SettingsUi::move(int dir) {
    if(RowCount == 0) return;
    int r = _sel + dir;
    if(r < 0) r = RowCount - 1;
    if(r >= RowCount) r = 0;
    select(r);
}

// ------------------------------------------------------------------ 改值

void SettingsUi::adjust(int dir, SettingChange &changed) {
    TermSettings &s = term_settings();
    switch(_sel) {
    case RowFont:
    case RowBold:
    case RowItalic:
        set_font_choice(_sel, font_choice_index(_sel) + dir, changed);
        return;

    case RowSize: {
        int v = s.font_size + dir * 2;
        if(v < 12) v = 12;
        if(v > 80) v = 80;
        if(v != s.font_size) {
            s.font_size = v;
            _dirty = true;
            changed = SettingChange::FontSize;
        }
        break;
    }

    case RowUiSize: {
        int v = s.ui_font_size;
        if(v == 0) {
            if(dir < 0) break;  // 「自动」已经是最小的一档
            v = auto_ui_size();
        } else {
            v += dir * 2;
            if(v < 12) v = dir < 0 ? 0 : 12;  // 再往下就回到「自动」
            if(v > 60) v = 60;
        }
        if(v != s.ui_font_size) {
            s.ui_font_size = v;
            _dirty = true;
            changed = SettingChange::FontSize;
        }
        break;
    }

    case RowTheme: {
        size_t n = 0;
        const TermTheme *table = theme_table(n);
        if(n == 0) break;
        const size_t i = (theme_index() + n + (dir > 0 ? 1 : n - 1)) % n;
        if(i != theme_index()) {
            theme_set_index(i);
            s.theme_id = table[i].id;  // 存的时候按 id 走,见 term_settings.cpp
            _dirty = true;
            changed = SettingChange::Theme;
        }
        break;
    }

    case RowCursor: {
        int v = s.cursor_style + dir;
        if(v < 0) v = 2;
        if(v > 2) v = 0;
        if(v != s.cursor_style) {
            s.cursor_style = v;
            _dirty = true;
            changed = SettingChange::Cursor;
        }
        break;
    }

    case RowBlink:
        // 只有两个值,左按右按都一样是翻转。
        s.cursor_blink = !s.cursor_blink;
        _dirty = true;
        changed = SettingChange::Cursor;
        break;

    case RowScrollback: {
        const int n = (int)(sizeof kScrollback / sizeof *kScrollback);
        int i = scrollback_index() + dir;
        if(i < 0) i = n - 1;
        if(i >= n) i = 0;
        if(kScrollback[i] != s.scrollback) {
            s.scrollback = kScrollback[i];
            _dirty = true;
            changed = SettingChange::Scrollback;
        }
        break;
    }

    default: return;
    }
    refresh_row(_sel);
}

bool SettingsUi::handle_key(int key, SettingChange &changed) {
    changed = SettingChange::None;
    if(!_visible) return true;
    if(key <= 0) return true;

    // 关闭。Esc 或 F10。设置开着的时候别的键一律吞掉,漏进 shell 会出事。
    if(key == 27 || key == kKeyMenu) return false;

    switch(key) {
    case KEY_UP: move(-1); break;
    case KEY_DOWN: move(+1); break;
    case KEY_LEFT: adjust(-1, changed); break;
    case KEY_RIGHT: adjust(+1, changed); break;
    default: break;  // 别的键吞掉,别漏进 shell
    }
    return true;
}

}  // namespace term
