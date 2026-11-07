#pragma once

// sixel 图像解码。**不依赖 LVGL**:纯 STL,所以 tests/sixel_test.cpp 能在宿主机上
// 直接编来跑,不用碰 framebuffer(和 vt.cpp / term_theme.cpp 一路)。
//
// 输入是 DCS 里 'q' 之后、ST 之前的那段载荷(见 vt.cpp 的 DCS 状态机)。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace term {

// 解码结果:ARGB8888(0xAARRGGBB,非预乘),和 LVGL 的 lv_color32_t 内存布局一致,
// 可以直接当 LV_COLOR_FORMAT_ARGB8888 的图像数据用。
// 未被任何 sixel 数据字节点亮的像素 alpha=0(透明)—— P2=1 的透明背景靠它。
struct SixelBitmap {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> argb;
};

// 解码一段 sixel 载荷。max_pixels 是 w*h 的上限(超了返回 false,不分配大块内存)。
// fg_rgb 是当前的 SGR 前景色(0xRRGGBB),0 号色按惯例就是它;传 0 表示「没给」,
// 保持 VT340 的黑色(和 fbterm 的 cur_fcolor_direct 判断一致)。
// 数据为空、解码不出任何像素、或超过上限时返回 false。
bool sixel_decode(const char *data, size_t len, size_t max_pixels, uint32_t fg_rgb,
                  SixelBitmap &out);

}  // namespace term
