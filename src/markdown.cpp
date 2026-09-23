#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

size_t md_utf8_step(const std::string &s, size_t i) {
    if(i >= s.size()) return s.size();
    unsigned char c = (unsigned char)s[i];
    size_t n = 1;
    if((c & 0xE0) == 0xC0) n = 2;
    else if((c & 0xF0) == 0xE0) n = 3;
    else if((c & 0xF8) == 0xF0) n = 4;
    if(i + n > s.size()) n = 1;
    return i + n;
}

bool md_is_rule(const std::string &t) {
    char c = 0;
    int n = 0;
    for(char ch : t) {
        if(ch == ' ' || ch == '\t') continue;
        if(ch != '-' && ch != '*' && ch != '_') return false;
        if(c == 0) c = ch;
        else if(c != ch) return false;
        n++;
    }
    return n >= 3;
}

int md_heading_level_of(const std::string &raw) {
    size_t lead = 0;
    while(lead < raw.size() && raw[lead] == ' ') lead++;
    int h = 0;
    while(lead + h < raw.size() && raw[lead + h] == '#' && h < 7) h++;
    if(h >= 1 && h <= 6 && (lead + h == raw.size() || raw[lead + h] == ' ')) return h;
    return 0;
}

bool md_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 标记要贴着内容:开标记后面不能是空白,闭标记前面不能是空白;
// 下划线另外不能在词内(否则 snake_case 会被当成强调)。
bool md_marker_edge(const std::string &b, size_t at, size_t len, bool underscore, bool opening) {
    if(opening) {
        size_t after = at + len;
        if(after >= b.size() || b[after] == ' ' || b[after] == '\t') return false;
        if(underscore && at > 0 && md_alnum((unsigned char)b[at - 1])) return false;
    } else {
        if(at == 0 || b[at - 1] == ' ' || b[at - 1] == '\t') return false;
        if(underscore && at + len < b.size() && md_alnum((unsigned char)b[at + len])) return false;
    }
    return true;
}

