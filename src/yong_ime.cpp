#include "yong_ime.h"

#include "IME.h"
#include "key_codes.h"

#ifdef PJOURNAL_HAS_YONG

#include "yong.h"
#include "yong_bridge.h"

#include <cstring>

namespace {

// 和 yong_bridge.c 的 YB_CAND_PER_PAGE 对齐(我们给引擎设的 EIM.CandWordMax)
constexpr int kCandPerPage = 9;

// EIM.CodeInput 最长 MAX_CODE_LEN(63)个 GBK 字节;StringGet 最长 MAX_CAND_LEN(255)。
// GBK→UTF-8 最坏 3 倍,再多留一点余量。
constexpr int kCodeBuf = MAX_CODE_LEN * 4 + 8;
constexpr int kCandBuf = MAX_CAND_LEN * 4 + 8;
constexpr int kPendBuf = MAX_CAND_LEN * 16 + 8;

} // namespace

bool YongIme::begin() {
    if(_ready) return true;
    _ready = yb_begin() != 0;
    if(!_ready) return false;
    refresh_candidates();
    return true;
}

void YongIme::set_active(bool on) {
    if(_active == on) return;
    _active = on;
    if(!on) set_english_proxy(false);
    reset_engine();
}

bool YongIme::composing() const {
    if(_english) return IME::getInstance().composing();
    if(!_ready) return false;
    return yb_code_len() > 0 || yb_cand_count() > 0;
}

std::string YongIme::composition() const {
    if(_english) return IME::getInstance().displayCode();
    if(!_ready) return std::string();
    char buf[kCodeBuf];
    if(yb_code_input_utf8(buf, sizeof buf) <= 0) return std::string();
    return std::string(buf);
}

std::string YongIme::schema_name() const {
    if(!_ready) return std::string();
    return std::string(yb_im_name(yb_cur_im()));
}

void YongIme::toggle_fullwidth() { _fullwidth = !_fullwidth; }

void YongIme::toggle_trad() {
    _trad = !_trad;
    refresh_candidates();
}

void YongIme::toggle_english() { set_english_proxy(!_english); }

void YongIme::set_display_width(int px) {
    _budget = px;
    // 英文代理那边是内置 IME 在分页,宽度也得给它一份
    IME::getInstance().setDisplayWidth(px);
    sync_page_fit();
}

void YongIme::set_page_size(int n) {
    if(n > 0) _rows = n;
    sync_page_fit();
}

void YongIme::set_width_fn(WidthFn fn) {
    _width_fn = fn;
    IME::getInstance().setWidthFn(fn);
    sync_page_fit();
}

bool YongIme::switch_schema() {
    if(!_ready) return false;
    int n = yb_im_count();
    if(n <= 1) return false;
    set_english_proxy(false);
    if(!yb_load_im((yb_cur_im() + 1) % n)) return false;
    refresh_candidates();
    return true;
}

void YongIme::reset_engine() {
    yb_reset();
    // TableReset 不清 StringGet,留着会被下一轮当上屏串重复吐出去
    yb_clear_string_get();
    refresh_candidates();
}

// 从引擎捞当前这一页。页宽(CandWordMax)由 sync_page_fit 维护,这里只管取。
void YongIme::refresh_candidates() {
    if(_english) {
        IME &b = IME::getInstance();
        _cands = b.candidates();
        _highlight = b.highlightIdx();
        _page = b.currentPage();
        _page_count = b.totalPages();
        _total = b.totalCandidates();
        return;
    }

    _cands.clear();
    int n = _ready ? yb_cand_count() : 0;
    for(int i = 0; i < n; i++) {
        char buf[kCandBuf];
        if(yb_cand_utf8(i, buf, sizeof buf, _trad ? 1 : 0) > 0) _cands.emplace_back(buf);
        else _cands.emplace_back();
    }
    _page_count = _ready ? yb_cand_page_count() : 0;
    if(_page_count < 1) _page_count = 1;
    _page = _ready ? yb_cur_cand_page() + 1 : 1;   // 引擎那边是 0 起
    if(_page < 1) _page = 1;
    _highlight = (_ready && n > 0) ? yb_select_index() : -1;
    if(_highlight < 0 || _highlight >= n) _highlight = n > 0 ? 0 : -1;
    // 总候选数:引擎只给「页数 + 本页个数」,满页都是 CandWordMax 个
    int per_page = _ready ? yb_cand_word_max() : 0;
    if(per_page < 1) per_page = kCandPerPage;
    _total = n > 0 ? (_page_count - 1) * per_page + n : 0;
}

