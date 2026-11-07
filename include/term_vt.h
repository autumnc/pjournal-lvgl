#pragma once

// 终端仿真核心:网格屏 + VT 解析器。**这个文件不依赖 LVGL**,
// 所以 tests/vt_test.cpp 能在宿主机上直接编一份来跑,不用碰 framebuffer。
//
// 双宽字符靠 wcwidth(),而 wcwidth 看的是 LC_CTYPE —— 调用方必须先
// setlocale(LC_CTYPE, "C.utf8")(见 main.cpp),否则汉字一律按 1 格算。

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace term {

// 调色板色和真彩的区分位:高字节是 0x01 表示低字节为调色板索引(0..255),
// 否则整个 uint32_t 就是 0x00RRGGBB。
constexpr uint32_t kPaletteBit = 0x01000000u;

constexpr uint32_t palette_color(uint8_t idx) { return kPaletteBit | idx; }
constexpr bool is_palette(uint32_t c) { return (c & 0xff000000u) == kPaletteBit; }
constexpr uint8_t palette_index(uint32_t c) { return (uint8_t)(c & 0xffu); }
constexpr uint32_t true_color(uint32_t rgb) { return rgb & 0x00ffffffu; }

// Nerd Font 图标所在的 Unicode 私用区。和 fbterm 一样,这些码位在网格里永远只占
// 一格(wcwidth 说什么都不算),字形比一格宽的部分由渲染层借右边那格来画。
constexpr bool is_private_use(uint32_t cp) {
    return (cp >= 0xe000u && cp <= 0xf8ffu) || (cp >= 0xf0000u && cp <= 0xffffdu) ||
           (cp >= 0x100000u && cp <= 0x10fffdu);
}

enum : uint8_t {
    kAttrBold = 1u << 0,
    kAttrDim = 1u << 1,
    kAttrUnderline = 1u << 2,
    kAttrBlink = 1u << 3,
    kAttrInverse = 1u << 4,
    kAttrWideTail = 1u << 5,  // 宽字符占的第二格:画的时候跳过这个格
    kAttrItalic = 1u << 6,    // SGR 3。渲染时挑斜体字面(term_ui.cpp 的 font_for)
    kAttrStrike = 1u << 7,    // SGR 9。渲染时画一条过格中点的删除线
};

// 一格 16 字节。40x16 的可见屏 + 1000 行回看一共 1MB 出头,400MB 的板子放得下。
struct Cell {
    uint32_t ch = ' ';
    uint32_t fg = palette_color(7);
    uint32_t bg = palette_color(0);
    uint8_t attr = 0;
};

struct Attrs {
    uint32_t fg = palette_color(7);
    uint32_t bg = palette_color(0);
    uint8_t attr = 0;
};

// 一张解码好的 sixel 图(见 src/sixel.cpp 的 sixel_decode)。锚点是「格子坐标」,
// 像素换算交给渲染层(它才知道格宽格高)—— 这样图才能跟着内容滚,列号用绝对行号。
struct SixelImage {
    int cell_x = 0;              // 起始格列(光标当时的列)
    int abs_line = 0;            // 主屏:起始绝对行;备用屏:起始屏幕行(备用屏没有回看)
    bool alt = false;            // 画在备用屏上(全屏 TUI,比如 yazi)
    int w = 0, h = 0;            // 像素宽高
    std::vector<uint32_t> argb;  // 0xAARRGGBB,非预乘,未上色像素 alpha=0
};

class Vt {
public:
    Vt(int cols, int rows);

    void resize(int cols, int rows);
    int cols() const { return _cols; }
    int rows() const { return _rows; }

    void feed(const char *data, size_t len);

    // ---- 渲染侧 ----
    // 屏幕第 y 行(0 是顶行)对应的那一行格子。回看待机会算进 view_offset,
    // 所以渲染只认这一个接口。越界或者宽度为 0 时返回 nullptr。
    const Cell *display_row(int y) const;
    // 屏幕第 y 行内容的 UTF-8 文本(尾巴空格去掉),测试和调试用。
    std::string row_text(int y) const;

    bool row_dirty(int y) const;
    void clear_dirty();

    int cursor_x() const { return _cx; }
    int cursor_y() const { return _cy; }
    bool cursor_visible() const { return _cursor_visible && !viewing_scrollback(); }
    bool alt_screen() const { return _alt_active; }

    // ---- 回看 ----
    int scrollback_lines() const { return (int)_scrollback.size(); }
    int view_offset() const { return _view_offset; }
    void set_view_offset(int off);
    void scroll_view(int delta);
    bool viewing_scrollback() const { return _view_offset > 0; }
    void scroll_to_bottom() { _view_offset = 0; }
    // 设置界面里调。调小会立刻把多出来的行丢掉(内存跟着降),不是等下次满了才生效。
    void set_scrollback_limit(size_t n);

    // ---- 给 shell 的应答 ----
    // DSR/CPR/DA 的回复字节。主循环每圈取一次写回 pty。
    bool take_reply(std::string &out);

    // 键映射要用:应用把方向键切成了 SS3 形式(CSI ? 1 h)
    bool app_cursor_keys() const { return _app_cursor_keys; }
    bool bracketed_paste() const { return _bracketed_paste; }

    // ---- sixel 图像 ----
    // 已解码的图,按到达顺序。渲染层遍历它,按锚点算屏幕像素位置。
    const std::vector<SixelImage> &images() const { return _images; }
    // 屏幕第 y 行对应的**绝对行号**(随滚动/回看变化)。渲染层拿它对齐图像:
    // 主屏图像屏幕行 = img.abs_line - abs_line_of_screen_row(0);
    // 备用屏图像没有回看,屏幕行就是 img.abs_line。
    int abs_line_of_screen_row(int y) const;
    // 渲染层在 create()/set_fonts() 里告诉我们格子像素尺寸 —— 淘汰旧图时要用
    // (图像高度按 cell_h 折算成占几行)。
    void set_cell_size(int w, int h) {
        if(w > 0) _cell_w = w;
        if(h > 0) _cell_h = h;
    }
    void clear_images();

private:
    using Row = std::vector<Cell>;

    // 解析器状态
    enum class State {
        Ground,
        Esc,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        OscString,
        OscEsc,
        DcsEntry,   // ESC P/0x90 后收参数,等 final 字节
        DcsData,    // sixel 载荷('q' 之后到 ST)
        DcsIgnore,  // 不是 sixel 的 DCS,丢掉
        DcsEsc,
        ApcString,  // ESC _/0x9f(应用私有,比如 Kitty 图形探测),收到 ST 为止
        ApcEsc,
        Charset,
    };

    struct SavedCursor {
        int x = 0;
        int y = 0;
        Attrs attrs;
        bool origin_mode = false;
        bool valid = false;
    };

    // ---- 字节入口 ----
    void handle_byte(uint8_t b);
    void handle_c0(uint8_t b);
    void handle_utf8(uint8_t b);
    void reset_parser();

    // ---- sixel ----
    void finish_dcs();                     // ST:收尾(是 sixel 就解码入库)
    void push_image(SixelImage &&img);     // 入库 + 超上限淘汰 + 标脏
    void trim_images();                    // 丢掉锚点已跌出保留范围的老图
    // 擦掉一片单元格时,把和这片区域相交的 sixel 图一起丢掉(和 fbterm 把图裁进
    // 格子、擦格即删图的行为一致),否则 clear 之后图还浮在空屏上。
    void erase_images_in(int r0, int c0, int r1, int c1);

    // ---- 网格 ----
    Row make_row() const;
    void fill_row(Row &r, int from) const;
    void clear_region(int y0, int x0, int y1, int x1);
    void scroll_up(int n);
    void scroll_down(int n);
    void reverse_index();
    Cell blank_cell() const;
    Cell erase_cell() const;
    // 写字符前把被切掉一半的双宽字擦干净(否则屏上会留半个汉字)
    void fix_wide(int y, int x);

    // ---- 光标 ----
    void set_cursor(int x, int y);
    void clamp_cursor();
    void line_feed();
    void carriage_return();
    void tab();
    void save_cursor();
    void restore_cursor();
    void save_cursor_to(SavedCursor &s);
    void restore_cursor_from(const SavedCursor &s);

    // ---- 备用屏 ----
    void alt_enter(bool save_cur, bool clear);
    void alt_exit(bool restore_cur);
    void alt_clear();

    // ---- 打印 ----
    void put_uint(uint32_t cp, int width);
    void put_char(uint32_t cp);
    void mark_dirty(int y);
    void mark_dirty_range(int y0, int y1);

    // ---- 转义序列 ----
    void handle_esc(uint8_t b);
    void handle_csi(uint8_t final);
    void handle_sgr();
    int arg(size_t i, int def) const;
    void erase_display(int mode);
    void erase_line(int mode);
    void erase_chars(int n);
    void insert_chars(int n);
    void delete_chars(int n);
    void insert_lines(int n);
    void delete_lines(int n);

    void reset_attrs();
    void soft_reset();
    void hard_reset();

    void set_dirty_all();

    // ---- 状态 ----
    int _cols = 80;
    int _rows = 24;
    Row _grid;
    Row _alt_grid;
    bool _alt_active = false;

    std::deque<Row> _scrollback;
    size_t _max_scrollback = 1000;
    int _view_offset = 0;

    int _cx = 0;
    int _cy = 0;
    Attrs _attrs;

    // 自动折行是**延迟**的:xterm 在写到最后一列时不立刻换行,而是把光标停在
    // 最后一列并立一个「待折行」旗子。下一个可打印字符来了才真正换行。readline
    // 和 vim 的行为都依赖这个细节(否则行尾那个字符会被立刻顶到下一行)。
    bool _wrap_pending = false;
    bool _autowrap = true;
    bool _origin_mode = false;
    bool _insert_mode = false;
    bool _newline_mode = false;
    bool _cursor_visible = true;
    bool _app_cursor_keys = false;
    bool _bracketed_paste = false;

    // 滚动区域(0 基,含两端)。默认整屏。
    int _scroll_top = 0;
    int _scroll_bottom = 23;

    SavedCursor _saved;       // ESC 7 / ESC 8 / CSI s / CSI u
    SavedCursor _saved_1049;  // CSI ? 1049 h/l 自带的保存位

    // 解析器
    State _state = State::Ground;
    std::vector<int> _params;      // -1 表示「这个位置是空的」(CSI 1;;3 m)
    bool _param_have_digits = false;
    bool _csi_private = false;   // '?' '>' '<' '='
    char _csi_prefix = 0;
    std::string _intermediates;

    // 字符集:ESC ( 0 切 G0 到 DEC 制图符。ncurses 画框线靠它。
    uint8_t _g0 = 'B';
    uint8_t _g1 = 'B';
    bool _gl_g1 = false;         // SO/SI 选的是 G1 还是 G0
    uint8_t _charset_slot = 0;   // ESC 后面那个 ( 或 )

    // UTF-8 解码
    uint32_t _utf8_acc = 0;
    int _utf8_need = 0;
    int _utf8_have = 0;

    std::string _reply;
    std::vector<uint8_t> _dirty;
    bool _dirty_all = true;

    // ---- sixel ----
    long _abs_top = 0;              // _grid[0] 的绝对行号,scroll_up 往回看压行时 ++
    std::vector<SixelImage> _images;
    std::string _dcs_buf;           // 正在收的 DCS 载荷('q' 之后的部分)
    bool _dcs_is_sixel = false;
    int _cell_w = 1, _cell_h = 1;   // 渲染层告知的格子像素尺寸(淘汰旧图要用)
};

}  // namespace term