// 找出正文里成对的行内标记(** __ == ~~ ` * _)、链接与转义,记下各自的字节范围。
// 光标落在某一对里面时整对都原样显示,方便直接改标记本身;其余情况下把这一对
// 记成一段带样式的区间,交给渲染层换成粗体/斜体/删除线/着重号/反白字体。
// 链接的方括号与 URL、转义的斜杠都记进 padded —— 显示成同宽空白,宽度不变。
void md_inline_runs(const std::string &body, int caret_rel,
                    std::vector<std::pair<int,int>> &hidden_out,
                    std::vector<std::pair<int,int>> &padded_out,
                    std::vector<MdRun> &runs_out) {
    static const struct { const char *s; size_t len; bool undersc; MdStyle st; } marks[] = {
        {"***", 3, false, {true,  true,  false, false, false, false, false}},
        {"**",  2, false, {true,  false, false, false, false, false, false}},
        {"__",  2, true,  {false, false, false, true,  false, false, false}},
        {"==",  2, false, {false, false, false, false, false, true,  false}},
        {"~~",  2, false, {false, false, true,  false, false, false, false}},
        {"`",   1, false, {false, false, false, false, true,  false, false}},
        {"*",   1, false, {false, true,  false, false, false, false, false}},
        {"_",   1, true,  {false, true,  false, false, false, false, false}},
    };
    const int nmark = (int)(sizeof(marks) / sizeof(marks[0]));
    std::set<int> used;  // 已经配过对的标记位置,不能再当开标记用
    size_t i = 0;
    while(i < body.size()) {
        if(used.count((int)i)) {
            i = md_utf8_step(body, i);
            continue;
        }

        // 转义:`\x`(x 是单字节 ASCII)→ 反斜杠换成空格、x 原样显示,不再当标记
        if(body[i] == '\\' && i + 1 < body.size() && (unsigned char)body[i + 1] < 0x80) {
            padded_out.push_back({(int)i, (int)i + 1});
            i += 2;
            continue;
        }

        // 书名号《…》:两个书名号都隐藏,中间的文字画波浪线,与粗体的处理完全一致
        if(body.compare(i, 3, "\xe3\x80\x8a") == 0) {
            size_t close = body.find("\xe3\x80\x8b", i + 3);
            if(close != std::string::npos) {
                int a1 = (int)i, b2 = (int)close + 3;
                if(!(caret_rel >= a1 && caret_rel < b2)) {
                    hidden_out.push_back({a1, (int)i + 3});
                    hidden_out.push_back({(int)close, b2});
                    if(close > i + 3)
                        runs_out.push_back({(int)(i + 3), (int)close,
                                            {false, false, false, false, false, false, true}});
                }
                i = (size_t)b2;
                continue;
            }
            i = md_utf8_step(body, i);
            continue;
        }

        // 链接 `[文字](url)`:方括号与整个 URL 换成同宽空白,文字反白+下划线
        if(body[i] == '[') {
            size_t p = body.find("](", i + 1);
            if(p != std::string::npos) {
                size_t cp = body.find(')', p + 2);
                if(cp != std::string::npos) {
                    int a1 = (int)i, end = (int)cp + 1;
                    if(!(caret_rel >= a1 && caret_rel < end)) {
                        padded_out.push_back({a1, a1 + 1});
                        padded_out.push_back({(int)p, end});
                        if(p > i + 1)
                            runs_out.push_back({(int)(i + 1), (int)p,
                                                {false, false, false, true, true, false, false}});
                    }
                    i = (size_t)end;
                    continue;
                }
            }
            i = md_utf8_step(body, i);
            continue;
        }

        bool claimed = false;
        for(int m = 0; m < nmark; ++m) {
            size_t ml = marks[m].len;
            if(i + ml > body.size() || body.compare(i, ml, marks[m].s) != 0) continue;
            claimed = true;
            if(!md_marker_edge(body, i, ml, marks[m].undersc, true)) {
                i = md_utf8_step(body, i);
                break;
            }
            size_t j = i + ml;
            size_t close = std::string::npos;
            while((j = body.find(marks[m].s, j)) != std::string::npos) {
                if(!used.count((int)j) && md_marker_edge(body, j, ml, marks[m].undersc, false)) {
                    close = j;
                    break;
                }
                j += 1;
            }
            if(close == std::string::npos) {
                i = md_utf8_step(body, i);
                break;
            }
            int a1 = (int)i, b2 = (int)(close + ml);
            // 标记的每个字节都得占位。只记首字节的话,闭标记的第二个 '*' 会在后续
            // 遍历里被当成新的单 '*' 开标记,把紧随其后的 **粗体** 误配成斜体。
            for(size_t k = 0; k < ml; ++k) {
                used.insert((int)i + (int)k);
                used.insert((int)close + (int)k);
            }
            if(!(caret_rel >= a1 && caret_rel < b2)) {
                hidden_out.push_back({a1, (int)(i + ml)});
                hidden_out.push_back({(int)close, b2});
                if((marks[m].st.bold || marks[m].st.italic || marks[m].st.strike ||
                    marks[m].st.underline || marks[m].st.invert || marks[m].st.emph ||
                    marks[m].st.wavy) &&
                   close > i + ml)
                    runs_out.push_back({(int)(i + ml), (int)close, marks[m].st});
            }
            i += ml;
            break;
        }
        if(!claimed) i = md_utf8_step(body, i);
    }
    std::sort(hidden_out.begin(), hidden_out.end());
    std::sort(padded_out.begin(), padded_out.end());
}

// 标题前缀:分级 Nerd Font 图标(与 ESP32 版同一组码位),比一根竖线更能看出层级
static const char *kHeadingGlyph[6] = {
    "\xf3\xb0\x8e\xa4", "\xf3\xb0\x8e\xa7", "\xf3\xb0\x8e\xaa",
    "\xf3\xb0\x8e\xad", "\xf3\xb0\x8e\xb1", "\xf3\xb0\x8e\xb3",
};

