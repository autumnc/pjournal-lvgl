#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class MdKind { Paragraph, Heading1, Heading2, Heading3, Quote, List, Code, Rule, Blank };

struct MdLine {
    MdKind kind = MdKind::Paragraph;
    std::string text;
};

std::vector<MdLine> parse_markdown_lines(const std::string &text);
std::string strip_inline_markdown(const std::string &text);

// ---------------------------------------------------------------------------
// 编辑器叠加层与竖排共用的行级解析(纯字符串,不依赖 UI)。
// 竖排用它把一行拆成「格」,横排叠加层用它决定显示文本与光标位置。
// ---------------------------------------------------------------------------

struct MdRender {
    std::string text;                        // 实际显示的文本(前缀 + 正文)
    int body_off = 0;                        // 正文在原始行里的字节偏移
    int prefix_bytes = 0;                    // 前缀占的显示字节数
    std::vector<std::pair<int,int>> hidden;  // 正文里要隐藏的字节范围(相对正文)
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

size_t md_utf8_step(const std::string &s, size_t i);
bool md_is_rule(const std::string &t);
int md_heading_level_of(const std::string &raw);
bool md_alnum(unsigned char c);
bool md_marker_edge(const std::string &b, size_t at, size_t len, bool underscore, bool opening);
void md_inline_hidden(const std::string &body, int caret_rel,
                      std::vector<std::pair<int,int>> &out);
MdRender md_build_line(const std::string &raw, bool in_code, int caret_rel);
int md_display_offset(const MdRender &l, int raw_rel);
void md_split_caret(const std::string &text, uint32_t caret_byte, MdDoc &out);
