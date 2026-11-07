#include "term_vt.h"

#include "term_sixel.h"

#include <algorithm>
#include <wchar.h>

namespace term {

namespace {

// 单张 sixel 解码后的像素上限(宽*高)。整屏 1024x768 才 78 万,400 万够放好几张,
// 又拦得住畸形/恶意流把内存撑爆。图像**总**字节上限见 kImageBytesCap。
constexpr size_t kSixelMaxPixels = 4u * 1024u * 1024u;
// 所有 sixel 图占的内存上限(ARGB8888 = 4 字节/像素)。超了按到达先后淘汰最老的。
constexpr size_t kImageBytesCap = 8u * 1024u * 1024u;
// DCS 载荷(正在收的原始字节)上限,防 ST 迟迟不来时无限涨。
constexpr size_t kDcsBufCap = 4u * 1024u * 1024u;

// 把码位编成 UTF-8。Cell 里存的是码位,画的时候 LVGL 要的是字节串。
void append_utf8(std::string &out, uint32_t cp) {
    if(cp < 0x80) {
        out.push_back((char)cp);
    } else if(cp < 0x800) {
        out.push_back((char)(0xc0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    } else if(cp < 0x10000) {
        out.push_back((char)(0xe0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    } else {
        out.push_back((char)(0xf0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    }
}

// DEC Special Graphics(ESC ( 0)。ncurses 画框线、top 的柱状图都走这里。
uint32_t dec_graphics(uint32_t cp) {
    switch(cp) {
    case '_': return 0x00a0;  // blank
    case '`': return 0x25c6;
    case 'a': return 0x2592;
    case 'b': return 0x2409;
    case 'c': return 0x240c;
    case 'd': return 0x240d;
    case 'e': return 0x240a;
    case 'f': return 0x00b0;
    case 'g': return 0x00b1;
    case 'h': return 0x2424;
    case 'i': return 0x240b;
    case 'j': return 0x2518;
    case 'k': return 0x2510;
    case 'l': return 0x250c;
    case 'm': return 0x2514;
    case 'n': return 0x253c;
    case 'o': return 0x23ba;
    case 'p': return 0x23bb;
    case 'q': return 0x2500;
    case 'r': return 0x23bc;
    case 's': return 0x23bd;
    case 't': return 0x251c;
    case 'u': return 0x2524;
    case 'v': return 0x2534;
    case 'w': return 0x252c;
    case 'x': return 0x2502;
    case 'y': return 0x2264;
    case 'z': return 0x2265;
    case '{': return 0x03c0;
    case '|': return 0x2260;
    case '}': return 0x00a3;
    case '~': return 0x00b7;
    default: return cp;
    }
}

}  // namespace

Vt::Vt(int cols, int rows) {
    resize(cols, rows);
}

// ---------------------------------------------------------------- 网格

Cell Vt::blank_cell() const {
    return Cell{};
}

// 擦除用的空格:底色跟当前 SGR 走(xterm 的 BCE,terminfo 里 xterm-256color
// 声明了 back_color_erase,应用指望这个)。
Cell Vt::erase_cell() const {
    Cell c;
    c.fg = _attrs.fg;
    c.bg = _attrs.bg;
    return c;
}

Vt::Row Vt::make_row() const {
    Row r((size_t)(_cols > 0 ? _cols : 1));
    for(size_t i = 0; i < r.size(); ++i) r[i] = blank_cell();
    return r;
}

void Vt::fill_row(Row &r, int from) const {
    for(int x = from; x < _cols && x < (int)r.size(); ++x) r[x] = erase_cell();
}

void Vt::resize(int cols, int rows) {
    if(cols < 1) cols = 1;
    if(rows < 1) rows = 1;
    if(cols == _cols && rows == _rows && (int)_grid.size() == cols * rows) return;

    const bool was_full_width = (_scroll_bottom == _rows - 1);
    const bool width_changed = (cols != _cols);

    Row ng((size_t)cols * (size_t)rows);
    Row na((size_t)cols * (size_t)rows);
    for(size_t i = 0; i < ng.size(); ++i) {
        ng[i] = blank_cell();
        na[i] = blank_cell();
    }

    // 第一次 resize(构造时)两个网格都还是空的,没有旧内容可搬
    const int cw = _grid.empty() ? 0 : std::min(cols, _cols);
    const int rh = _grid.empty() ? 0 : std::min(rows, _rows);
    for(int y = 0; y < rh; ++y) {
        for(int x = 0; x < cw; ++x) {
            ng[(size_t)y * cols + x] = _grid[(size_t)y * _cols + x];
            na[(size_t)y * cols + x] = _alt_grid[(size_t)y * _cols + x];
        }
    }

    _grid.swap(ng);
    _alt_grid.swap(na);
    _cols = cols;
    _rows = rows;

    // 回看里存的是老宽度的行,宽度一变就得跟着补齐/截断,不然渲染按新 _cols
    // 去读会越界。
    if(width_changed) {
        for(Row &r : _scrollback) {
            r.resize((size_t)cols, blank_cell());
            // 截断处可能只剩下双宽字的一半,抹掉它
            if(!r.empty() && (r.back().attr & kAttrWideTail)) r.back() = blank_cell();
        }
    }

    _dirty.assign((size_t)rows, 1);
    _dirty_all = true;

    if(was_full_width || _scroll_bottom >= _rows) _scroll_bottom = _rows - 1;
    if(_scroll_top >= _rows) _scroll_top = 0;
    if(_scroll_bottom <= _scroll_top) {
        _scroll_top = 0;
        _scroll_bottom = _rows - 1;
    }
    clamp_cursor();
}

void Vt::clear_region(int y0, int x0, int y1, int x1) {
    if(y0 < 0) y0 = 0;
    if(x0 < 0) x0 = 0;
    if(y1 >= _rows) y1 = _rows - 1;
    if(x1 >= _cols) x1 = _cols - 1;
    if(_grid.empty()) return;
    for(int y = y0; y <= y1; ++y) {
        Cell *row = &_grid[(size_t)y * _cols];
        for(int x = x0; x <= x1; ++x) row[x] = erase_cell();
    }
    mark_dirty_range(y0, y1);
}

void Vt::scroll_up(int n) {
    if(n < 1) n = 1;
    const int top = _scroll_top, bot = _scroll_bottom;
    const int h = bot - top + 1;
    if(h <= 0) return;
    if(n > h) n = h;

    // 只有整屏滚动才进回看;滚动区域里的滚动和备用屏期间都不算历史。
    const bool to_scrollback = (!_alt_active && top == 0 && bot == _rows - 1);
    // 备用屏是原地滚动:图跟着文字走,滚出屏幕的直接丢(图不裁剪)。
    if(_alt_active) {
        for(auto it = _images.begin(); it != _images.end();) {
            if(!it->alt) { ++it; continue; }
            const int span = it->h > 0 ? (it->h + _cell_h - 1) / _cell_h : 0;
            const int y0 = it->abs_line, y1 = y0 + span - 1;
            if(y1 < top || y0 > bot) { ++it; continue; }
            if(y1 > bot - n || y0 < top) {
                mark_dirty_range(y0, y1);
                it = _images.erase(it);
                continue;
            }
            mark_dirty_range(y0 - n, y1 - n);
            it->abs_line -= n;
            ++it;
        }
    }
    for(int i = 0; i < n && to_scrollback; ++i) {
        _scrollback.emplace_back(_grid.begin(), _grid.begin() + _cols);
        ++_abs_top;  // 顶行让给了回看,sixel 锚点的绝对行号跟着抬
        if(_scrollback.size() > _max_scrollback) _scrollback.pop_front();
        if(_view_offset > 0) {
            ++_view_offset;
            if(_view_offset > (int)_scrollback.size()) _view_offset = (int)_scrollback.size();
        }
    }
    if(to_scrollback) trim_images();

    for(int y = top; y <= bot - n; ++y) {
        std::copy(_grid.begin() + (size_t)(y + n) * _cols,
                  _grid.begin() + (size_t)(y + n) * _cols + _cols,
                  _grid.begin() + (size_t)y * _cols);
    }
    for(int y = bot - n + 1; y <= bot; ++y) {
        Cell *row = &_grid[(size_t)y * _cols];
        for(int x = 0; x < _cols; ++x) row[x] = erase_cell();
    }
    mark_dirty_range(top, bot);
}

void Vt::scroll_down(int n) {
    if(n < 1) n = 1;
    const int top = _scroll_top, bot = _scroll_bottom;
    const int h = bot - top + 1;
    if(h <= 0) return;
    if(n > h) n = h;

    if(_alt_active) {
        for(auto it = _images.begin(); it != _images.end();) {
            if(!it->alt) { ++it; continue; }
            const int span = it->h > 0 ? (it->h + _cell_h - 1) / _cell_h : 0;
            const int y0 = it->abs_line, y1 = y0 + span - 1;
            if(y1 < top || y0 > bot) { ++it; continue; }
            if(y0 < top + n || y1 + n > bot) {
                mark_dirty_range(y0, y1);
                it = _images.erase(it);
                continue;
            }
            mark_dirty_range(y0 + n, y1 + n);
            it->abs_line += n;
            ++it;
        }
    }

    for(int y = bot; y >= top + n; --y) {
        std::copy(_grid.begin() + (size_t)(y - n) * _cols,
                  _grid.begin() + (size_t)(y - n) * _cols + _cols,
                  _grid.begin() + (size_t)y * _cols);
    }
    for(int y = top; y < top + n; ++y) {
        Cell *row = &_grid[(size_t)y * _cols];
        for(int x = 0; x < _cols; ++x) row[x] = erase_cell();
    }
    mark_dirty_range(top, bot);
}

void Vt::reverse_index() {
    _wrap_pending = false;
    if(_cy == _scroll_top) scroll_down(1);
    else if(_cy > 0) --_cy;
}

void Vt::fix_wide(int y, int x) {
    if(y < 0 || y >= _rows || x < 0 || x >= _cols) return;
    Cell *row = &_grid[(size_t)y * _cols];
    if(row[x].attr & kAttrWideTail) {
        // 我们正踩在一个宽字的右半格上,左半也要一起抹掉
        if(x > 0) row[x - 1] = erase_cell();
        row[x] = erase_cell();
    } else if(x + 1 < _cols && (row[x + 1].attr & kAttrWideTail)) {
        row[x + 1] = erase_cell();
    }
}

// ---------------------------------------------------------------- 光标

void Vt::mark_dirty(int y) {
    if(y < 0 || y >= _rows) {
        _dirty_all = true;
        return;
    }
    if(!_dirty_all && (size_t)y < _dirty.size()) _dirty[(size_t)y] = 1;
}

void Vt::mark_dirty_range(int y0, int y1) {
    if(y0 < 0) y0 = 0;
    if(y1 >= _rows) y1 = _rows - 1;
    if(_dirty_all) return;
    for(int y = y0; y <= y1; ++y)
        if((size_t)y < _dirty.size()) _dirty[(size_t)y] = 1;
}

void Vt::set_dirty_all() {
    _dirty_all = true;
    std::fill(_dirty.begin(), _dirty.end(), 1);
}

void Vt::set_cursor(int x, int y) {
    _wrap_pending = false;
    _cx = x;
    _cy = y;
    clamp_cursor();
}

void Vt::clamp_cursor() {
    if(_cx < 0) _cx = 0;
    if(_cx > _cols - 1) _cx = _cols - 1;
    if(_cy < 0) _cy = 0;
    if(_cy > _rows - 1) _cy = _rows - 1;
}

void Vt::carriage_return() {
    _cx = 0;
    _wrap_pending = false;
}

void Vt::line_feed() {
    _wrap_pending = false;
    if(_newline_mode) _cx = 0;
    if(_cy == _scroll_bottom) {
        scroll_up(1);
    } else if(_cy < _rows - 1) {
        ++_cy;
    }
}

void Vt::tab() {
    _wrap_pending = false;
    const int next = ((_cx / 8) + 1) * 8;
    _cx = next < _cols ? next : _cols - 1;
}

void Vt::save_cursor_to(SavedCursor &s) {
    s.x = _cx;
    s.y = _cy;
    s.attrs = _attrs;
    s.origin_mode = _origin_mode;
    s.valid = true;
}

void Vt::restore_cursor_from(const SavedCursor &s) {
    if(!s.valid) return;
    _cx = s.x;
    _cy = s.y;
    _attrs = s.attrs;
    _origin_mode = s.origin_mode;
    _wrap_pending = false;
    clamp_cursor();
}

void Vt::save_cursor() { save_cursor_to(_saved); }
void Vt::restore_cursor() { restore_cursor_from(_saved); }

// ---------------------------------------------------------------- 备用屏

void Vt::alt_clear() {
    for(size_t i = 0; i < _grid.size(); ++i) _grid[i] = blank_cell();
    _cx = _cy = 0;
    _wrap_pending = false;
    set_dirty_all();
}

void Vt::alt_enter(bool save_cur, bool clear) {
    if(_alt_active) return;
    if(save_cur) save_cursor_to(_saved_1049);
    std::swap(_grid, _alt_grid);
    _alt_active = true;
    _scroll_top = 0;
    _scroll_bottom = _rows - 1;
    // 进备用屏时光标回原点(1049 之后 restore 会把它放回去)
    _cx = _cy = 0;
    _wrap_pending = false;
    if(clear) alt_clear();
    else set_dirty_all();
}

void Vt::alt_exit(bool restore_cur) {
    if(!_alt_active) return;
    // 备用屏上的图随备用屏一起丢掉;主屏的图像原样保留。
    _images.erase(std::remove_if(_images.begin(), _images.end(),
                                 [](const SixelImage &im) { return im.alt; }),
                  _images.end());
    std::swap(_grid, _alt_grid);
    _alt_active = false;
    _scroll_top = 0;
    _scroll_bottom = _rows - 1;
    set_dirty_all();
    if(restore_cur) restore_cursor_from(_saved_1049);
}

// ---------------------------------------------------------------- 回看

void Vt::set_view_offset(int off) {
    const int maxoff = (int)_scrollback.size();
    if(off < 0) off = 0;
    if(off > maxoff) off = maxoff;
    if(off == _view_offset) return;
    _view_offset = off;
    set_dirty_all();
}

void Vt::scroll_view(int delta) {
    set_view_offset(_view_offset + delta);
}

void Vt::set_scrollback_limit(size_t n) {
    if(n < 1) n = 1;
    _max_scrollback = n;
    if(_scrollback.size() <= n) return;
    // 丢掉的是最老的行(队首)。正在往回看的话,视口锚的是「距底多少行」,
    // 丢了多少就把它减多少,视图才不会跳。
    const size_t dropped = _scrollback.size() - n;
    while(_scrollback.size() > n) _scrollback.pop_front();
    if(_view_offset > 0) {
        if(_view_offset > (int)dropped) _view_offset -= (int)dropped;
        else _view_offset = 0;
    }
    trim_images();  // 被丢掉的老行里若锚着图,一起扔
    set_dirty_all();
}

const Cell *Vt::display_row(int y) const {
    if(y < 0 || y >= _rows || _cols <= 0) return nullptr;
    if(_grid.empty()) return nullptr;

    if(_view_offset > 0) {
        const int total = (int)_scrollback.size() + _rows;
        const int start = total - _rows - _view_offset;
        const int idx = start + y;
        if(idx < 0) return nullptr;
        if(idx < (int)_scrollback.size()) return _scrollback[(size_t)idx].data();
        return _grid.data() + (size_t)(idx - (int)_scrollback.size()) * _cols;
    }
    return _grid.data() + (size_t)y * _cols;
}

std::string Vt::row_text(int y) const {
    const Cell *row = display_row(y);
    if(!row) return std::string();
    std::string s;
    for(int x = 0; x < _cols; ++x) {
        if(row[x].attr & kAttrWideTail) continue;
        append_utf8(s, row[x].ch ? row[x].ch : ' ');
    }
    while(!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

bool Vt::row_dirty(int y) const {
    if(_dirty_all) return true;
    if(y < 0 || y >= _rows) return false;
    return (size_t)y < _dirty.size() && _dirty[(size_t)y] != 0;
}

void Vt::clear_dirty() {
    _dirty_all = false;
    std::fill(_dirty.begin(), _dirty.end(), 0);
}

bool Vt::take_reply(std::string &out) {
    if(_reply.empty()) return false;
    out += _reply;
    _reply.clear();
    return true;
}

// ---------------------------------------------------------------- sixel

// 屏幕第 y 行的绝对行号。和 display_row 用同一套索引算法,只是把「相对行号」换成
// 「自程序启动以来第几行」—— 图像锚在绝对行上,滚动/回看时才能跟着动。
int Vt::abs_line_of_screen_row(int y) const {
    if(_view_offset > 0) {
        const int total = (int)_scrollback.size() + _rows;
        const int start = total - _rows - _view_offset;
        const int idx = start + y;
        if(idx < (int)_scrollback.size())
            return (int)(_abs_top - ((long)_scrollback.size() - idx));
        return (int)(_abs_top + (idx - (int)_scrollback.size()));
    }
    return (int)(_abs_top + y);
}

void Vt::clear_images() { _images.clear(); }

void Vt::erase_images_in(int r0, int c0, int r1, int c1) {
    if(_images.empty() || r0 > r1 || c0 > c1) return;
    const int cw = _cell_w > 0 ? _cell_w : 1;
    const int ch = _cell_h > 0 ? _cell_h : 1;
    const int top_abs = abs_line_of_screen_row(0);
    for(auto it = _images.begin(); it != _images.end();) {
        // 擦主屏不会动备用屏上的图(反之亦然),两张屏的格子本来就是分开的。
        if(it->alt != _alt_active) {
            ++it;
            continue;
        }
        const int xc0 = it->cell_x;
        const int xc1 = xc0 + (it->w + cw - 1) / cw - 1;
        const int yc0 = it->alt ? it->abs_line : it->abs_line - top_abs;
        const int yc1 = yc0 + (it->h + ch - 1) / ch - 1;
        const int ic0 = std::max(c0, xc0), ic1 = std::min(c1, xc1);
        const int ir0 = std::max(r0, yc0), ir1 = std::min(r1, yc1);
        if(ic0 > ic1 || ir0 > ir1) {
            ++it;
            continue;
        }

        // 整张图都被盖住就直接丢;只盖住一部分时,把被盖格子的像素涂透明 ——
        // 相当于 fbterm 只释放那一格的 pixmap,图的其他部分还留着。之前是「碰到
        // 一点就整张丢」,所以图下面那行提示符一写,整张图就变黑了。
        if(ic0 <= xc0 && ic1 >= xc1 && ir0 <= yc0 && ir1 >= yc1) {
            mark_dirty_range(yc0, yc1);
            it = _images.erase(it);
            continue;
        }
        const int px0 = (ic0 - xc0) * cw;
        const int px1 = std::min(it->w, (ic1 - xc0 + 1) * cw);
        const int py0 = (ir0 - yc0) * ch;
        const int py1 = std::min(it->h, (ir1 - yc0 + 1) * ch);
        for(int py = py0; py < py1; ++py) {
            uint32_t *row = &it->argb[(size_t)py * it->w];
            for(int px = px0; px < px1; ++px) row[px] = 0;
        }
        mark_dirty_range(ir0, ir1);
        ++it;
    }
}

// 收尾一条 DCS:是 sixel 就解码、以当前光标为锚入库。全屏 TUI(yazi 等)跑在
// 备用屏上,图也得画 —— 备用屏没有回看,锚点直接用屏幕行号。
void Vt::finish_dcs() {
    const bool was_sixel = _dcs_is_sixel;
    _dcs_is_sixel = false;
    if(!was_sixel || _dcs_buf.empty()) {
        _dcs_buf.clear();
        return;
    }

    SixelBitmap bmp;
    // 0 号色 = 当前 SGR 前景色。和 fbterm 一样只在真彩前景时生效:调色板色(含默认
    // 的 7 号)传 0,sixel 那边就保持 VT340 的黑色。
    const uint32_t fg = is_palette(_attrs.fg) ? 0u : true_color(_attrs.fg);
    if(sixel_decode(_dcs_buf.data(), _dcs_buf.size(), kSixelMaxPixels, fg, bmp)) {
        const int lines = bmp.h > 0 ? (bmp.h + _cell_h - 1) / _cell_h : 0;

        // 和 fbterm 一样先为整张图腾出空间再落图:不足就在滚动区里上滚,
        // 光标跟着抬。这样图不会被屏底截掉,也不会画完再把整屏滚走一行
        // (全屏 TUI 的布局会因此错位)。
        const int room = (_scroll_bottom + 1) - _cy;
        if(lines > room) {
            int need = lines - room;
            const int region = _scroll_bottom - _scroll_top + 1;
            if(need > region) need = region;
            scroll_up(need);
            if(_cy >= _scroll_top + need) _cy -= need;
            else _cy = _scroll_top;
        }

        SixelImage img;
        img.cell_x = _cx;
        img.abs_line = _alt_active ? _cy : abs_line_of_screen_row(_cy);
        img.alt = _alt_active;
        img.w = bmp.w;
        img.h = bmp.h;
        img.argb = std::move(bmp.argb);
        push_image(std::move(img));

        // 光标落到图下面一行;夹在屏内,和 fbterm 的 move_cursor(0, new_y) 一致。
        int ny = _cy + lines;
        if(ny > _rows - 1) ny = _rows - 1;
        _cx = 0;
        _cy = ny;
        _wrap_pending = false;
    }
    _dcs_buf.clear();
}

void Vt::push_image(SixelImage &&img) {
    // 图像压住的行(按 cell_h 折算占几行)标脏,让它这一轮就画出来。
    const int span = (img.h + _cell_h - 1) / _cell_h;
    const bool alt = img.alt;
    const int first = alt ? img.abs_line : img.abs_line - abs_line_of_screen_row(0);
    _images.push_back(std::move(img));
    if(first + span > 0 && first < _rows)
        mark_dirty_range(first, first + span - 1);
    trim_images();
}

// 丢掉锚点已经跌出保留范围的老图(滚进回看又被丢出去的那些),再按总字节上限
// 从最老的开始淘汰。
void Vt::trim_images() {
    const long oldest = _abs_top - (long)_scrollback.size();
    for(auto it = _images.begin(); it != _images.end();) {
        if(it->alt) {
            ++it;
            continue;
        }
        const int span = it->h > 0 ? (it->h + _cell_h - 1) / _cell_h : 0;
        if((long)it->abs_line + span <= oldest) it = _images.erase(it);
        else ++it;
    }
    size_t total = 0;
    for(const SixelImage &im : _images) total += im.argb.size() * sizeof(uint32_t);
    while(total > kImageBytesCap && !_images.empty()) {
        total -= _images.front().argb.size() * sizeof(uint32_t);
        _images.erase(_images.begin());
    }
}

// ---------------------------------------------------------------- 打印

void Vt::put_uint(uint32_t cp, int width) {
    if(width < 1 || _cols <= 0 || _rows <= 0) return;

    if(_wrap_pending) {
        _wrap_pending = false;
        _cx = 0;
        line_feed();
    }
    if(_cx + width > _cols) {
        if(!_autowrap) {
            _cx = _cols - width;
            if(_cx < 0) _cx = 0;
        } else {
            // 剩的地方不够了:先把行尾补空,再折行
            Cell *tail = &_grid[(size_t)_cy * _cols];
            for(int x = _cx; x < _cols; ++x) tail[x] = erase_cell();
            _cx = 0;
            line_feed();
        }
    }

    if(_insert_mode) insert_chars(width);

    if((size_t)_cy * _cols + _cx + width > _grid.size()) return;
    Cell *row = &_grid[(size_t)_cy * _cols];

    // 被覆盖的位置原先可能有双宽字,先把只剩一半的抹掉
    fix_wide(_cy, _cx);
    fix_wide(_cy, _cx + 1);

    // 文字写进图像占的格子时,图就没法再完整盖着了 —— 和 fbterm 一样把图丢掉
    // (yazi 这类 TUI 就是往预览区写空格来擦图)。
    erase_images_in(_cy, _cx, _cy, _cx + width - 1);

    row[_cx].ch = cp;
    row[_cx].fg = _attrs.fg;
    row[_cx].bg = _attrs.bg;
    row[_cx].attr = _attrs.attr;
    if(width == 2) {
        row[_cx + 1].ch = 0;
        row[_cx + 1].fg = _attrs.fg;
        row[_cx + 1].bg = _attrs.bg;
        row[_cx + 1].attr = (uint8_t)(_attrs.attr | kAttrWideTail);
    }

    _cx += width;
    if(_cx >= _cols) {
        _cx = _cols - 1;
        _wrap_pending = _autowrap;
    }
    mark_dirty(_cy);
}

void Vt::put_char(uint32_t cp) {
    if(cp < 0x80) {
        const uint8_t g = _gl_g1 ? _g1 : _g0;
        if(g == '0') cp = dec_graphics(cp);
    }
    int w = ::wcwidth((wchar_t)cp);
    // Nerd 图标只占一格:字体给它的 advance 可能是一格,但墨迹往往比一格宽,
    // 那部分交给渲染层借邻格,不能让它变成双宽字把后面的格子顶走。
    if(is_private_use(cp)) w = 1;
    if(w == 0) return;  // 组合字符:不占格,先丢掉(不做字形合成)
    if(w < 0) w = 1;
    if(w > 2) w = 1;
    put_uint(cp, w);
}

// ---------------------------------------------------------------- 擦除/插入/删除

void Vt::erase_display(int mode) {
    if(mode == 3) {
        _scrollback.clear();
        _view_offset = 0;
        _images.clear();  // 屏幕和回看一起清,锚在上面的图也留不住
        set_dirty_all();
        return;
    }
    switch(mode) {
    case 0:
        erase_images_in(_cy, _cx, _cy, _cols - 1);
        erase_images_in(_cy + 1, 0, _rows - 1, _cols - 1);
        clear_region(_cy, _cx, _rows - 1, _cols - 1);
        break;
    case 1:
        erase_images_in(0, 0, _cy - 1, _cols - 1);
        erase_images_in(_cy, 0, _cy, _cx);
        clear_region(0, 0, _cy, _cx);
        break;
    default:
        erase_images_in(0, 0, _rows - 1, _cols - 1);
        clear_region(0, 0, _rows - 1, _cols - 1);
        break;
    }
}

void Vt::erase_line(int mode) {
    switch(mode) {
    case 0:
        erase_images_in(_cy, _cx, _cy, _cols - 1);
        clear_region(_cy, _cx, _cy, _cols - 1);
        break;
    case 1:
        erase_images_in(_cy, 0, _cy, _cx);
        clear_region(_cy, 0, _cy, _cx);
        break;
    default:
        erase_images_in(_cy, 0, _cy, _cols - 1);
        clear_region(_cy, 0, _cy, _cols - 1);
        break;
    }
}

void Vt::erase_chars(int n) {
    if(_cx >= _cols) return;
    if(n < 1) n = 1;
    if(n > _cols - _cx) n = _cols - _cx;
    erase_images_in(_cy, _cx, _cy, _cx + n - 1);
    Cell *row = &_grid[(size_t)_cy * _cols];
    for(int x = _cx; x < _cx + n; ++x) row[x] = erase_cell();
    mark_dirty(_cy);
}

void Vt::insert_chars(int n) {
    if(_cx >= _cols) return;
    if(n < 1) n = 1;
    if(n > _cols - _cx) n = _cols - _cx;
    Cell *row = &_grid[(size_t)_cy * _cols];
    for(int x = _cols - 1; x >= _cx + n; --x) row[x] = row[x - n];
    for(int x = _cx; x < _cx + n; ++x) row[x] = erase_cell();
    mark_dirty(_cy);
}

void Vt::delete_chars(int n) {
    if(_cx >= _cols) return;
    if(n < 1) n = 1;
    if(n > _cols - _cx) n = _cols - _cx;
    Cell *row = &_grid[(size_t)_cy * _cols];
    for(int x = _cx; x < _cols - n; ++x) row[x] = row[x + n];
    for(int x = _cols - n; x < _cols; ++x) row[x] = erase_cell();
    mark_dirty(_cy);
}

void Vt::insert_lines(int n) {
    if(_cy < _scroll_top || _cy > _scroll_bottom) return;
    if(n < 1) n = 1;
    const int avail = _scroll_bottom - _cy + 1;
    if(n > avail) n = avail;
    for(int y = _scroll_bottom; y >= _cy + n; --y) {
        std::copy(_grid.begin() + (size_t)(y - n) * _cols,
                  _grid.begin() + (size_t)(y - n) * _cols + _cols,
                  _grid.begin() + (size_t)y * _cols);
    }
    for(int y = _cy; y < _cy + n; ++y) {
        Cell *row = &_grid[(size_t)y * _cols];
        for(int x = 0; x < _cols; ++x) row[x] = erase_cell();
    }
    mark_dirty_range(_cy, _scroll_bottom);
}

void Vt::delete_lines(int n) {
    if(_cy < _scroll_top || _cy > _scroll_bottom) return;
    if(n < 1) n = 1;
    const int avail = _scroll_bottom - _cy + 1;
    if(n > avail) n = avail;
    for(int y = _cy; y <= _scroll_bottom - n; ++y) {
        std::copy(_grid.begin() + (size_t)(y + n) * _cols,
                  _grid.begin() + (size_t)(y + n) * _cols + _cols,
                  _grid.begin() + (size_t)y * _cols);
    }
    for(int y = _scroll_bottom - n + 1; y <= _scroll_bottom; ++y) {
        Cell *row = &_grid[(size_t)y * _cols];
        for(int x = 0; x < _cols; ++x) row[x] = erase_cell();
    }
    mark_dirty_range(_cy, _scroll_bottom);
}

// ---------------------------------------------------------------- 复位

void Vt::reset_attrs() {
    _attrs = Attrs{};
}

void Vt::soft_reset() {
    reset_attrs();
    _origin_mode = false;
    _insert_mode = false;
    _newline_mode = false;
    _app_cursor_keys = false;
    _cursor_visible = true;
    _scroll_top = 0;
    _scroll_bottom = _rows - 1;
    _g0 = 'B';
    _g1 = 'B';
    _gl_g1 = false;
    _saved = SavedCursor{};
    _wrap_pending = false;
    clamp_cursor();
}

void Vt::hard_reset() {
    reset_attrs();
    _scroll_top = 0;
    _scroll_bottom = _rows - 1;
    _origin_mode = false;
    _insert_mode = false;
    _newline_mode = false;
    _autowrap = true;
    _cursor_visible = true;
    _app_cursor_keys = false;
    _bracketed_paste = false;
    _cx = _cy = 0;
    _wrap_pending = false;
    _g0 = 'B';
    _g1 = 'B';
    _gl_g1 = false;
    _saved = SavedCursor{};
    _saved_1049 = SavedCursor{};
    _scrollback.clear();
    _view_offset = 0;
    _alt_active = false;
    _abs_top = 0;      // 回看清空了,绝对行号从头算
    _images.clear();
    for(size_t i = 0; i < _grid.size(); ++i) _grid[i] = blank_cell();
    for(size_t i = 0; i < _alt_grid.size(); ++i) _alt_grid[i] = blank_cell();
    set_dirty_all();
}

// ---------------------------------------------------------------- 解析器

void Vt::reset_parser() {
    _params.clear();
    _param_have_digits = false;
    _csi_private = false;
    _csi_prefix = 0;
    _intermediates.clear();
}

void Vt::feed(const char *data, size_t len) {
    if(!data) return;
    for(size_t i = 0; i < len; ++i) handle_byte((uint8_t)data[i]);
}

void Vt::handle_byte(uint8_t b) {
    switch(_state) {
    case State::Ground:
        // 多字节序列的续字节可以落在 0x80-0x9f,那和 8 位 C1 控制码是同一个范围。
        // 正在收续字节时一律先当 UTF-8,否则汉字的最后一字节会被当成控制码吃掉。
        if(_utf8_need > 0) {
            handle_utf8(b);
            return;
        }
        if(b == 0x1b) { _state = State::Esc; return; }
        if(b < 0x20) { handle_c0(b); return; }
        if(b == 0x7f) return;  // DEL:xterm 也是吞掉
        if(b == 0x9b) { reset_parser(); _state = State::CsiEntry; return; }
        if(b == 0x9d) { _state = State::OscString; return; }
        if(b == 0x9f) { _state = State::ApcString; return; }
        if(b == 0x90) {
            reset_parser();
            _dcs_is_sixel = false;
            _dcs_buf.clear();
            _state = State::DcsEntry;
            return;
        }
        if(b >= 0x80 && b <= 0x9f) { handle_c0(b); return; }
        handle_utf8(b);
        return;

    case State::Esc:
        handle_esc(b);
        return;

    case State::CsiEntry:
        if(b >= '0' && b <= '9') {
            _params.push_back(b - '0');
            _param_have_digits = true;
            _state = State::CsiParam;
        } else if(b == ';') {
            _params.push_back(-1);
            _param_have_digits = false;
            _state = State::CsiParam;
        } else if(b == '?' || b == '>' || b == '<' || b == '=') {
            _csi_private = true;
            _csi_prefix = (char)b;
            _state = State::CsiParam;
        } else if(b >= 0x20 && b <= 0x2f) {
            _intermediates.push_back((char)b);
            _state = State::CsiIntermediate;
        } else if(b >= 0x40 && b <= 0x7e) {
            handle_csi(b);
            _state = State::Ground;
        } else if(b < 0x20) {
            handle_c0(b);  // CSI 里的执行字符:照办,序列继续
        } else {
            _state = State::CsiIgnore;
        }
        return;

    case State::CsiParam:
        if(b >= '0' && b <= '9') {
            if(_param_have_digits) {
                int &v = _params.back();
                if(v < 0) v = 0;
                if(v < 65535) v = v * 10 + (b - '0');
            } else {
                _params.push_back(b - '0');
                _param_have_digits = true;
            }
        } else if(b == ';') {
            if(!_param_have_digits) _params.push_back(-1);
            _param_have_digits = false;
        } else if(b >= 0x20 && b <= 0x2f) {
            _intermediates.push_back((char)b);
            _state = State::CsiIntermediate;
        } else if(b >= 0x40 && b <= 0x7e) {
            if(!_param_have_digits && !_params.empty()) _params.push_back(-1);
            handle_csi(b);
            _state = State::Ground;
        } else if(b < 0x20) {
            handle_c0(b);
        } else {
            _state = State::CsiIgnore;
        }
        return;

    case State::CsiIntermediate:
        if(b >= 0x20 && b <= 0x2f) {
            _intermediates.push_back((char)b);
        } else if(b >= 0x40 && b <= 0x7e) {
            if(!_param_have_digits && !_params.empty()) _params.push_back(-1);
            handle_csi(b);
            _state = State::Ground;
        } else if(b < 0x20) {
            handle_c0(b);
        } else {
            _state = State::CsiIgnore;
        }
        return;

    case State::CsiIgnore:
        if(b >= 0x40 && b <= 0x7e) _state = State::Ground;
        else if(b < 0x20) handle_c0(b);
        return;

    case State::OscString:
        if(b == 0x07) _state = State::Ground;
        else if(b == 0x1b) _state = State::OscEsc;
        return;

    case State::OscEsc:
        _state = (b == '\\') ? State::Ground : State::OscString;
        return;

    case State::ApcString:
        // APC 载荷是应用私有的(Kitty 图形探测就走这条),终端只要吞到 ST。
        if(b == 0x1b) _state = State::ApcEsc;
        else if(b == 0x9c || b == 0x07) _state = State::Ground;
        return;

    case State::ApcEsc:
        if(b == '\\') _state = State::Ground;
        else if(b != 0x1b) _state = State::ApcString;
        return;

    case State::DcsEntry:
        // ESC P/0x90 之后,收参数和中间字节,等 final 字节决定这是什么 DCS。
        if(b == 0x1b) { _state = State::DcsEsc; return; }
        if((b >= '0' && b <= '9') || b == ';' || (b >= 0x3c && b <= 0x3f)) return;
        if(b >= 0x20 && b <= 0x2f) {
            _intermediates.push_back((char)b);
            return;
        }
        if(b >= 0x40 && b <= 0x7e) {
            // sixel 是 DCS ... q(ST),没有中间字节;$q(DECRQSS)之类带中间字节,
            // 一律不当 sixel。
            if(b == 'q' && _intermediates.empty()) {
                _dcs_is_sixel = true;
                _dcs_buf.clear();
                _state = State::DcsData;
            } else {
                _state = State::DcsIgnore;
            }
            return;
        }
        if(b < 0x20) return;  // DCS 里的执行字符:忽略
        _state = State::DcsIgnore;
        return;

    case State::DcsData:
        // sixel 载荷:'q' 之后到 ST。
        if(b == 0x1b) { _state = State::DcsEsc; return; }
        if(b == 0x9c) {
            finish_dcs();
            _state = State::Ground;
            return;
        }
        if(_dcs_buf.size() < kDcsBufCap) _dcs_buf.push_back((char)b);
        return;

    case State::DcsIgnore:
        if(b == 0x1b) _state = State::DcsEsc;
        return;

    case State::DcsEsc:
        if(b == '\\') {
            if(_dcs_is_sixel) finish_dcs();
            _state = State::Ground;
        } else if(b != 0x1b) {
            // ESC 后面不是 ST:按原来那条路接着收(DcsData 收数据 / DcsIgnore 丢)
            _state = _dcs_is_sixel ? State::DcsData : State::DcsIgnore;
        }
        return;

    case State::Charset:
        if(_charset_slot == '(') _g0 = b;
        else if(_charset_slot == ')') _g1 = b;
        // '*' '+' 是 G2/G3,用不上,直接丢掉
        _state = State::Ground;
        return;
    }
}

void Vt::handle_c0(uint8_t b) {
    switch(b) {
    case 0x07: break;  // BEL:没有蜂鸣器,静音
    case 0x08:
        if(_cx > 0) --_cx;
        _wrap_pending = false;
        break;
    case 0x09: tab(); break;
    case 0x0a:
    case 0x0b:
    case 0x0c: line_feed(); break;
    case 0x0d: carriage_return(); break;
    case 0x0e: _gl_g1 = true; break;   // SO
    case 0x0f: _gl_g1 = false; break;  // SI
    case 0x84: line_feed(); break;     // IND
    case 0x85:                         // NEL
        carriage_return();
        line_feed();
        break;
    case 0x88: break;                  // HTS:制表位我们只用每 8 列,忽略
    case 0x8d: reverse_index(); break; // RI
    default: break;
    }
}

void Vt::handle_utf8(uint8_t b) {
    if(_utf8_need > 0 && (b & 0xc0) == 0x80) {
        _utf8_acc = (_utf8_acc << 6) | (uint32_t)(b & 0x3f);
        if(++_utf8_have >= _utf8_need) {
            const uint32_t cp = _utf8_acc;
            _utf8_need = 0;
            _utf8_have = 0;
            put_char(cp);
        }
        return;
    }
    _utf8_need = 0;
    _utf8_have = 0;
    if(b < 0x80) {
        put_char(b);
    } else if(b < 0xc2) {
        // 0x80-0xbf 孤立续字节、0xc0/0xc1 过长编码:丢掉
    } else if(b < 0xe0) {
        _utf8_acc = b & 0x1fu;
        _utf8_need = 1;
    } else if(b < 0xf0) {
        _utf8_acc = b & 0x0fu;
        _utf8_need = 2;
    } else if(b < 0xf5) {
        _utf8_acc = b & 0x07u;
        _utf8_need = 3;
    }
}

void Vt::handle_esc(uint8_t b) {
    _state = State::Ground;
    switch(b) {
    case '[':
        reset_parser();
        _state = State::CsiEntry;
        return;
    case ']':
        _state = State::OscString;
        return;
    case '_':
        _state = State::ApcString;  // 应用私有串(Kitty 图形探测等),整段吞掉
        return;
    case 'P':
        reset_parser();
        _dcs_is_sixel = false;
        _dcs_buf.clear();
        _state = State::DcsEntry;
        return;
    case '7': save_cursor(); return;
    case '8': restore_cursor(); return;
    case 'D': line_feed(); return;                       // IND
    case 'E':                                            // NEL
        carriage_return();
        line_feed();
        return;
    case 'M': reverse_index(); return;                   // RI
    case 'c': hard_reset(); return;                      // RIS
    case '(':
    case ')':
    case '*':
        _charset_slot = b;
        _state = State::Charset;
        return;
    default:
        // '=' '>' (小键盘模式) 'H' (HTS) '\\' (ST) 'N'/'O' (SS2/SS3) 都直接丢
        return;
    }
}

int Vt::arg(size_t i, int def) const {
    if(i < _params.size() && _params[i] >= 0) return _params[i];
    return def;
}

void Vt::handle_csi(uint8_t final) {
    const bool priv = _csi_private;

    // 参数值 0 在「位置/个数」类参数里按 1 算
    auto n1 = [&](size_t i) {
        int v = arg(i, 1);
        return v > 0 ? v : 1;
    };

    switch(final) {
    case '@': insert_chars(n1(0)); break;

    case 'A': {
        _wrap_pending = false;
        const int lim = (_cy >= _scroll_top) ? _scroll_top : 0;
        _cy -= n1(0);
        if(_cy < lim) _cy = lim;
        break;
    }
    case 'B': {
        _wrap_pending = false;
        const int lim = (_cy <= _scroll_bottom) ? _scroll_bottom : _rows - 1;
        _cy += n1(0);
        if(_cy > lim) _cy = lim;
        break;
    }
    case 'C':
        _wrap_pending = false;
        _cx += n1(0);
        if(_cx > _cols - 1) _cx = _cols - 1;
        break;
    case 'D':
        _wrap_pending = false;
        _cx -= n1(0);
        if(_cx < 0) _cx = 0;
        break;
    case 'E':
        _wrap_pending = false;
        _cx = 0;
        _cy += n1(0);
        if(_cy > _rows - 1) _cy = _rows - 1;
        break;
    case 'F':
        _wrap_pending = false;
        _cx = 0;
        _cy -= n1(0);
        if(_cy < 0) _cy = 0;
        break;
    case 'G':
    case '`':
        _wrap_pending = false;
        _cx = n1(0) - 1;
        clamp_cursor();
        break;
    case 'd': {
        _wrap_pending = false;
        int row = n1(0) - 1;
        if(_origin_mode) row += _scroll_top;
        _cy = row;
        clamp_cursor();
        break;
    }
    case 'H':
    case 'f': {
        _wrap_pending = false;
        int row = n1(0) - 1;
        int col = arg(1, 1);
        if(col < 1) col = 1;
        col -= 1;
        if(_origin_mode) {
            row += _scroll_top;
            if(row > _scroll_bottom) row = _scroll_bottom;
        }
        _cx = col;
        _cy = row;
        clamp_cursor();
        break;
    }
    case 'J': erase_display(arg(0, 0)); break;
    case 'K': erase_line(arg(0, 0)); break;
    case 'L': insert_lines(n1(0)); break;
    case 'M': delete_lines(n1(0)); break;
    case 'P': delete_chars(n1(0)); break;
    case 'X': erase_chars(n1(0)); break;
    case 'S': scroll_up(n1(0)); break;
    case 'T': scroll_down(n1(0)); break;

    case 'm': handle_sgr(); break;

    case 'n': {
        const int p = arg(0, 0);
        if(p == 5) {
            _reply += "\033[0n";
        } else if(p == 6) {
            int row = _cy + 1;
            const int col = _cx + 1;
            if(_origin_mode) row = _cy - _scroll_top + 1;
            _reply += "\033[";
            _reply += std::to_string(row);
            _reply += ";";
            _reply += std::to_string(col);
            _reply += "R";
        }
        break;
    }

    case 'c':
        if(priv) {
            if(_csi_prefix == '>') _reply += "\033[>0;0;0c";
        } else {
            // DA1:62 = VT220 级,4 = 支持 sixel。yazi / chafa 这类客户端靠这个
            // 属性判断能不能直接发图。
            _reply += "\033[?62;4c";
        }
        break;

    case 'r': {
        int top = n1(0) - 1;
        int bot = arg(1, _rows) - 1;
        if(top < 0) top = 0;
        if(bot > _rows - 1) bot = _rows - 1;
        if(top < bot) {
            _scroll_top = top;
            _scroll_bottom = bot;
        }
        set_cursor(0, _origin_mode ? _scroll_top : 0);
        break;
    }

    case 's': save_cursor(); break;
    case 'u': restore_cursor(); break;

    case 'h':
    case 'l': {
        const bool on = (final == 'h');
        const size_t np = _params.empty() ? 1 : _params.size();
        for(size_t i = 0; i < np; ++i) {
            const int p = arg(i, 0);
            if(!priv) {
                switch(p) {
                case 4: _insert_mode = on; break;
                case 20: _newline_mode = on; break;
                default: break;
                }
                continue;
            }
            switch(p) {
            case 1: _app_cursor_keys = on; break;
            case 6:
                _origin_mode = on;
                set_cursor(0, on ? _scroll_top : 0);
                break;
            case 7: _autowrap = on; break;
            case 12: break;  // 光标闪烁:我们有光标就够了
            case 25: _cursor_visible = on; break;
            case 1000:
            case 1002:
            case 1003:
            case 1005:
            case 1006:
            case 1015:
                break;  // 鼠标上报:没有鼠标,记都不记
            case 1047:
                if(on) alt_enter(false, true);
                else alt_exit(false);
                break;
            case 1048:
                if(on) save_cursor_to(_saved_1049);
                else restore_cursor_from(_saved_1049);
                break;
            case 1049:
                if(on) alt_enter(true, true);
                else alt_exit(true);
                break;
            case 2004: _bracketed_paste = on; break;
            default: break;
            }
        }
        break;
    }

    case 'p':
        // CSI ! p = DECSTR(软复位)。'!' 落在 _intermediates 里。
        if(_intermediates == "!") soft_reset();
        break;

    case 'q':
        // CSI (空格) q = DECSCUSR,应用要求改光标形状(块/下划线/竖线)。**故意忽略**:
        // 形状是用户在设置界面里选的,不该被 vim/tmux 随手指使唤。
        break;

    case 't': {
        // XTWINOPS:14 t 报文本区像素尺寸,16 t 报单格像素尺寸。yazi 这类带图预览的
        // 客户端用后者算缩放,不回它就会当成不支持图。别的窗口操作没有窗口,忽略。
        if(priv || !_intermediates.empty()) break;
        const int p = arg(0, 0);
        if(p == 14) {
            _reply += "\033[4;";
            _reply += std::to_string(_rows * _cell_h);
            _reply += ";";
            _reply += std::to_string(_cols * _cell_w);
            _reply += "t";
        } else if(p == 16) {
            _reply += "\033[6;";
            _reply += std::to_string(_cell_h);
            _reply += ";";
            _reply += std::to_string(_cell_w);
            _reply += "t";
        }
        break;
    }

    default:
        break;
    }
}

void Vt::handle_sgr() {
    if(_params.empty()) {
        reset_attrs();
        return;
    }
    for(size_t i = 0; i < _params.size(); ++i) {
        int p = _params[i];
        if(p < 0) p = 0;
        switch(p) {
        case 0: reset_attrs(); break;
        case 1: _attrs.attr |= kAttrBold; break;
        case 2: _attrs.attr |= kAttrDim; break;
        case 3: _attrs.attr |= kAttrItalic; break;
        case 4: _attrs.attr |= kAttrUnderline; break;
        case 5:
        case 6: _attrs.attr |= kAttrBlink; break;
        case 7: _attrs.attr |= kAttrInverse; break;
        case 9: _attrs.attr |= kAttrStrike; break;
        case 21:
        case 22: _attrs.attr &= (uint8_t)~(kAttrBold | kAttrDim); break;
        case 23: _attrs.attr &= (uint8_t)~kAttrItalic; break;
        case 24: _attrs.attr &= (uint8_t)~kAttrUnderline; break;
        case 25: _attrs.attr &= (uint8_t)~kAttrBlink; break;
        case 27: _attrs.attr &= (uint8_t)~kAttrInverse; break;
        case 29: _attrs.attr &= (uint8_t)~kAttrStrike; break;
        case 39: _attrs.fg = palette_color(7); break;
        case 49: _attrs.bg = palette_color(0); break;

        case 38:
        case 48: {
            uint32_t col = 0;
            bool ok = false;
            if(i + 1 < _params.size() && _params[i + 1] == 5 && i + 2 < _params.size()) {
                int idx = _params[i + 2];
                if(idx < 0) idx = 0;
                col = palette_color((uint8_t)(idx & 0xff));
                i += 2;
                ok = true;
            } else if(i + 1 < _params.size() && _params[i + 1] == 2 && i + 4 < _params.size()) {
                auto clamp8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
                col = true_color(((uint32_t)clamp8(_params[i + 2]) << 16) |
                                 ((uint32_t)clamp8(_params[i + 3]) << 8) |
                                 (uint32_t)clamp8(_params[i + 4]));
                i += 4;
                ok = true;
            }
            if(ok) {
                if(p == 38) _attrs.fg = col;
                else _attrs.bg = col;
            } else {
                // 认不出来的扩展色:后面的参数都不是 SGR 参数,别当普通码接着解释
                return;
            }
            break;
        }

        default:
            if(p >= 30 && p <= 37) _attrs.fg = palette_color((uint8_t)(p - 30));
            else if(p >= 40 && p <= 47) _attrs.bg = palette_color((uint8_t)(p - 40));
            else if(p >= 90 && p <= 97) _attrs.fg = palette_color((uint8_t)(p - 90 + 8));
            else if(p >= 100 && p <= 107) _attrs.bg = palette_color((uint8_t)(p - 100 + 8));
            break;
        }
    }
}

}  // namespace term
