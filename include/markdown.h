#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// 编辑器叠加层与竖排共用的行级解析(纯字符串,不依赖 UI)。
// 竖排用它把一行拆成「格」,横排叠加层用它决定显示文本与光标位置。
// ---------------------------------------------------------------------------

struct MdStyle {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool underline = false;
    bool invert = false;
    bool emph = false;
};

// 一段带样式的显示文本区间(相对 MdRender::text 的字节范围)。
struct MdRun {
    int lo = 0;
    int hi = 0;
    MdStyle st;
};

struct MdRender {
    std::string text;                        // 实际显示的文本(前缀 + 正文)
    int body_off = 0;                        // 正文在原始行里的字节偏移
    int prefix_bytes = 0;                    // 前缀占的显示字节数
    std::vector<std::pair<int,int>> hidden;  // 正文里要隐藏的字节范围(相对正文)
    std::vector<std::pair<int,int>> padded;  // 正文里换成空白但占位的字节范围
    std::vector<MdRun> runs;                 // 显示文本里的样式片段(升序、不重叠)
    bool heading = false;
    bool rule = false;
    bool muted = false;
    int level = 0;
};

struct MdDoc {
    std::vector<std::string> lines;
    std::vector<uint32_t> start;  // 每行在原文里的起始字节
    int caret_line = 0;
    int caret_rel = 0;
};

// 行首列表标记(含前导缩进)。
struct MdListMarker {
    bool ok = false;
    bool task = false;
    bool ordered = false;
    bool cn = false;   // 中文数字序号(一、)
    int start = 0;     // 标记在行内的起始字节(跳过缩进)
    int len = 0;       // 标记连同尾随空格的字节长度
    int indent = 0;    // 前导空白字节数(= start)
    int num = 0;       // 有序列表的当前序号
    int num_len = 0;   // 序号占的字节数(有序)
};

size_t md_utf8_step(const std::string &s, size_t i);
bool md_is_rule(const std::string &t);
int md_heading_level_of(const std::string &raw);
bool md_alnum(unsigned char c);
bool md_marker_edge(const std::string &b, size_t at, size_t len, bool underscore, bool opening);
// runs_out 的 lo/hi 相对正文起点;md_build_line 会把它们换算到显示坐标。
void md_inline_runs(const std::string &body, int caret_rel,
                    std::vector<std::pair<int,int>> &hidden_out,
                    std::vector<std::pair<int,int>> &padded_out,
                    std::vector<MdRun> &runs_out);
MdRender md_build_line(const std::string &raw, bool in_code, int caret_rel);
int md_display_offset(const MdRender &l, int raw_rel);
// 显示字节偏移处的行内样式(不在任何区间内 → 全部为 false)。
MdStyle md_style_at(const MdRender &l, int disp);
void md_split_caret(const std::string &text, uint32_t caret_byte, MdDoc &out);

// 列表标记识别(供回车续行用)。
MdListMarker md_list_marker(const std::string &line);
// 中文数字:读值(返回读到的位数)与生成。
int md_cn_num_value(const std::string &line, int from, int &num_len);
std::string md_cn_numeral(int n);
