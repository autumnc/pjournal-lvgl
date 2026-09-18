#include "vertical_layout.h"

#include "markdown.h"

static bool vt_is_fence(const std::string &line) {
    size_t lead = 0;
    while(lead < line.size() && line[lead] == ' ') lead++;
    return line.compare(lead, 3, "```") == 0 || line.compare(lead, 3, "~~~") == 0;
}

static void vt_code_flags(const std::vector<std::string> &lines, std::vector<char> &in_code) {
    in_code.assign(lines.size(), 0);
    bool code = false;
    for(size_t i = 0; i < lines.size(); ++i) {
        bool fence = vt_is_fence(lines[i]);
        in_code[i] = code ? 1 : 0;
        if(fence) code = !code;
    }
}

// 正文里 [from,to)(相对正文起点)的可见字符逐个成格。
static void vt_append_body(const std::string &line, int body_off, int from, int to,
                           VerticalCellKind kind, std::vector<VerticalCell> &cs) {
    if(to > (int)line.size() - body_off) to = (int)line.size() - body_off;
    if(from >= to) return;
    for(int p = body_off + from; p < body_off + to;) {
        size_t n = md_utf8_step(line, p);
        cs.push_back({p, (int)n, line.substr(p, n - p), kind, false});
        p = (int)n;
    }
}

static void vt_append_raw(const std::string &s, int base, VerticalCellKind kind,
                          std::vector<VerticalCell> &cs) {
    for(int p = 0; p < (int)s.size();) {
        size_t n = md_utf8_step(s, p);
        cs.push_back({base + p, base + (int)n, s.substr(p, n - p), kind, false});
        p = (int)n;
    }
}

VerticalLayoutMetrics vertical_metrics(int x, int y, int w, int h, int line_height) {
    VerticalLayoutMetrics m;
    m.x = x;
    m.y = y;
    m.w = w;
    m.h = h;
    m.rowAdvance = line_height + 2;
    m.colAdvance = line_height + 6;
    m.rows = h / m.rowAdvance;
    if(m.rows < 1) m.rows = 1;
    m.cols = w / m.colAdvance;
    if(m.cols < 1) m.cols = 1;
    return m;
}

std::vector<char> vertical_fold_hidden(const std::vector<std::string> &lines,
                                       const std::set<int> *folded, bool md_on) {
    std::vector<char> hidden(lines.size(), 0);
    if(!md_on || !folded || folded->empty()) return hidden;
    std::vector<char> in_code;
    vt_code_flags(lines, in_code);
    std::vector<int> hlevel(lines.size(), 0);
    for(size_t i = 0; i < lines.size(); ++i)
        if(!in_code[i]) hlevel[i] = md_heading_level_of(lines[i]);

    bool in_fold = false;
    int fold_level = 0;
    for(size_t i = 0; i < lines.size(); ++i) {
        if(in_fold && hlevel[i] && hlevel[i] <= fold_level) in_fold = false;
        hidden[i] = in_fold ? 1 : 0;
        if(!in_fold && hlevel[i] && folded->count((int)i)) {
            in_fold = true;
            fold_level = hlevel[i];
        }
    }
    return hidden;
}

VerticalData build_vertical_data(const std::vector<std::string> &lines, int rows_per_col,
                                 const std::vector<char> *hidden_lines, bool md_on,
                                 const std::set<int> *folded,
                                 int cursor_line, int cursor_byte) {
    VerticalData data;
    data.cells.resize(lines.size());
    if(rows_per_col < 1) rows_per_col = 1;

    std::vector<char> in_code;
    vt_code_flags(lines, in_code);

    for(size_t li = 0; li < lines.size(); ++li) {
        if(hidden_lines && li < hidden_lines->size() && (*hidden_lines)[li]) continue;
        const std::string &line = lines[li];
        auto &cs = data.cells[li];
        bool caret_here = ((int)li == cursor_line);

        if(!md_on) {
            vt_append_raw(line, 0, VerticalCellKind::Normal, cs);
        } else {
            MdRender r = md_build_line(line, in_code[li] != 0, caret_here ? cursor_byte : -1);
            bool folded_here = folded && folded->count((int)li) > 0;
            VerticalCellKind kind = r.heading ? VerticalCellKind::Heading
                                             : (r.muted ? VerticalCellKind::Muted
                                                        : VerticalCellKind::Normal);
            int lead = 0;
            while(lead < (int)line.size() && line[lead] == ' ') lead++;

            if(r.rule) {
                // 水平线/围栏行:原样字符竖排,样式弱化
                vt_append_raw(line, 0, VerticalCellKind::Rule, cs);
            } else {
                // 列表/引用的前导空格占空白格,保住缩进;块标记本身也归这一段
                for(int p = 0; p < lead; ++p) cs.push_back({p, p + 1, " ", kind, false});
                // 替换型块标记:尾部空格丢掉,所有格共享同一段原始字节
                int pend = r.prefix_bytes;
                while(pend > 0 && r.text[pend - 1] == ' ') pend--;
                for(int p = 0; p < pend;) {
                    size_t n = md_utf8_step(r.text, p);
                    cs.push_back({lead, r.body_off, r.text.substr(p, n - p), kind, false});
                    p = (int)n;
                }
                // 正文:成对行内标记整段隐藏
                int prev = 0;
                for(const auto &hs : r.hidden) {
                    vt_append_body(line, r.body_off, prev, hs.first, kind, cs);
                    prev = hs.second;
                }
                vt_append_body(line, r.body_off, prev, (int)line.size() - r.body_off, kind, cs);
                if(folded_here && r.heading) {
                    for(const char *g : {" ", "[", "+", "]"})
                        cs.push_back({lead, r.body_off, g, kind, true});
                }
            }
        }

        int start = 0, row = 0;
        for(int i = 0; i < (int)cs.size(); ++i) {
            if(row == rows_per_col) {
                data.cols.push_back({(int)li, start, i});
                start = i;
                row = 0;
            }
            ++row;
        }
        data.cols.push_back({(int)li, start, (int)cs.size()});
    }
    if(data.cols.empty()) data.cols.push_back({0, 0, 0});
    return data;
}

int vertical_cell_row(const std::vector<VerticalCell> &cells, int byte_pos) {
    int row = 0;
    for(const auto &c : cells)
        if(c.end <= byte_pos) ++row;
    return row;
}

int vertical_find_col(const VerticalData &data, int line_idx, int byte_pos) {
    if(line_idx < 0 || line_idx >= (int)data.cells.size()) return 0;
    const auto &cells = data.cells[line_idx];
    int rowB = vertical_cell_row(cells, byte_pos);
    int fallback = -1;
    for(int i = 0; i < (int)data.cols.size(); ++i) {
        const auto &c = data.cols[i];
        if(c.lineIdx != line_idx) continue;
        fallback = i;
        if(rowB >= c.start && rowB < c.end) return i;
        if(rowB == c.end) {
            if(c.end == (int)cells.size()) return i;
            if(c.start == c.end) return i;
        }
    }
    return fallback >= 0 ? fallback : 0;
}

int vertical_row_to_byte(const std::vector<VerticalCell> &cells, int col_start, int col_end, int row) {
    int idx = col_start + row;
    if(idx >= col_start && idx < col_end && idx < (int)cells.size()) return cells[idx].start;
    if(col_end > col_start && col_end <= (int)cells.size()) return cells[col_end - 1].end;
    return 0;
}