MdRender md_build_line(const std::string &raw, bool in_code, int caret_rel) {
    MdRender l;
    std::string prefix;

    if(!in_code) {
        size_t lead = 0;
        while(lead < raw.size() && raw[lead] == ' ') lead++;
        std::string t = raw.substr(lead);
        size_t b = 0;

        int h = md_heading_level_of(raw);
        if(h > 0) {
            b = (size_t)h;
            while(b < t.size() && t[b] == ' ') b++;
            l.heading = true;
            l.level = h;
            prefix = std::string(kHeadingGlyph[h - 1]) + " ";
            l.body_off = (int)(lead + b);
        } else if(t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0 || md_is_rule(t)) {
            l.rule = true;
            l.body_off = (int)raw.size();
            return l;
        } else if(!t.empty() && t[0] == '>') {
            b = 1;
            while(b < t.size() && t[b] == ' ') b++;
            // 引用整块比正文右移一格。用两个半角空格而不是全角空格:横排宽度一样,
            // 竖排也是 2 格,与缩进两级列表(两个空格)对齐,两种排版看着一致。
            prefix = "  \xe2\x96\x8d ";
            l.muted = true;
            l.body_off = (int)(lead + b);
        } else if(t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            size_t k = 2;
            bool task = false, done = false;
            if(t.compare(k, 3, "[ ]") == 0) { task = true; k += 3; }
            else if(t.size() >= k + 3 && t[k] == '[' && (t[k + 1] == 'x' || t[k + 1] == 'X') && t[k + 2] == ']') {
                task = true; done = true; k += 3;
            }
            while(k < t.size() && t[k] == ' ') k++;
            if(task) prefix = done ? "[\xe2\x9c\x93] " : "[ ] ";
            else prefix = lead > 0 ? "\xe2\x97\x8b " : "\xe2\x80\xa2 ";  // 次级列表用空心圆
            l.muted = task;
            l.body_off = (int)(lead + k);
        } else {
            size_t k = 0;
            while(k < t.size() && t[k] >= '0' && t[k] <= '9') k++;
            bool cn_sep = false;
            if(k > 0 && k < t.size()) cn_sep = t.compare(k, 3, "\xe3\x80\x81") == 0;
            if(k > 0 && k < t.size() && (t[k] == '.' || t[k] == ')' || cn_sep)) {
                size_t m = k + (cn_sep ? 3 : 1);
                while(m < t.size() && t[m] == ' ') m++;
                prefix = t.substr(0, m);
                l.body_off = (int)(lead + m);
            } else {
                // 中文数字序号:一、二、十、十一、…(原文渲染,前缀即序号+顿号)
                int nl = 0;
                int v = md_cn_num_value(t, 0, nl);
                if(v >= 0 && nl > 0 && t.compare(nl, 3, "\xe3\x80\x81") == 0) {
                    size_t m = (size_t)nl + 3;
                    while(m < t.size() && t[m] == ' ') m++;
                    prefix = t.substr(0, m);
                    l.body_off = (int)(lead + m);
                }
            }
        }

        // 嵌套块的缩进(前导空格)跟着前缀一起显示:横排靠它右移,竖排照旧把这些
        // 字节排成空白格。前缀为空的行(普通段落)不动,竖排仍自己补缩进格。
        if(!prefix.empty() && lead > 0) prefix = raw.substr(0, lead) + prefix;

        // 光标落在块标记(#、-、1.、一、、>)的字节范围里时,标记原样平文显示;
        // 离开这一小段才换成图标/子弹并套上标题样式,与行内标记的显隐规则一致。
        if(l.body_off > 0 && caret_rel >= 0 && caret_rel < l.body_off) {
            prefix = raw.substr(0, (size_t)l.body_off);
            l.plain_marker = true;
            if(l.heading) { l.heading = false; l.level = 0; }
        }
    }

    const std::string body = raw.substr((size_t)l.body_off);
    // caret_rel 是行内偏移,而 md_inline_runs 里的标记区间是相对正文起点(body_off)的:
    // 标题、列表、引用这些带前缀的行必须先换算,否则光标落点跟标记对不上,
    // 该平文显示的时候渲染了、该渲染的时候又平文显示。
    std::vector<MdRun> raw_runs;
    md_inline_runs(body, caret_rel - l.body_off, l.hidden, l.padded, raw_runs);

    // 重叠的样式区间(如 ***粗斜***)按位取并集,再逐字节压成不重叠的显示片段。
    std::vector<char> hid(body.size(), 0), pad(body.size(), 0);
    for(const auto &hs : l.hidden) {
        for(int k = hs.first; k < hs.second && k < (int)body.size(); ++k)
            if(k >= 0) hid[(size_t)k] = 1;
    }
    for(const auto &ps : l.padded) {
        for(int k = ps.first; k < ps.second && k < (int)body.size(); ++k)
            if(k >= 0) pad[(size_t)k] = 1;
    }
    std::vector<MdStyle> sty(body.size());
    for(const MdRun &r : raw_runs) {
        for(int k = r.lo; k < r.hi && k < (int)body.size(); ++k) {
            if(k < 0) continue;
            sty[(size_t)k].bold |= r.st.bold;
            sty[(size_t)k].italic |= r.st.italic;
            sty[(size_t)k].strike |= r.st.strike;
            sty[(size_t)k].underline |= r.st.underline;
            sty[(size_t)k].invert |= r.st.invert;
            sty[(size_t)k].emph |= r.st.emph;
            sty[(size_t)k].wavy |= r.st.wavy;
        }
    }

    l.prefix_bytes = (int)prefix.size();
    l.text = prefix;
    l.runs.clear();
    for(size_t k = 0; k < body.size(); ++k) {
        if(hid[k]) continue;
        char ch = pad[k] ? ' ' : body[k];
        MdStyle s = sty[k];
        if(!s.bold && !s.italic && !s.strike && !s.underline && !s.invert && !s.emph && !s.wavy) {
            l.text += ch;
            continue;
        }
        int at = (int)l.text.size();
        if(!l.runs.empty() && l.runs.back().hi == at &&
           l.runs.back().st.bold == s.bold && l.runs.back().st.italic == s.italic &&
           l.runs.back().st.strike == s.strike && l.runs.back().st.underline == s.underline &&
           l.runs.back().st.invert == s.invert && l.runs.back().st.emph == s.emph &&
           l.runs.back().st.wavy == s.wavy)
            l.runs.back().hi = at + 1;
        else
            l.runs.push_back({at, at + 1, s});
        l.text += ch;
    }

    // 标题整行(含前缀图标)用粗体,和 ESP32 的 base.bold 一致。
    if(l.heading)
        l.runs.assign(1, {0, (int)l.text.size(), {true, false, false}});
    return l;
}

