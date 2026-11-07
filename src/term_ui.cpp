#include "term_ui.h"

#include "term_theme.h"
#include "term_vt.h"

#include <algorithm>

namespace term {

namespace {

// 光标(下划线/竖线)的粗细:按格子尺寸比例算,四舍五入到像素,最小 2px ——
// 1px 在 1024x768 的屏上细得看不见。
int cursor_thickness(int cell) {
    const int t = cell / 12;
    return t < 2 ? 2 : t;
}

uint32_t cell_fg(uint32_t c, uint8_t attr) {
    if(is_palette(c)) {
        uint8_t i = palette_index(c);
        // 加粗 = 用亮色,这是终端的传统做法(ls --color 的 di=01;34 靠它变亮)
        if((attr & kAttrBold) && i < 8) i += 8;
        return palette_rgb(i);
    }
    return true_color(c);
}

uint32_t cell_bg(uint32_t c) {
    return is_palette(c) ? palette_rgb(palette_index(c)) : true_color(c);
}

// 0xRRGGBB 三通道各按 pct% 从 c 混向 t。暗色(SGR 2)靠它把前景往背景压。
uint32_t blend_toward(uint32_t c, uint32_t t, int pct) {
    if(pct <= 0) return c;
    if(pct > 100) pct = 100;
    const int inv = 100 - pct;
    auto ch = [&](int shift) {
        const int a = (int)((c >> shift) & 0xffu);
        const int b = (int)((t >> shift) & 0xffu);
        return (a * inv + b * pct) / 100;
    };
    return ((uint32_t)ch(16) << 16) | ((uint32_t)ch(8) << 8) | (uint32_t)ch(0);
}

struct CellColors {
    uint32_t fg;
    uint32_t bg;
};

// 一格的最终前景/背景。顺序照终端的规矩:bold 先把 16 色前景提亮(见 cell_fg)→
// dim 把前景往背景压一半 → inverse 交换前后景。
CellColors cell_colors(const Cell &c) {
    uint32_t fg = cell_fg(c.fg, c.attr);
    uint32_t bg = cell_bg(c.bg);
    if(c.attr & kAttrDim) fg = blend_toward(fg, bg, 50);
    if(c.attr & kAttrInverse) {
        const uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }
    return {fg, bg};
}

// 推进宽度。lv_font_get_glyph_width 会自己走回退链,所以生僻字拿到的是回退字体的宽度。
int glyph_adv(const lv_font_t *f, uint32_t cp) {
    return (int)lv_font_get_glyph_width(f, cp ? cp : (uint32_t)' ', 0);
}

// 字形墨迹宽度(像素)和实际命中的字体。整条回退链都没有这个字返回 false
// (此时 LVGL 会画占位方块,宽度不说明问题)。
bool resolve_glyph(const lv_font_t *font, uint32_t cp, lv_font_glyph_dsc_t &d) {
    lv_memzero(&d, sizeof(d));
    if(!lv_font_get_glyph_dsc(font, &d, cp, 0)) return false;
    return !d.is_placeholder;
}

// 回退链里找一份「墨迹装得进一格」的同码位字形 —— 对应 fbterm 的 getNarrowGlyph:
// 图标右边没地方借的时候换窄字面(设备上是 NF-Mono),别压到邻格的字。
const lv_font_t *narrow_font_for(const lv_font_t *font, uint32_t cp, int max_w) {
    for(const lv_font_t *f = font; f; f = f->fallback) {
        if(!f->get_glyph_dsc) continue;
        lv_font_glyph_dsc_t d;
        lv_memzero(&d, sizeof(d));
        if(!f->get_glyph_dsc(f, &d, cp, 0) || d.is_placeholder) continue;
        if((int)d.box_w <= max_w) return f;
    }
    return nullptr;
}

// lv_area_intersect 在 lv_area_private.h 里,是个内部头。这里只要个「求交、空了就
// 返回 false」,自己写六行,不去攀内部头。
bool clip_area(lv_area_t &a, const lv_area_t &b) {
    if(a.x1 < b.x1) a.x1 = b.x1;
    if(a.y1 < b.y1) a.y1 = b.y1;
    if(a.x2 > b.x2) a.x2 = b.x2;
    if(a.y2 > b.y2) a.y2 = b.y2;
    return a.x1 <= a.x2 && a.y1 <= a.y2;
}

}  // namespace

// idx 0..15 是标准/亮色,16..231 是 6x6x6 色立方,232..255 是灰阶 —— 和 xterm 的
// 编号一样,这样应用报的 256 色索引在这边是对得上的。
//
// 0..15 走**当前主题**的表(这是换主题能整屏变色、不用 shell 重画的原因:格子存的是
// 索引);16 以上是按公式算出来的,和主题无关 —— 那些色是应用自己指定的,不该被主题改。
uint32_t palette_rgb(uint8_t idx) {
    if(idx < 16) return theme_current().palette[idx];
    if(idx < 232) {
        static const uint32_t ramp[6] = {0, 95, 135, 175, 215, 255};
        const int i = idx - 16;
        return (ramp[(i / 36) % 6] << 16) | (ramp[(i / 6) % 6] << 8) | ramp[i % 6];
    }
    const uint32_t g = 8u + (uint32_t)(idx - 232) * 10u;
    return (g << 16) | (g << 8) | g;
}

bool TermUi::measure(const lv_font_t *font, int &cell_w, int &cell_h, int &cols, int &rows) const {
    if(!font || _w <= 0 || _h <= 0) return false;
    // 格宽 = ASCII 可打印字符里最宽的那个推进宽度。比例字体下这是「怎么都不串格」的
    // 唯一定法:代价是窄字母(ilj)周围留白多,但光标、底色块和文字永远对得齐。
    int adv = 1;
    for(uint32_t c = 0x21; c <= 0x7e; ++c) adv = std::max(adv, glyph_adv(font, c));
    // 全角字占两格,得保证两个格宽装得下一个汉字,否则会和邻格叠上。
    const int cjk = glyph_adv(font, 0x4e2d);  // 中
    if(cjk > 0) adv = std::max(adv, (cjk + 1) / 2);

    int line_h = (int)lv_font_get_line_height(font);
    if(line_h < 1) line_h = 1;

    const int c = _w / adv;
    const int r = _h / line_h;
    if(c < 1 || r < 1) return false;

    cell_w = adv;
    cell_h = line_h;
    cols = c;
    rows = r;
    return true;
}

bool TermUi::create(lv_obj_t *parent, const lv_font_t *font, const lv_font_t *ui_font, int x,
                    int y, int w, int h) {
    if(!parent || !font) return false;
    _font = font;
    _ui_font = ui_font ? ui_font : font;
    _x = x;
    _y = y;
    _w = w;
    _h = h;
    if(!measure(_font, _cell_w, _cell_h, _cols, _rows)) return false;

    _canvas = lv_obj_create(parent);
    lv_obj_set_pos(_canvas, x, y);
    lv_obj_set_size(_canvas, w, h);
    lv_obj_set_style_radius(_canvas, 0, 0);
    lv_obj_set_style_border_width(_canvas, 0, 0);
    lv_obj_set_style_pad_all(_canvas, 0, 0);
    lv_obj_set_style_shadow_width(_canvas, 0, 0);
    lv_obj_set_style_bg_opa(_canvas, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_canvas, draw_cb, LV_EVENT_DRAW_MAIN, this);

    const int ui_h = (int)lv_font_get_line_height(_ui_font);
    _ime_h = 2 * ui_h + 20;

    // 候选条:一条浮层,只在组字时出现(竖位置由 place_ime_bar 每轮定)。
    _ime_bar = lv_obj_create(parent);
    lv_obj_set_size(_ime_bar, w, _ime_h);
    lv_obj_set_style_radius(_ime_bar, 0, 0);
    lv_obj_set_style_border_width(_ime_bar, 0, 0);
    lv_obj_set_style_pad_all(_ime_bar, 8, 0);
    lv_obj_set_style_bg_opa(_ime_bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_ime_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_ime_bar, LV_OBJ_FLAG_HIDDEN);
    _ime_label = lv_label_create(_ime_bar);
    lv_obj_set_style_text_font(_ime_label, _ui_font, 0);
    lv_obj_set_style_text_line_space(_ime_label, 4, 0);
    lv_obj_set_width(_ime_label, w - 16);
    lv_label_set_long_mode(_ime_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(_ime_label, LV_ALIGN_LEFT_MID, 0, 0);

    // 右上角角标([中] 雾凇拼音 / [EN])。
    _status = lv_label_create(parent);
    lv_obj_set_style_text_font(_status, _ui_font, 0);
    lv_obj_set_style_bg_opa(_status, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(_status, 4, 0);
    lv_obj_set_style_radius(_status, 6, 0);
    lv_obj_add_flag(_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_status, LV_ALIGN_TOP_RIGHT, -8, 8);

    // 背光 OSD:居中一块,1 秒后自己收掉(tick 里管)。
    _osd = lv_label_create(parent);
    lv_obj_set_style_text_font(_osd, _ui_font, 0);
    lv_obj_set_style_bg_opa(_osd, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(_osd, 12, 0);
    lv_obj_set_style_radius(_osd, 8, 0);
    lv_obj_add_flag(_osd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_osd, LV_ALIGN_CENTER, 0, 0);

    refresh_theme();
    return true;
}

void TermUi::attach(Vt *vt) {
    _vt = vt;
    // Vt 淘汰 sixel 旧图时要按格高把图像高度折算成行数,所以把格子尺寸告诉它。
    if(_vt) _vt->set_cell_size(_cell_w, _cell_h);
}

bool TermUi::set_fonts(const lv_font_t *font, const lv_font_t *ui_font, const lv_font_t *bold,
                       const lv_font_t *italic) {
    if(!font) return false;
    // 先量,量得出来才动成员 —— 半途失败会把 _font 换成放不下的那份,而行列数还是旧的。
    int cw = 0, ch = 0, c = 0, r = 0;
    if(!measure(font, cw, ch, c, r)) return false;

    _font = font;
    _ui_font = ui_font ? ui_font : font;
    _bold_font = bold;
    _italic_font = italic;
    _cell_w = cw;
    _cell_h = ch;
    _cols = c;
    _rows = r;
    if(_vt) _vt->set_cell_size(cw, ch);

    // 浮层的字号也跟着换,高度是照它算的。
    const int ui_h = (int)lv_font_get_line_height(_ui_font);
    if(_ime_bar) {
        _ime_h = 2 * ui_h + 20;
        _ime_top = -1;  // 格高和条高都变了,摆放位置作废
        lv_obj_set_size(_ime_bar, _w, _ime_h);
        lv_obj_set_style_text_font(_ime_label, _ui_font, 0);
        lv_obj_set_width(_ime_label, _w - 16);
    }
    if(_status) lv_obj_set_style_text_font(_status, _ui_font, 0);
    if(_osd) lv_obj_set_style_text_font(_osd, _ui_font, 0);

    // 网格换了几何,记录的光标位置作废 —— 新光标位置靠下一轮 invalidate_dirty 重新
    // 认一遍,不然会拿旧格子去标脏,抹掉别处的画面。
    _last_cx = -1;
    _last_cy = -1;
    return true;
}

void TermUi::set_cursor_style(int style, bool blink) {
    if(style < 0 || style > 2) style = 0;
    if(_cursor_style == style && _cursor_blink == blink) return;
    _cursor_style = style;
    _cursor_blink = blink;
    _cursor_on = true;  // 换了样式先亮着,不然关闪烁又刚好停在灭的那半拍会一直看不见
    _blink_at = 0;
    invalidate_cells(_vt ? _vt->cursor_x() : 0, _vt ? _vt->cursor_y() : 0, 2);
}

void TermUi::refresh_theme() {
    const TermTheme &th = theme_current();
    if(_canvas) lv_obj_set_style_bg_color(_canvas, lv_color_hex(th.palette[0]), 0);
    if(_ime_bar) lv_obj_set_style_bg_color(_ime_bar, lv_color_hex(th.panel_bg), 0);
    if(_ime_label) lv_obj_set_style_text_color(_ime_label, lv_color_hex(th.panel_fg), 0);
    if(_status) {
        lv_obj_set_style_text_color(_status, lv_color_hex(th.status), 0);
        lv_obj_set_style_bg_color(_status, lv_color_hex(th.palette[0]), 0);
    }
    if(_osd) {
        lv_obj_set_style_text_color(_osd, lv_color_hex(th.panel_fg), 0);
        lv_obj_set_style_bg_color(_osd, lv_color_hex(th.panel_bg), 0);
    }
    invalidate_all();
}

const lv_font_t *TermUi::font_for(uint8_t attr) const {
    // 粗+斜体同时来的时候用粗体那一份:两份字体文件没法叠,再合成一份又多占一份内存,
    // 而 SGR 1;3 在实际输出里很少见。合成那条路上两个位是一起给的(见 main.cpp)。
    if((attr & kAttrBold) && _bold_font) return _bold_font;
    if((attr & kAttrItalic) && _italic_font) return _italic_font;
    return _font;
}

void TermUi::draw_cb(lv_event_t *e) {
    TermUi *ui = (TermUi *)lv_event_get_user_data(e);
    if(ui) ui->draw(lv_event_get_layer(e));
}

void TermUi::draw(lv_layer_t *layer) {
    if(!_vt || !layer) return;
    lv_area_t org;
    lv_obj_get_coords(_canvas, &org);
    // 绘制任务建立期间 layer->_clip_area 就是当前裁剪区(LVGL 头里写明了这个用法),
    // 拿它把不在重绘范围内的行整行跳过 —— 平时只有一两行是脏的,省下的是大头。
    const lv_area_t clip = layer->_clip_area;

    // ---- sixel 图像:先当背景铺上,后面画的底色块/文字叠在它上面 ----
    // 主屏的锚点是绝对行,和文本同一套坐标系,滚动/回看时图跟着走;备用屏(yazi 这类
    // 全屏 TUI)没有回看,锚点直接是屏幕行。
    {
        const std::vector<SixelImage> &imgs = _vt->images();
        _img_dscs.resize(imgs.size());
        const int top_abs = _vt->abs_line_of_screen_row(0);
        const bool alt = _vt->alt_screen();
        for(size_t k = 0; k < imgs.size(); ++k) {
            const SixelImage &img = imgs[k];
            if(img.alt != alt) continue;  // 备用屏上不画主屏的图,反之亦然
            const lv_coord_t ix = (lv_coord_t)(org.x1 + img.cell_x * _cell_w);
            const lv_coord_t iy = (lv_coord_t)(org.y1 + (img.alt ? img.abs_line
                                                                 : img.abs_line - top_abs) *
                                                            _cell_h);
            lv_area_t full{ix, iy, (lv_coord_t)(ix + img.w - 1), (lv_coord_t)(iy + img.h - 1)};
            lv_area_t vis = full;
            if(!clip_area(vis, clip)) continue;  // 整张都不在可见区

            lv_image_dsc_t &idsc = _img_dscs[k];
            lv_memzero(&idsc, sizeof(idsc));
            idsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            idsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
            idsc.header.w = (uint32_t)img.w;
            idsc.header.h = (uint32_t)img.h;
            idsc.header.stride = (uint32_t)img.w * 4u;
            idsc.data_size = (uint32_t)img.argb.size() * 4u;
            idsc.data = (const uint8_t *)img.argb.data();

            lv_draw_image_dsc_t dsc;
            lv_draw_image_dsc_init(&dsc);
            dsc.src = &idsc;
            // coords 要传整张图的位置(LVGL 自己按 layer 裁剪区裁),不能传裁过的,
            // 否则图像会被挪到裁剪区左上角。
            lv_draw_image(layer, &dsc, &full);
        }
    }

    for(int y = 0; y < _rows; ++y) {
        const int py = org.y1 + y * _cell_h;
        if(py + _cell_h - 1 < clip.y1 || py > clip.y2) continue;
        draw_row(layer, org, y, clip);
    }
}

void TermUi::draw_row(lv_layer_t *layer, const lv_area_t &org, int y, const lv_area_t &clip) const {
    const Cell *row = _vt->display_row(y);
    if(!row) return;
    const int py = org.y1 + y * _cell_h;

    lv_draw_rect_dsc_t rdsc;
    lv_draw_rect_dsc_init(&rdsc);
    rdsc.radius = 0;
    rdsc.border_width = 0;
    rdsc.bg_opa = LV_OPA_COVER;

    // ---- 底色:只铺非默认底色的连续段(默认底色就是画布自己的背景) ----
    // 「默认底色」是当前主题的 palette[0] —— 画布背景已经铺的就是它,再铺一遍是白干。
    // 用 cell_colors 的**有效** bg:反显格的底色变成了原来的前景色,自然就铺出来了。
    const uint32_t default_bg = theme_current().palette[0];
    for(int x = 0; x < _cols;) {
        const uint32_t bg = cell_colors(row[x]).bg;
        if(bg == default_bg) {
            ++x;
            continue;
        }
        int e = x + 1;
        while(e < _cols && cell_colors(row[e]).bg == bg) ++e;
        lv_area_t a{org.x1 + x * _cell_w, py, org.x1 + e * _cell_w - 1, py + _cell_h - 1};
        if(clip_area(a, clip)) {
            rdsc.bg_color = lv_color_hex(bg);
            lv_draw_rect(layer, &rdsc, &a);
        }
        x = e;
    }

    // ---- 文字:按前景/属性分段,段内一格一个字形,在格子里居中 ----
    // 分段键是 (attr, fg),所以段内属性一致 —— 粗/斜体挑字面在段这一级做一次就够,
    // 不用逐格换。
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);

    for(int x = 0; x < _cols;) {
        const Cell &c = row[x];
        if(c.attr & kAttrWideTail) {
            ++x;
            continue;
        }
        const uint32_t fg = cell_colors(c).fg;
        const uint8_t attr = (uint8_t)(c.attr & ~kAttrWideTail);
        int e = x + 1;
        while(e < _cols) {
            const Cell &n = row[e];
            if(n.attr & kAttrWideTail) {
                ++e;  // 属于前一个全角字的右半格,跟着那一段走
                continue;
            }
            if((uint8_t)(n.attr & ~kAttrWideTail) != attr || cell_colors(n).fg != fg) break;
            ++e;
        }
        dsc.color = lv_color_hex(fg);
        const lv_font_t *seg_font = font_for(attr);
        dsc.font = seg_font;

        for(int i = x; i < e; ++i) {
            const Cell &g = row[i];
            if(g.attr & kAttrWideTail) continue;  // 全角字的右半格,头那一格已经画了
            // 闪烁灭的半拍:字不画,底色留着(SGR 5/6 的常规表现)。
            if((g.attr & kAttrBlink) && !_text_blink_on) continue;
            const uint32_t cp = g.ch ? g.ch : (uint32_t)' ';
            if(cp == ' ') continue;  // 空格只要底色,没有字形
            const bool wide = (i + 1 < e) && (row[i + 1].attr & kAttrWideTail);

            const lv_font_t *gfont = seg_font;
            int box = (wide ? 2 : 1) * _cell_w;
            // PUA 图标(Nerd Font):advance 是一格,墨迹却往往比一格宽(比如终端图标
            // 在 30 号字下墨迹 26px、格子才 15px)。右边空着就借那格把它整个画出来;
            // 右边有字就换回退链里的窄字面;实在没有才裁。这是 fbterm 的 overflow +
            // narrow-fallback 策略。
            bool natural = false;
            if(!wide && is_private_use(cp)) {
                lv_font_glyph_dsc_t gd;
                if(resolve_glyph(seg_font, cp, gd) && (int)gd.box_w > _cell_w) {
                    const bool next_free =
                        i + 1 < _cols && !(row[i + 1].attr & kAttrWideTail) &&
                        (row[i + 1].ch == 0 || row[i + 1].ch == ' ');
                    if(!next_free) {
                        if(const lv_font_t *nf = narrow_font_for(seg_font, cp, _cell_w)) {
                            gfont = nf;
                            resolve_glyph(nf, cp, gd);
                        }
                    }
                    if((int)gd.box_w > _cell_w) {
                        box = (i + 1 < _cols ? 2 : 1) * _cell_w;
                        natural = true;  // 图标从格原点起画,往右溢出
                    }
                }
            }

            const int px = org.x1 + i * _cell_w;
            if(wide) ++i;
            if(px + box - 1 < clip.x1 || px > clip.x2) continue;
            if(py + _cell_h - 1 < clip.y1 || py > clip.y2) continue;

            dsc.font = gfont;
            // 会溢出的图标靠左站(和字形本身的左侧留白连着),普通字在格子里居中。
            const int dx = natural ? 0 : (box - glyph_adv(gfont, cp)) / 2;
            lv_point_t pt{(lv_coord_t)(px + dx), (lv_coord_t)py};
            // 把这一格的矩形压进裁剪区再建绘制任务,字形就不会溢到邻格上。
            const lv_area_t saved = layer->_clip_area;
            lv_area_t cell{px, py, px + box - 1, py + _cell_h - 1};
            if(clip_area(cell, saved)) layer->_clip_area = cell;
            lv_draw_character(layer, &dsc, &pt, cp);
            layer->_clip_area = saved;
        }
        x = e;
    }

    // ---- 下划线 / 删除线:单独一趟 ----
    // 不能并进画字那趟(那里空格和全角尾格都跳过了),而线是要画在它们上面的。
    for(int x = 0; x < _cols; ++x) {
        const Cell &g = row[x];
        const uint8_t a = (uint8_t)(g.attr & ~kAttrWideTail);
        if(!(a & (kAttrUnderline | kAttrStrike))) continue;
        if((a & kAttrBlink) && !_text_blink_on) continue;  // 跟着字一起闪
        const int px = org.x1 + x * _cell_w;
        if(px + _cell_w - 1 < clip.x1 || px > clip.x2) continue;
        const int t = cursor_thickness(_cell_h);
        rdsc.bg_color = lv_color_hex(cell_colors(g).fg);
        if(a & kAttrUnderline) {
            lv_area_t bar{px, py + _cell_h - t, px + _cell_w - 1, py + _cell_h - 1};
            if(clip_area(bar, clip)) lv_draw_rect(layer, &rdsc, &bar);
        }
        if(a & kAttrStrike) {
            const int mid = py + _cell_h / 2;
            lv_area_t bar{px, mid - t / 2, px + _cell_w - 1, mid - t / 2 + t - 1};
            if(clip_area(bar, clip)) lv_draw_rect(layer, &rdsc, &bar);
        }
    }

    // ---- 光标 ----
    if(y != _vt->cursor_y() || !_cursor_on || !_vt->cursor_visible()) return;
    const int cx = _vt->cursor_x();
    if(cx >= _cols) return;
    const bool cwide = (cx + 1 < _cols) && (row[cx + 1].attr & kAttrWideTail);
    const int cbox = (cwide ? 2 : 1) * _cell_w;
    lv_area_t ca{org.x1 + cx * _cell_w, py, org.x1 + cx * _cell_w + cbox - 1, py + _cell_h - 1};
    if(!clip_area(ca, clip)) return;

    const TermTheme &th = theme_current();
    const uint32_t cp = row[cx].ch;
    const lv_font_t *cfont = font_for((uint8_t)(row[cx].attr & ~kAttrWideTail));

    if(_cursor_style == 0) {
        // 实心块 + 把字用底色重画一遍(块下面是字,不重画就看不见)。
        rdsc.bg_color = lv_color_hex(th.cursor);
        lv_draw_rect(layer, &rdsc, &ca);
        if(cp == 0 || cp == ' ') return;
        dsc.font = cfont;
        dsc.color = lv_color_hex(th.palette[0]);
        lv_point_t pt{(lv_coord_t)(ca.x1 + (cbox - glyph_adv(cfont, cp)) / 2), (lv_coord_t)py};
        const lv_area_t saved = layer->_clip_area;
        layer->_clip_area = ca;
        lv_draw_character(layer, &dsc, &pt, cp);
        layer->_clip_area = saved;
        return;
    }

    // 下划线/竖线:字刚才照常画过了,这里只在上面压一条细横杠/竖杠。
    lv_area_t bar = ca;
    if(_cursor_style == 1) {
        const int t = cursor_thickness(_cell_h);
        bar.y1 = ca.y2 - t + 1;
    } else {
        const int t = cursor_thickness(_cell_w);
        bar.x2 = ca.x1 + t - 1;
    }
    rdsc.bg_color = lv_color_hex(th.cursor);
    lv_draw_rect(layer, &rdsc, &bar);
}

void TermUi::invalidate_cells(int x, int y, int n) {
    if(!_canvas || y < 0 || y >= _rows || x < 0 || x >= _cols) return;
    if(n > _cols - x) n = _cols - x;
    if(n < 1) return;
    lv_area_t org;
    lv_obj_get_coords(_canvas, &org);
    lv_area_t a{org.x1 + x * _cell_w, org.y1 + y * _cell_h, org.x1 + (x + n) * _cell_w - 1,
                org.y1 + (y + 1) * _cell_h - 1};
    lv_obj_invalidate_area(_canvas, &a);
}

void TermUi::invalidate_dirty() {
    if(!_vt || !_canvas) return;

    // 光标挪过位置(或者被应用藏起来/放出来)时,原来那一格也得重绘 —— 光标不改变
    // 网格内容,VT 那边不会因此把它标脏。
    const int cx = _vt->cursor_x();
    const int cy = _vt->cursor_y();
    const bool vis = _vt->cursor_visible() && _cursor_on;
    if(cx != _last_cx || cy != _last_cy || vis != _last_cursor_visible) {
        if(_last_cursor_visible) invalidate_cells(_last_cx, _last_cy, 2);
        if(vis) invalidate_cells(cx, cy, 2);
        _last_cx = cx;
        _last_cy = cy;
        _last_cursor_visible = vis;
    }

    for(int y = 0; y < _rows; ++y)
        if(_vt->row_dirty(y)) invalidate_cells(0, y, _cols);
}

void TermUi::invalidate_all() {
    if(_canvas) lv_obj_invalidate(_canvas);
}

void TermUi::tick(uint32_t now_ms) {
    // 关了闪烁就一直亮着(_cursor_on 由 set_cursor_style() 保证是 true)。
    if(_cursor_blink && _vt && _vt->cursor_visible() && now_ms - _blink_at >= 500) {
        _blink_at = now_ms;
        _cursor_on = !_cursor_on;
        invalidate_cells(_vt->cursor_x(), _vt->cursor_y(), 2);
        _last_cx = _vt->cursor_x();
        _last_cy = _vt->cursor_y();
        _last_cursor_visible = _cursor_on;
    }
    // SGR 5/6 的文字闪烁:和光标同一个 2Hz 节拍,但只重画**含闪烁格**的行,
    // 不用整屏刷。屏上没闪烁格时这一趟就是空转,代价可以忽略。
    if(_vt && now_ms - _text_blink_at >= 500) {
        _text_blink_at = now_ms;
        _text_blink_on = !_text_blink_on;
        for(int y = 0; y < _rows; ++y) {
            const Cell *r = _vt->display_row(y);
            if(!r) continue;
            for(int x = 0; x < _cols; ++x) {
                if(r[x].attr & kAttrBlink) {
                    invalidate_cells(0, y, _cols);
                    break;
                }
            }
        }
    }
    // 组字期间 shell 里跑的东西还在动(进度条、tail -f),光标会被推着走 ——
    // 每轮重算一次条的竖位置,别让它回头又压到光标行上。
    if(_ime_on) place_ime_bar();
    if(_osd_hide_at && (int32_t)(now_ms - _osd_hide_at) >= 0) {
        _osd_hide_at = 0;
        if(_osd) lv_obj_add_flag(_osd, LV_OBJ_FLAG_HIDDEN);
    }
}

void TermUi::set_status(const std::string &text, bool on) {
    if(!_status) return;
    if(!on || text.empty()) {
        lv_obj_add_flag(_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(_status, text.c_str());
    lv_obj_remove_flag(_status, LV_OBJ_FLAG_HIDDEN);
}

// 光标戳在屏幕底部时,贴底的候选条会把正在打的那一行整个盖住 —— 看不到自己
// 组的是什么字。所以只在条和光标行不重叠时才贴底,一旦压上就改成贴着光标行
// 的上沿摆,让候选条始终挨着打字的地方。条高固定,只跟光标所在行有关。
void TermUi::place_ime_bar() {
    if(!_ime_bar || !_vt || _ime_h <= 0) return;
    int top = _h - _ime_h;                               // 默认:贴底
    const int cur_top = _vt->cursor_y() * _cell_h;
    if(cur_top + _cell_h > top) top = cur_top - _ime_h;  // 撞上光标行,挪到它正上方
    if(top < 0) top = 0;                                 // 连上面也放不下就顶到屏幕顶
    if(top == _ime_top) return;                          // 没挪动就别白标一次脏
    _ime_top = top;
    lv_obj_align(_ime_bar, LV_ALIGN_TOP_MID, 0, _y + top);
}

void TermUi::set_ime(const std::string &preedit, const std::vector<std::string> &cands,
                     int highlight) {
    if(!_ime_bar || !_ime_label) return;
    if(preedit.empty() && cands.empty()) {
        if(_ime_on) {
            _ime_on = false;
            lv_obj_add_flag(_ime_bar, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    std::string s = preedit;
    if(!cands.empty()) {
        if(!s.empty()) s += '\n';
        for(size_t i = 0; i < cands.size(); ++i) {
            if(i) s += "  ";
            // 编号就是 rime 的选字键:这一页第几个,按数字几就选中它。不标出来用户
            // 没法知道哪个候选对应哪个数字。
            s += std::to_string(i + 1);
            s += '.';
            if((int)i == highlight) {
                s += '[';
                s += cands[i];
                s += ']';
            } else {
                s += cands[i];
            }
        }
    }
    lv_label_set_text(_ime_label, s.c_str());
    if(!_ime_on) {
        _ime_on = true;
        lv_obj_remove_flag(_ime_bar, LV_OBJ_FLAG_HIDDEN);
    }
    place_ime_bar();
}

void TermUi::show_osd(const std::string &text, uint32_t now_ms, uint32_t hold_ms) {
    if(!_osd) return;
    lv_label_set_text(_osd, text.c_str());
    lv_obj_remove_flag(_osd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_osd);
    _osd_hide_at = now_ms + hold_ms;
}

}  // namespace term
