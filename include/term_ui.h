#pragma once

// 终端画布 + 三个浮层(输入法候选条、状态角标、背光 OSD)。
//
// 终端内容不是 LVGL 摆得出来的东西(它没有网格),所以整块屏是一个自绘对象:
// LV_EVENT_DRAW_MAIN 里按行铺底色块、按格铺字符。
//
// 字体是比例字体(Go-LavaPropo),格宽按「ASCII 可打印字符里最宽的那个推进宽度」
// 定,每个字形在格子里居中画 —— 这样比例字体也能严格对齐。代价是窄字母(ilj)
// 周围留白多,想紧凑就换 NF-Mono.ttf(见 README)。
//
// 配色不写死在这里:一律走 theme_current()(term_theme.h)。

#include <lvgl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace term {

class Vt;

class TermUi {
public:
    // canvas 占 parent 上的 (x, y, w, h);字号量不出格子时返回 false。
    // font 是终端正文字体,ui_font 给浮层用(通常小一号)。
    // bold/italic 可以为空(那就跟正文一样),见 set_fonts()。
    bool create(lv_obj_t *parent, const lv_font_t *font, const lv_font_t *ui_font, int x, int y,
                int w, int h);
    // VT 必须在 create() 之后建(Vt 的尺寸由格子数决定),建好再挂过来。
    void attach(Vt *vt);

    // 换字体。重算格宽/行高/行列数,跟着调整浮层尺寸;**调用方**负责把新的行列数
    // 通知 Vt 和 pty(vt.resize + pty.set_winsize),再 invalidate_all()。
    // 新字号放不下一格时返回 false(行列数保持原样不动)。
    bool set_fonts(const lv_font_t *font, const lv_font_t *ui_font, const lv_font_t *bold,
                   const lv_font_t *italic);

    // 0=块 1=下划线 2=竖线。blink=false 就常亮。
    void set_cursor_style(int style, bool blink);

    // 主题换了之后重新上色(画布底色、三个浮层)。屏上已有内容会跟着变色 ——
    // 格子里存的是调色板索引,不是 RGB。
    void refresh_theme();

    int cols() const { return _cols; }
    int rows() const { return _rows; }
    int cell_w() const { return _cell_w; }
    int cell_h() const { return _cell_h; }
    // 候选条贴底时要占的高度。别的浮层(小智)钉在底边上时把这一段让出来,才不压
    // 住候选字。
    int ime_reserve() const { return _ime_h; }

    // 把这一轮变脏的行(以及光标挪动前后的两格)标成需要重绘。调用方随后
    // vt.clear_dirty()。配合 lv_conf.h 的 PARTIAL 渲染模式,只会重绘这几行。
    void invalidate_dirty();
    void invalidate_all();

    // 光标闪烁 + 浮层自动收起。主循环每圈调一次。
    void tick(uint32_t now_ms);

    // 右上角角标:[中]/[EN] + 方案名。on=false 就整个收掉。
    void set_status(const std::string &text, bool on);
    // 候选条。cands 为空时收掉。
    void set_ime(const std::string &preedit, const std::vector<std::string> &cands, int highlight);
    // 居中浮一个提示,hold_ms 之后自动消失。
    void show_osd(const std::string &text, uint32_t now_ms, uint32_t hold_ms = 1000);

private:
    static void draw_cb(lv_event_t *e);
    void draw(lv_layer_t *layer);
    void draw_row(lv_layer_t *layer, const lv_area_t &org, int y, const lv_area_t &clip) const;
    void invalidate_cells(int x, int y, int n);
    // 量一下这套字体在 _w x _h 上能放几行几列(宽按 ASCII 最宽的那个推进宽度定,
    // 全角保证占得下两格)。一格都放不下返回 false。
    bool measure(const lv_font_t *font, int &cell_w, int &cell_h, int &cols, int &rows) const;
    // 给这一段的属性挑字体。粗+斜体同时出现时用粗体那一份(见 README)。
    const lv_font_t *font_for(uint8_t attr) const;
    // 候选条的竖位置。默认贴底,压住光标行时改成贴光标行上沿(见 .cpp)。
    void place_ime_bar();

    lv_obj_t *_canvas = nullptr;
    lv_obj_t *_ime_bar = nullptr;
    lv_obj_t *_ime_label = nullptr;
    lv_obj_t *_status = nullptr;
    lv_obj_t *_osd = nullptr;
    const lv_font_t *_font = nullptr;
    const lv_font_t *_ui_font = nullptr;
    const lv_font_t *_bold_font = nullptr;
    const lv_font_t *_italic_font = nullptr;
    Vt *_vt = nullptr;
    // sixel 图的 lv_image_dsc_t。lv_draw_image 只是浅拷贝 dsc,绘制任务要到 draw()
    // 返回之后才执行,所以描述符不能放栈上 —— 放这儿活到本帧画完(下一帧才可能重排)。
    std::vector<lv_image_dsc_t> _img_dscs;

    int _x = 0, _y = 0, _w = 0, _h = 0;
    int _cols = 0, _rows = 0, _cell_w = 1, _cell_h = 1;
    int _cursor_style = 0;
    bool _cursor_blink = true;
    int _last_cx = -1, _last_cy = -1;
    bool _last_cursor_visible = false;
    bool _cursor_on = true;
    // SGR 5/6 的文字闪烁:全局一个相位,2Hz 翻转。只影响带 kAttrBlink 的格。
    bool _text_blink_on = true;
    uint32_t _text_blink_at = 0;
    bool _ime_on = false;
    int _ime_h = 0;    // 候选条的高度(照 ui 字号算的),place_ime_bar 要用
    int _ime_top = -1; // 上次摆放的顶边(画布坐标);-1 = 还没摆过
    uint32_t _blink_at = 0;
    uint32_t _osd_hide_at = 0;
};

// 调色板 0-255 → 0xRRGGBB。0-15 走当前主题的表,16 以上是按公式算的色立方/灰阶。
uint32_t palette_rgb(uint8_t idx);

}  // namespace term
