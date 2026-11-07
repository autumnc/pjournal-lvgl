#pragma once

// 屏内设置界面(长按 △ 进,见 input.cpp)。
//
// 一块满屏浮层,盖在终端画布和输入法浮层上。**只改 term_settings()** —— 它不认识
// Vt 和 pty,改完一项由主程序决定怎么落地(重载字体 / 换主题 / …)。
//
// 导航走 input.cpp 那套自定义键码,不注册 LVGL indev(见 keymap.h):
//   ▲▼ 选行,◀▶ 改值,Esc / 再长按一次 △ / ✕ 返回。

#include <lvgl.h>

#include <string>

namespace term {

// 这次改的是哪一类。主程序按它决定重载什么 —— 拆这么细是因为「换主字体」和「换字号」
// 的代价不一样:前者要重新挑一遍回退链,后者不用(见 font_manager.h 的 reset_fallbacks)。
enum class SettingChange {
    None,
    FontFamily,  // 正文路径:要重挑回退链
    FontStyle,   // 粗体/斜体路径:只换那两份
    FontSize,    // 字号 / 界面字号
    Theme,
    Cursor,      // 样式或闪烁
    Scrollback,
};

class SettingsUi {
public:
    // 满屏浮层,坐标 (0,0,w,h)。建完是**收着**的,要 show()。
    bool create(lv_obj_t *parent, const lv_font_t *ui_font, int w, int h);

    // 界面字号换了:重排每一行、重设所有文字的字体。
    void set_font(const lv_font_t *ui_font);
    // 主题换了:重新上色。
    void refresh_theme();

    void show();
    void hide();
    bool visible() const { return _visible; }

    // 处理一键。**返回 false = 请求关闭**(Esc / △ / ✕)。其余键一律吞下 —— 设置开着
    // 的时候不该有键漏进 shell。changed 是这次改动的类别,没改就是 None。
    bool handle_key(int key, SettingChange &changed);

    bool dirty() const { return _dirty; }
    void clear_dirty() { _dirty = false; }

private:
    enum Row {
        RowFont = 0,
        RowSize,
        RowBold,
        RowItalic,
        RowTheme,
        RowCursor,
        RowBlink,
        RowScrollback,
        RowUiSize,
        RowCount,
    };

    void layout();
    void select(int row);
    void move(int dir);
    void adjust(int dir, SettingChange &changed);
    void refresh_row(int row);
    void refresh_all();

    // 字体项的下拉:第 0 项是「默认」/「合成」,往后是字体表。返回下标用的都是
    // 这套编号。
    int font_choice_count() const;
    int font_choice_index(int row) const;
    std::string font_label(int row) const;
    void set_font_choice(int row, int idx, SettingChange &changed);
    int scrollback_index() const;
    int auto_ui_size() const;

    const lv_font_t *_ui_font = nullptr;
    lv_obj_t *_panel = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_hint = nullptr;
    lv_obj_t *_rows[RowCount] = {};
    lv_obj_t *_bars[RowCount] = {};    // 选中行的高亮条(在文字下面)
    lv_obj_t *_names[RowCount] = {};
    lv_obj_t *_values[RowCount] = {};

    int _w = 0, _h = 0;
    int _row_h = 1, _top = 0;
    int _sel = 0;
    bool _visible = false;
    bool _dirty = false;
};

}  // namespace term