// 这一页按当前可用空间放得下几个候选:横排按像素(_budget,和候选条画出来的
// " 编号.候选" 前缀量宽方式一致),竖排按行数(_rows)。量不出来返回 0 = 别动引擎。
int YongIme::fit_count() const {
    if(_cands.empty()) return 0;
    if(_budget > 0 && _width_fn) {
        int used = 0, fit = 0;
        for(size_t i = 0; i < _cands.size(); ++i) {
            std::string part = " " + std::to_string((int)i + 1) + "." + _cands[i];
            int w = _width_fn(part.c_str());
            if(i > 0 && used + w > _budget) break;
            used += w;
            ++fit;
        }
        // 一个都放不下时也别把页宽压成 0,留 1 让界面自己去裁
        return fit > 0 ? fit : 1;
    }
    return _rows;
}

// 引擎按 CandWordMax 分页。量出来的个数和引擎现在的页宽不一致,就让引擎按新页宽重排
// 第一页——放不下的候选整项挪到下一页、编号从 1 重新计,和内置输入法一个策略。
// 每次候选表重建之后、以及候选条宽度/行数变化时都要过一遍。
//
// 量出来的个数不能无条件当页宽。本页没满(n < CandWordMax)说明候选就这么多,"都放得下"
// 只意味着页宽有富余,不该拿 n 去覆盖页宽——早先没分辨这一点,一个只有 3 个候选的码就
// 把页宽永久钉成 3,之后所有码都只出一页 3 个。反过来,页宽被压小之后满页的那一页再也
// 量不出"本来还能放几个",得先按上限重取一次第一页当样本。
void YongIme::sync_page_fit() {
    if(!_ready || _english) return;
    // 只拿第一页来量:别的页是用户自己翻过去的。同一个码的候选长短差不多,第一页量
    // 出来的个数拿去分页够用(编码一变引擎自己就回到第一页,那时再量)。
    if(yb_cur_cand_page() != 0) return;

    int n = yb_cand_count();
    if(n <= 0) return;
    int max = yb_cand_word_max();
    int fit = fit_count();
    if(fit > kCandPerPage) fit = kCandPerPage;
    if(fit <= 0) return;

    if(n >= max && fit >= n && max != kCandPerPage) {
        // 满页且全都放得下:页宽可能先前被压小过,按上限重取一次再量
        yb_set_cand_word_max(kCandPerPage);
        yb_get_cand_words(PAGE_FIRST);
        refresh_candidates();
        n = yb_cand_count();
        if(n <= 0) return;
        fit = fit_count();
        if(fit > kCandPerPage) fit = kCandPerPage;
        if(fit <= 0) return;
    }
    // 这一页全放得下,页宽有富余,不动
    if(fit >= n) return;
    yb_set_cand_word_max(fit);
    yb_get_cand_words(PAGE_FIRST);
    refresh_candidates();
}

void YongIme::append_string_get(std::string &out) {
    char buf[kCandBuf];
    if(yb_string_get_utf8(buf, sizeof buf, _trad ? 1 : 0) > 0) out += buf;
}

void YongIme::drain_pending(std::string &out) {
    char buf[kPendBuf];
    if(yb_take_output(buf, sizeof buf, _trad ? 1 : 0) > 0) out += buf;
}

int YongIme::to_yong_key(int key) const {
    switch(key) {
    case '\n': return YK_ENTER;
    case '\t': return YK_TAB;
    case 8: return YK_BACKSPACE;
    case 27: return YK_ESC;
    case 127: return YK_DELETE;
    default: break;
    }
    if(key >= 'A' && key <= 'Z') return key - 'A' + 'a';   // 大写归一到基本键
    if(key >= 'a' && key <= 'z') return key;
    if(key >= '0' && key <= '9') return key;
    if(key > 0x1f && key < 0x7f) return key;               // 其余 ASCII 可打印字符
    if(key >= 0x01 && key <= 0x1a) return ('a' + key - 1) | KEYM_CTRL;
    switch(key) {
    case KEY_UP: return YK_UP;
    case KEY_DOWN: return YK_DOWN;
    case KEY_LEFT: return YK_LEFT;
    case KEY_RIGHT: return YK_RIGHT;
    case KEY_HOME: return YK_HOME;
    case KEY_END: return YK_END;
    case KEY_SHIFT_UP: return YK_UP | KEYM_SHIFT;
    case KEY_SHIFT_DOWN: return YK_DOWN | KEYM_SHIFT;
    case KEY_SHIFT_LEFT: return YK_LEFT | KEYM_SHIFT;
    case KEY_SHIFT_RIGHT: return YK_RIGHT | KEYM_SHIFT;
    case KEY_CTRL_ENTER: return YK_ENTER | KEYM_CTRL;
    case KEY_CTRL_I: return 'i' | KEYM_CTRL;
    case KEY_SEARCH: return '/' | KEYM_CTRL;
    case KEY_HELP: return '/' | KEYM_CTRL | KEYM_SHIFT;
    case KEY_REDO: return 'z' | KEYM_CTRL | KEYM_SHIFT;
    default: return YK_NONE;
    }
}

