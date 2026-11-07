#pragma once

// 终端配色。**这个文件不依赖 LVGL**(颜色都是 uint32_t 的 0xRRGGBB),所以能在宿主机
// 上直接编来测(见 tests/theme_test.cpp)。
//
// 为什么换主题是「换一张表」就够:Vt 的格子里存的不是 RGB,是调色板索引
// (vt.h 的 palette_color/is_palette)。屏幕上的字符只记「我是第 3 号色」,真正把
// 索引变成像素的只有 palette_rgb() 一处 —— 换掉这张表,屏上已有内容立刻跟着变色,
// 不用等 shell 重画。
//
// 约定和 xterm 一样:palette[0] 是底色、palette[7] 是正文色,palette[8..15] 是
// 对应的亮色(加粗会用,见 term_ui.cpp 的 cell_fg)。256 色立方和灰阶(idx >= 16)
// 是按公式算的,不随主题变。

#include <cstddef>
#include <cstdint>
#include <string>

namespace term {

struct TermTheme {
    const char *id;
    const char *label;    // 设置界面里显示的名字
    uint32_t palette[16]; // ANSI 0-15
    uint32_t cursor;      // 光标(块/下划线/竖线);块光标上的字用 palette[0] 挖出来
    uint32_t panel_bg;    // 候选条 / OSD 的底色
    uint32_t panel_fg;    // 候选条 / OSD 的文字
    uint32_t status;      // 右上角角标([中]/[EN])的文字色
};

// 设置界面要枚举整张表:返回首元素和个数。
const TermTheme *theme_table(size_t &count);

// 认不出的 id 一律退回第一个(typix)—— 用户可能存了个已经删掉的主题名,
// 不能因此变成没有配色。
const TermTheme &theme_by_id(const std::string &id);

const TermTheme &theme_current();
// 当前主题在表里的下标(和前后翻用的滚轮语义一样,-- / ++ 之后夹回范围)。
size_t theme_index();
// 只认表里的下标,越界夹到 [0, count)。
void theme_set_index(size_t i);
void theme_set_by_id(const std::string &id);

}  // namespace term
