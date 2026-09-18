#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <set>

static bool starts_with(const std::string &s, const char *p) {
    return s.rfind(p, 0) == 0;
}

std::string strip_inline_markdown(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for(size_t i = 0; i < text.size(); ++i) {
        if((text[i] == '*' || text[i] == '_' || text[i] == '`' || text[i] == '=' || text[i] == '~') &&
           i + 1 < text.size() && text[i + 1] == text[i]) {
            ++i;
            continue;
        }
        if(text[i] == '[' || text[i] == ']') continue;
        out += text[i];
    }
    return out;
}

std::vector<MdLine> parse_markdown_lines(const std::string &text) {
    std::vector<MdLine> lines;
    size_t pos = 0;
    while(pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        MdLine md;
        std::string trimmed = line;
        size_t first = trimmed.find_first_not_of(" \t\r");
        if(first == std::string::npos) {
            md.kind = MdKind::Blank;
            md.text = "";
        } else {
            trimmed = trimmed.substr(first);
            if(starts_with(trimmed, "# ")) { md.kind = MdKind::Heading1; md.text = trimmed.substr(2); }
            else if(starts_with(trimmed, "## ")) { md.kind = MdKind::Heading2; md.text = trimmed.substr(3); }
            else if(starts_with(trimmed, "### ")) { md.kind = MdKind::Heading3; md.text = trimmed.substr(4); }
            else if(starts_with(trimmed, "> ")) { md.kind = MdKind::Quote; md.text = trimmed.substr(2); }
            else if(starts_with(trimmed, "- ") || starts_with(trimmed, "* ")) { md.kind = MdKind::List; md.text = trimmed.substr(2); }
            else if(starts_with(trimmed, "```")) { md.kind = MdKind::Code; md.text = trimmed; }
            else if(trimmed == "---" || trimmed == "***") { md.kind = MdKind::Rule; md.text = ""; }
            else { md.kind = MdKind::Paragraph; md.text = trimmed; }
            md.text = strip_inline_markdown(md.text);
        }
        lines.push_back(md);
        if(nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

// ---------------------------------------------------------------------------
// 行级解析(横排叠加层与竖排共用)
// ---------------------------------------------------------------------------

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

// 找出正文里成对的行内标记(** __ == ~~ ` * _)并记下它们的字节范围。
// 光标落在某一对里面时整对都原样显示,方便直接改标记本身。
void md_inline_hidden(const std::string &body, int caret_rel,
                      std::vector<std::pair<int,int>> &out) {
    static const struct { const char *s; size_t len; bool undersc; } marks[] = {
        {"**", 2, false}, {"__", 2, true}, {"==", 2, false}, {"~~", 2, false},
        {"`", 1, false}, {"*", 1, false}, {"_", 1, true},
    };
    const int nmark = (int)(sizeof(marks) / sizeof(marks[0]));
    std::set<int> used;  // 已经配过对的标记位置,不能再当开标记用
    size_t i = 0;
    while(i < body.size()) {
        if(used.count((int)i)) {
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
            used.insert(a1);
            used.insert((int)close);
            if(!(caret_rel >= a1 && caret_rel < b2)) {
                out.push_back({a1, (int)(i + ml)});
                out.push_back({(int)close, b2});
            }
            i += ml;
            break;
        }
        if(!claimed) i = md_utf8_step(body, i);
    }
    std::sort(out.begin(), out.end());
}

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
            prefix = "\xe2\x96\x8d ";  // 粗竖线,比 # 直观
            l.body_off = (int)(lead + b);
        } else if(t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0 || md_is_rule(t)) {
            l.rule = true;
            l.body_off = (int)raw.size();
            return l;
        } else if(!t.empty() && t[0] == '>') {
            b = 1;
            while(b < t.size() && t[b] == ' ') b++;
            prefix = "\xe2\x96\x8d ";
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
            else prefix = "\xe2\x80\xa2 ";
            l.muted = task;
            l.body_off = (int)(lead + k);
        } else {
            size_t k = 0;
            while(k < t.size() && t[k] >= '0' && t[k] <= '9') k++;
            if(k > 0 && k < t.size()) {
                bool cn_sep = t.compare(k, 3, "\xe3\x80\x81") == 0;
                if(t[k] == '.' || t[k] == ')' || cn_sep) {
                    size_t m = k + (cn_sep ? 3 : 1);
                    while(m < t.size() && t[m] == ' ') m++;
                    prefix = t.substr(0, m);
                    l.body_off = (int)(lead + m);
                }
            }
        }
    }

    const std::string body = raw.substr((size_t)l.body_off);
    md_inline_hidden(body, caret_rel, l.hidden);

    l.prefix_bytes = (int)prefix.size();
    l.text = prefix;
    size_t prev = 0;
    for(const auto &hs : l.hidden) {
        int s = hs.first < (int)prev ? (int)prev : hs.first;
        if(hs.second <= s) continue;
        if(s > (int)prev) l.text.append(body, prev, (size_t)s - prev);
        prev = (size_t)hs.second;
    }
    if(prev < body.size()) l.text.append(body, prev, body.size() - prev);
    return l;
}

// 原始行内偏移 → 显示文本里的字节偏移。
int md_display_offset(const MdRender &l, int raw_rel) {
    if(raw_rel < l.body_off) return 0;
    int rel = raw_rel - l.body_off;
    int d = l.prefix_bytes + rel;
    for(const auto &hs : l.hidden) {
        if(hs.first >= rel) break;
        d -= (rel < hs.second ? rel : hs.second) - hs.first;
    }
    return d;
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