// 原始行内偏移 → 显示文本里的字节偏移。
int md_display_offset(const MdRender &l, int raw_rel) {
    // 光标在块标记里时前缀是原文,偏移一一对应,光标跟着标记走;否则整段前缀只算一个落点。
    if(raw_rel < l.body_off) return (l.plain_marker && raw_rel > 0) ? raw_rel : 0;
    int rel = raw_rel - l.body_off;
    int d = l.prefix_bytes + rel;
    for(const auto &hs : l.hidden) {
        if(hs.first >= rel) break;
        d -= (rel < hs.second ? rel : hs.second) - hs.first;
    }
    return d;
}

MdStyle md_style_at(const MdRender &l, int disp) {
    for(const auto &r : l.runs) {
        if(disp < r.lo) break;
        if(disp < r.hi) return r.st;
    }
    return MdStyle {};
}

void md_split_caret(const std::string &text, uint32_t caret_byte, MdDoc &out) {
    out.lines.clear();
    out.start.clear();
    size_t begin = 0;
    uint32_t pos = 0;
    while(true) {
        size_t nl = text.find('\n', begin);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string l = text.substr(begin, end - begin);
        while(!l.empty() && l.back() == '\r') l.pop_back();
        out.lines.push_back(l);
        out.start.push_back(pos);
        pos += (uint32_t)(end - begin) + 1;
        if(nl == std::string::npos) break;
        begin = nl + 1;
    }
    out.caret_line = 0;
    out.caret_rel = 0;
    for(int i = (int)out.lines.size() - 1; i >= 0; --i) {
        if(caret_byte >= out.start[i]) {
            out.caret_line = i;
            int rel = (int)(caret_byte - out.start[i]);
            if(rel > (int)out.lines[i].size()) rel = (int)out.lines[i].size();
            out.caret_rel = rel;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 列表标记与中文数字序号(回车续行用)
// ---------------------------------------------------------------------------

static bool md_cn_is_num(const std::string &s, int at) {
    static const char *kCn[13] = {"零", "一", "二", "三", "四", "五", "六",
                                  "七", "八", "九", "十", "百", "千"};
    if(at + 3 > (int)s.size()) return false;
    for(int k = 0; k < 13; ++k)
        if(s.compare(at, 3, kCn[k]) == 0) return true;
    return false;
}

int md_cn_num_value(const std::string &line, int from, int &num_len) {
    static const char *kDg[10] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    int len = (int)line.size();
    int value = 0, section = 0;
    int i = from;
    while(i + 3 <= len) {
        const std::string c = line.substr(i, 3);
        int dv = -1, mult = 0;
        for(int k = 0; k < 10; ++k)
            if(c == kDg[k]) { dv = k; break; }
        if(dv < 0) {
            if(c == "十") mult = 10;
            else if(c == "百") mult = 100;
            else if(c == "千") mult = 1000;
            else break;
        }
        if(mult) { value += (section == 0 ? 1 : section) * mult; section = 0; }
        else section = dv;
        i += 3;
    }
    num_len = i - from;
    if(num_len == 0) return -1;
    return value + section;
}

std::string md_cn_numeral(int n) {
    static const char *kCN[10] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    static const char *kUnit[4] = {"", "十", "百", "千"};
    if(n < 0 || n > 9999) return std::to_string(n);
    if(n < 10) return kCN[n];
    std::string out;
    int digits[4] = {0, 0, 0, 0};
    int len = 0;
    for(int t = n; t > 0; t /= 10) digits[len++] = t % 10;
    bool zeroPending = false;
    for(int i = len - 1; i >= 0; --i) {
        if(digits[i] == 0) {
            if(i > 0) zeroPending = true;
        } else {
            if(zeroPending) { out += kCN[0]; zeroPending = false; }
            out += kCN[digits[i]];
            out += kUnit[i];
        }
    }
    if(n >= 10 && n < 20) out.erase(0, 3);  // "一十"→"十"
    return out;
}

MdListMarker md_list_marker(const std::string &line) {
    MdListMarker m;
    int len = (int)line.size();
    int i = 0;
    while(i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    m.start = m.indent = i;
    int rest = len - i;

    if(rest >= 5 && (line.compare(i, 5, "- [ ]") == 0 || line.compare(i, 5, "- [x]") == 0 ||
                     line.compare(i, 5, "- [X]") == 0)) {
        m.ok = m.task = true;
        m.len = (i + 5 < len && line[i + 5] == ' ') ? 6 : 5;
        return m;
    }
    if(rest >= 2 && (line[i] == '-' || line[i] == '*' || line[i] == '+') && line[i + 1] == ' ') {
        m.ok = true;
        m.len = 2;
        return m;
    }
    int d = i;
    while(d < len && line[d] >= '0' && line[d] <= '9') d++;
    int nd = d - i;
    if(nd >= 1 && d + 2 < len && (unsigned char)line[d] == 0xE3 &&
       (unsigned char)line[d + 1] == 0x80 && (unsigned char)line[d + 2] == 0x81) {  // 、
        m.num_len = nd;
        m.num = std::atoi(line.substr(i, nd).c_str());
        m.len = nd + 3;
        if(d + 3 < len && line[d + 3] == ' ') m.len++;
        m.ordered = m.ok = true;
        return m;
    }
    if(nd >= 1 && d < len && (line[d] == '.' || line[d] == ')')) {
        if(d + 1 >= len) m.len = nd + 1;              // "1." 行尾
        else if(line[d + 1] == ' ') m.len = nd + 2;   // "1. "
        else return m;                                 // "1.5" 不是列表
        m.num_len = nd;
        m.num = std::atoi(line.substr(i, nd).c_str());
        m.ordered = m.ok = true;
        return m;
    }
    // 中文序号 + 顿号:一、二、十、十一、…
    int c = i, nchars = 0;
    while(c + 3 <= len && md_cn_is_num(line, c)) { c += 3; nchars++; }
    if(nchars >= 1 && c + 2 < len && (unsigned char)line[c] == 0xE3 &&
       (unsigned char)line[c + 1] == 0x80 && (unsigned char)line[c + 2] == 0x81) {
        int nl = 0;
        m.num = md_cn_num_value(line, i, nl);
        m.num_len = nl;
        m.cn = true;
        m.len = (c - i) + 3;
        if(c + 3 < len && line[c + 3] == ' ') m.len++;
        m.ordered = m.ok = true;
    }
    return m;
}
