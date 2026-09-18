#pragma once

#include <set>
#include <string>
#include <vector>

#include "markdown.h"

// 竖排以「格」为最小单位:一行先分解为格子(Markdown 关闭时每 UTF-8 字符一格,
// 开启时块标记替换为等价符号、成对行内标记整段隐藏),列 = 连续 rowsPerCol 个格。
// 光标/导航按格索引映射,字节只存在于格的 [start,end) 区间内。
enum class VerticalCellKind { Normal, Heading, Muted, Rule };

struct VerticalCell {
    int start = 0;      // 原始行内字节起点
    int end = 0;        // 原始行内字节终点
    std::string glyph;  // 本格绘制的文本(通常一个字符)
    VerticalCellKind kind = VerticalCellKind::Normal;
    bool foldMark = false;  // 折叠标志格
    MdStyle style;          // 行内粗体/斜体/删除线
};

struct VerticalCol {
    int lineIdx = 0;
    int start = 0;  // 行内格起始索引
    int end = 0;    // 行内格结束索引(不含)
};

struct VerticalData {
    std::vector<std::vector<VerticalCell>> cells;  // 每行格子(被折叠隐藏的行保持为空)
    std::vector<VerticalCol> cols;
};

struct VerticalLayoutMetrics {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int rowAdvance = 0;
    int colAdvance = 0;
    int rows = 1;
    int cols = 1;
};

enum class VerticalGuideStyle { Solid, Dash, Dot };

VerticalLayoutMetrics vertical_metrics(int x, int y, int w, int h, int line_height);

// 被折叠标题吞掉的行。与横排叠加层用同一套语义,保证两种排版隐藏的行一致。
std::vector<char> vertical_fold_hidden(const std::vector<std::string> &lines,
                                       const std::set<int> *folded, bool md_on);

// 把可见行切成列。hidden_lines 标记被折叠吞掉的行;folded 里的标题行末尾补折叠标志格。
VerticalData build_vertical_data(const std::vector<std::string> &lines, int rows_per_col,
                                 const std::vector<char> *hidden_lines, bool md_on,
                                 const std::set<int> *folded,
                                 int cursor_line = -1, int cursor_byte = -1);

// bytePos 落在行内第几个格(行尾 → 格总数)。
int vertical_cell_row(const std::vector<VerticalCell> &cells, int byte_pos);

// bytePos 所在列(全局列索引)。隐藏字节映射到其后第一个可见格,行尾映射到末列。
int vertical_find_col(const VerticalData &data, int line_idx, int byte_pos);

// 列内第 row 格对应的字节位置(越界时截到列尾字节)。
int vertical_row_to_byte(const std::vector<VerticalCell> &cells, int col_start, int col_end, int row);
