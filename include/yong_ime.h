#pragma once

#include <cstdint>
#include <string>
#include <vector>

// yong 输入法后端。yong 的引擎(mb/face.c 里那个 EXTRA_IM EIM)静态编进本程序,这边只做
// 驱动侧的事:AppKey 翻成 yong 的按键码喂给 EIM.DoInput(),跟 IMR_* 结果走,把 GBK 的
// 候选/上屏串转成 UTF-8 交给现有候选条。碰引擎的活全在 src/yong_bridge.c,这里只跟
// 那层的 yb_* 接口打交道。
//
// 编译时 /home/ywz/yong 不在(PJOURNAL_HAS_YONG 没定义)→ 整个类退化成「没有输入法」,
// handle_key 恒返回 false,不崩。
//
// 中英模式复用内置输入法的英文查词(yong 的 common/english.c 是 glib 绑定的,编不进来),
// 符号和全角也走内置那套(设备上没有 yong 的 bd.txt 标点表,而且全角在 yong 里本来就是
// 驱动侧的活)。
class YongIme {
public:
    using WidthFn = int (*)(const char *text);

    bool begin();
    void pump() {}

    bool connected() const { return _ready; }

    bool active() const { return _active; }
    bool composing() const;
    void toggle() { set_active(!_active); }
    void set_active(bool on);
    bool handle_key(int key, std::string &out);

    std::string composition() const;
    const std::vector<std::string> &candidates() const { return _cands; }
    // 当前方案的显示名(永码/五笔/两分…,yong.ini 里的 name=),给状态栏用
    std::string schema_name() const;
    int current_page() const { return _page; }
    int total_pages() const { return _page_count; }
    int total_candidates() const { return _total; }
    int page_size() const { return _cands.empty() ? 1 : (int)_cands.size(); }
    void set_page_size(int n);
    int highlight_index() const { return _highlight; }

    bool fullwidth() const { return _fullwidth; }
    bool trad() const { return _trad; }
    bool english() const { return _english; }
    void toggle_fullwidth();
    void toggle_trad();
    void toggle_english();

    // 分页权在 yong 引擎手里,每页几个由 EIM.CandWordMax 定。横排按「这一页的候选
    // 按当前像素宽度放得下几个」算、竖排按行数算,和引擎现在的页宽不一致就让引擎按
    // 新页宽重排——放不下的候选整项挪到下一页、编号从 1 重新计,和内置输入法一致。
    void set_display_width(int px);
    void set_width_fn(WidthFn fn);

    // Ctrl+Shift:在 yong.ini 的 [IM] 列表里轮转下一种输入法(跳过非 libmb.so 的)
    bool switch_schema();

private:
    void reset_engine();
    void refresh_candidates();                  // 从引擎捞当前页(含 _cands / _page / _total)
    int fit_count() const;                      // 当前页按 _budget / _rows 放得下几个
    void sync_page_fit();                       // 量出来的数 ≠ EIM.CandWordMax 就重排
    void append_string_get(std::string &out);   // EIM.StringGet → out(过 s2t)
    void drain_pending(std::string &out);       // SendString 攒的串 → out
    int to_yong_key(int key) const;
    // IMR_NEXT:引擎不管的键由驱动侧兜底(翻页/选字/上屏/标点)
    bool next_key(int yk, std::string &out);
    bool punct_key(int yk, std::string &out);
    bool english_key(int key, std::string &out);
    void set_english_proxy(bool on);

    bool _ready = false;
    bool _active = false;
    bool _fullwidth = false;
    bool _trad = false;
    bool _english = false;

    std::vector<std::string> _cands;    // 当前页候选(UTF-8)
    int _highlight = -1;
    int _page = 1;
    int _page_count = 1;
    int _total = 0;
    int _budget = 0;
    int _rows = 0;    // 竖排:一页几行(set_page_size 给的)
    WidthFn _width_fn = nullptr;
};

YongIme &yong_ime();