// 引擎不认的键(yong 驱动侧兜底的那一段):先看有没有编码/候选,该翻页翻页、该选字
// 选字、该上屏上屏;没有组字时走标点/全角。
bool YongIme::next_key(int yk, std::string &out) {
    int n = yb_cand_count();

    if(n > 0 || yb_code_len() > 0) {
        if(yk == yb_key_pagedown()) {
            if(yb_cur_cand_page() + 1 < yb_cand_page_count()) yb_get_cand_words(PAGE_NEXT);
            refresh_candidates();
            return true;
        }
        if(yk == yb_key_pageup()) {
            if(yb_cand_page_count() > 0) yb_get_cand_words(PAGE_PREV);
            refresh_candidates();
            return true;
        }
        int pos = yb_select_pos(yk);
        if(pos >= 0 && pos < n) {
            if(yb_get_cand_word(pos)) {
                append_string_get(out);
                reset_engine();
            } else {
                // GetCandWord 返回 0 不等于「没选上」:拼音引擎里选中一个只吃掉一部分
                // 编码的候选(如 nihao 里选「泥」)时,它把这一段攒进 StringGet、把
                // 剩下的编码留着继续组,返回的却是 NULL。这一支不能 reset —— 一 reset
                // 就把攒好的那截丢了(现象是数字键按下去候选条消失、什么都不上屏)。
                // 照 yong 驱动里 `else { ret=IMR_DISPLAY; goto IMR_TEST; }`。
                refresh_candidates();
                sync_page_fit();
            }
            return true;
        }
        if(yb_is_commit_key(yk)) {
            if(yb_get_cand_word(yb_select_index())) {
                append_string_get(out);
                reset_engine();
            } else {
                refresh_candidates();
            }
            return true;
        }
        if(yk == YK_ENTER) {
            // 回车把编码原样吐出去(enter_mode 默认 0),不选候选
            char buf[kCodeBuf];
            if(yb_code_input_utf8(buf, sizeof buf) > 0) out += buf;
            reset_engine();
            return true;
        }
    }
    return punct_key(yk, out);
}

// 设备上没有 yong 的 bd.txt 标点表,符号和全角都借内置输入法那两个转换
// (IME::handleFullwidthPunct 本来就只在 IME 开着时用,和这里的语义一致)。
bool YongIme::punct_key(int yk, std::string &out) {
    if(yk <= 0 || yk >= 0x80) return false;
    IME &b = IME::getInstance();
    std::string t;
    if(b.handleFullwidthPunct(yk, t)) {
        out += t;
        return true;
    }
    if(_fullwidth && b.handleFullwidthChar(yk, t)) {
        out += t;
        return true;
    }
    return false;
}

bool YongIme::english_key(int key, std::string &out) {
    // 再按一次临时英文键退出英文(yong 里 key==key_temp_english 走的是 CNen 那支);
    // 正在查词的时候还是当普通键拼进去,免得把查了一半的词切断。
    if(key == yb_key_temp_english() && yb_key_temp_english() != YK_NONE &&
       !IME::getInstance().composing()) {
        set_english_proxy(false);
        return true;
    }
    bool consumed = IME::getInstance().handleKey(key, out);
    // 英文候选是内置输入法现算的,每次喂键后都得重新捞一遍(它没有自己的 pump)
    refresh_candidates();
    return consumed || !out.empty();
}

void YongIme::set_english_proxy(bool on) {
    if(_english == on) return;
    _english = on;
    IME &b = IME::getInstance();
    b.setActive(on);
    b.setEnglish(on);
    if(on) yb_reset();   // 进英文前把 yong 的组字清掉(照 yong 的 YongResetIM_)
    refresh_candidates();
}

bool YongIme::handle_key(int key, std::string &out) {
    out.clear();
    if(!_active || !_ready) return false;
    if(_english) return english_key(key, out);

    // Home/End 翻页(和内置输入法一致)。组字时吃掉,免得编辑器把光标顶到行首/行尾;
    // 没组字时返回 false,Home/End 还是编辑器的光标移动。
    if(key == KEY_HOME || key == KEY_END) {
        if(yb_code_len() == 0 && yb_cand_count() == 0) return false;
        if(key == KEY_HOME) {
            if(yb_cand_page_count() > 0) yb_get_cand_words(PAGE_PREV);
        } else if(yb_cur_cand_page() + 1 < yb_cand_page_count()) {
            yb_get_cand_words(PAGE_NEXT);
        }
        refresh_candidates();
        return true;
    }

    if(key >= 'A' && key <= 'Z') {
        // 大写字母不喂引擎(yong 驱动侧也一样):先把高亮候选吐出去,再把这个字母原样
        // 交给编辑器。
        if(yb_cand_count() > 0 && yb_get_cand_word(yb_select_index())) append_string_get(out);
        reset_engine();
        out.push_back((char)key);
        return true;
    }

    int yk = to_yong_key(key);
    if(yk == YK_NONE) return false;

    // 临时英文键([key] tEN,设备上拼音是 v):空码时切到英文,这个键本身也交给英文词库
    if(yk == yb_key_temp_english() && yb_code_len() == 0 && yb_cand_count() == 0) {
        set_english_proxy(true);
        if(english_key(key, out)) return true;
        set_english_proxy(false);
    }

    // 拆字(笔画)键([key] bihua,设备上拼音是 u):空码时切进拆字引擎,这个键本身也归它
    // (会被当成拆字行的第一个字符收进去)。引擎不可用(bihua.bin 没读到)就不进,照原样走。
    if(!yb_bihua_active() && yb_code_len() == 0 && yb_cand_count() == 0) {
        int bk = yb_bihua_key();
        if(bk != YK_NONE && yk == bk) yb_bihua_enter();
    }

    bool consumed;
    // 引擎重建了候选表 → 量一下这一页真正放得下几个,和引擎的页宽不一致就重排。
    // 翻页/选字那支是用户自己选过来的页,重排会把他顶回第一页,所以跳过。
    bool refit = true;
    switch(yb_do_input(yk)) {
    case IMR_BLOCK:
        refresh_candidates();
        consumed = true;
        break;
    case IMR_PASS:
        consumed = false;
        break;
    case IMR_CLEAN_PASS:
        reset_engine();
        consumed = false;
        break;
    case IMR_CLEAN:
        reset_engine();
        consumed = true;
        break;
    case IMR_DISPLAY:
        refresh_candidates();
        consumed = true;
        break;
    case IMR_COMMIT:
        append_string_get(out);
        reset_engine();
        consumed = true;
        break;
    case IMR_COMMIT_DISPLAY:
        append_string_get(out);
        yb_clear_string_get();
        refresh_candidates();
        consumed = true;
        break;
    case IMR_PUNC:
        // 引擎先塞了要上屏的字(如候选是 $SPACE 命令),再把命令留在 StringGet
        append_string_get(out);
        yb_clear_string_get();
        punct_key(yk, out);
        reset_engine();
        consumed = true;
        break;
    case IMR_ENGLISH:
    case IMR_CHINGLISH:
        set_english_proxy(true);
        consumed = true;
        break;
    default:
        consumed = next_key(yk, out);
        refit = false;
        break;
    }
    if(refit) sync_page_fit();
    drain_pending(out);
    return consumed || !out.empty();
}

#else // !PJOURNAL_HAS_YONG

bool YongIme::begin() { return false; }
bool YongIme::composing() const { return false; }
void YongIme::set_active(bool) {}
std::string YongIme::composition() const { return std::string(); }
std::string YongIme::schema_name() const { return std::string(); }
void YongIme::toggle_fullwidth() {}
void YongIme::toggle_trad() {}
void YongIme::toggle_english() {}
void YongIme::set_display_width(int) {}
void YongIme::set_width_fn(WidthFn) {}
void YongIme::set_page_size(int) {}
bool YongIme::switch_schema() { return false; }
bool YongIme::handle_key(int, std::string &) { return false; }

#endif

YongIme &yong_ime() {
    static YongIme inst;
    return inst;
}
