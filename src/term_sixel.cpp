#include "term_sixel.h"

#include <algorithm>

namespace term {

namespace {

// VT340 默认 16 色(RGB 0-255)。和 fbterm 用同一张表:多数真实流(img2sixel 之类)
// 都会自己 #n;2;... 定义,这张默认表只在「用了没定义的色号」时才露脸。
const uint8_t kDefaultPalette[16][3] = {
    {0, 0, 0},       {0, 0, 205},     {0, 205, 0},     {0, 205, 205},
    {205, 0, 0},     {205, 0, 205},   {205, 205, 0},   {205, 205, 205},
    {0, 0, 238},     {0, 0, 255},     {0, 238, 0},     {0, 255, 0},
    {238, 0, 0},     {238, 0, 255},   {238, 238, 0},   {238, 238, 255},
};

// 16..255 和 xterm 的 256 色调色板一致(6x6x6 色立方 + 24 级灰阶),这样 sixel 流
// 里直接引用 xterm 色号时颜色是对得上的。
uint32_t xterm_color(int idx) {
    static const uint32_t ramp[6] = {0, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    if(idx < 232) {
        const int i = idx - 16;
        return (ramp[i / 36] << 16) | (ramp[(i / 6) % 6] << 8) | ramp[i % 6];
    }
    const uint32_t g = 8u + (uint32_t)(idx - 232) * 10u;
    return (g << 16) | (g << 8) | g;
}

void init_palette(uint32_t *pal, uint32_t fg_rgb) {
    for(int i = 0; i < 256; ++i) {
        if(i < 16) {
            pal[i] = ((uint32_t)kDefaultPalette[i][0] << 16) |
                     ((uint32_t)kDefaultPalette[i][1] << 8) | (uint32_t)kDefaultPalette[i][2];
        } else {
            pal[i] = xterm_color(i);
        }
    }
    // 0 号色是「当前前景色」(VT340 的惯例)。调用方给的是真彩前景才用,否则保持黑色。
    if(fg_rgb) pal[0] = fg_rgb & 0x00ffffffu;
}

uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// #n;2;R;G;B 的 R/G/B 是 0-100 的百分比。
uint32_t rgb100(int r, int g, int b) {
    auto pct = [](int v) { return clamp8((v < 0 ? 0 : (v > 100 ? 100 : v)) * 255 / 100); };
    return ((uint32_t)pct(r) << 16) | ((uint32_t)pct(g) << 8) | (uint32_t)pct(b);
}

// #n;1;H;L;S 的 H 是 0-360 度、L/S 是 0-100。标准 HLS→RGB。
uint32_t hls300(int h, int l, int s) {
    double hh = (double)(h % 360) / 360.0;
    if(hh < 0) hh += 1.0;
    const double ll = (l < 0 ? 0 : (l > 100 ? 100 : l)) / 100.0;
    const double ss = (s < 0 ? 0 : (s > 100 ? 100 : s)) / 100.0;

    auto hue2rgb = [](double p, double q, double t) {
        if(t < 0) t += 1;
        if(t > 1) t -= 1;
        if(t < 1.0 / 6.0) return p + (q - p) * 6 * t;
        if(t < 1.0 / 2.0) return q;
        if(t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6;
        return p;
    };

    double r, g, b;
    if(ss <= 0.0) {
        r = g = b = ll;
    } else {
        const double q = ll < 0.5 ? ll * (1 + ss) : ll + ss - ll * ss;
        const double p = 2 * ll - q;
        r = hue2rgb(p, q, hh + 1.0 / 3.0);
        g = hue2rgb(p, q, hh);
        b = hue2rgb(p, q, hh - 1.0 / 3.0);
    }
    return ((uint32_t)clamp8((int)(r * 255 + 0.5)) << 16) |
           ((uint32_t)clamp8((int)(g * 255 + 0.5)) << 8) |
           (uint32_t)clamp8((int)(b * 255 + 0.5));
}

}  // namespace

bool sixel_decode(const char *data, size_t len, size_t max_pixels, uint32_t fg_rgb,
                  SixelBitmap &out) {
    if(!data || len == 0 || max_pixels == 0) return false;

    uint32_t pal[256];
    init_palette(pal, fg_rgb);

    int cur = 0;                              // 当前色号
    int x = 0, band = 0;                      // 光标:像素列、6 行带
    int declared_w = 0, declared_h = 0;       // "  ...;Ph;Pv 声明的尺寸
    size_t max_x = 0;
    bool oom = false;

    // 每行像素一个 vector,按需向右长。带上限守一把:畸形/恶意流别把内存撑爆。
    std::vector<std::vector<uint32_t>> rows;
    auto put = [&](int px, int py, uint32_t argb) {
        if(px < 0 || py < 0) return;
        if((size_t)px >= max_pixels || (size_t)py >= max_pixels) { oom = true; return; }
        const size_t cols = max_x > (size_t)px + 1 ? max_x : (size_t)px + 1;
        if((size_t)(py + 1) * cols > max_pixels) { oom = true; return; }
        if((size_t)py >= rows.size()) rows.resize((size_t)py + 1);
        std::vector<uint32_t> &r = rows[(size_t)py];
        if((size_t)px >= r.size()) r.resize((size_t)px + 1, 0);
        r[(size_t)px] = argb;
        if((size_t)px + 1 > max_x) max_x = (size_t)px + 1;
    };
    // 一个 sixel 数据字符:6 个竖像素(bit0 在上),占 1 列。置位画当前色,否则留透明。
    auto emit = [&](int mask) {
        if(mask) {
            const uint32_t argb = pal[cur] | 0xff000000u;
            for(int b = 0; b < 6; ++b)
                if(mask & (1 << b)) put(x, band * 6 + b, argb);
        }
        ++x;
    };
    // 读一串数字(无数字返回 0),不越过非数字。sixel 的参数最大只到 65535,
    // 多出来的位数直接夹掉,免得 `!999999999-` 这种串把循环拖死。
    auto read_int = [&](size_t &pos) {
        int v = 0;
        while(pos < len && data[pos] >= '0' && data[pos] <= '9') {
            v = v * 10 + (data[pos] - '0');
            if(v > 65535) v = 65535;
            ++pos;
        }
        return v;
    };
    auto skip_semi = [&](size_t &pos) {
        if(pos < len && data[pos] == ';') ++pos;
    };

    size_t i = 0;
    while(i < len && !oom) {
        const uint8_t c = (uint8_t)data[i];
        if(c == '"') {
            // 栅格属性:"Pan;Pad;Ph;Pv(Pan/Pad 不用,Ph/Pv 是像素宽高)
            ++i;
            (void)read_int(i);
            skip_semi(i);
            (void)read_int(i);
            skip_semi(i);
            declared_w = read_int(i);
            skip_semi(i);
            declared_h = read_int(i);
            continue;
        }
        if(c == '#') {
            ++i;
            int pc = read_int(i);
            if(pc < 0 || pc > 255) pc = 0;
            if(i < len && data[i] == ';') {
                ++i;
                const int pu = read_int(i);
                skip_semi(i);
                if(pu == 2) {
                    const int r = read_int(i);
                    skip_semi(i);
                    const int g = read_int(i);
                    skip_semi(i);
                    const int b = read_int(i);
                    pal[pc] = rgb100(r, g, b);
                } else if(pu == 1) {
                    const int h = read_int(i);
                    skip_semi(i);
                    const int l = read_int(i);
                    skip_semi(i);
                    const int s = read_int(i);
                    pal[pc] = hls300(h, l, s);
                }
            }
            cur = pc;
            continue;
        }
        if(c == '!') {
            // 重复:!Pn 后面那个数据字符重复 Pn 次
            ++i;
            int n = read_int(i);
            if(n < 1) n = 1;
            if(n > 65535) n = 65535;  // 同 fbterm:重复次数封顶,别让畸形的流把这里卡死
            if(i < len) {
                const uint8_t d = (uint8_t)data[i];
                if(d >= 0x3f && d <= 0x7e) {
                    ++i;
                    for(int k = 0; k < n && !oom; ++k) emit(d - 0x3f);
                    continue;
                }
            }
            continue;  // '!' 后面不是数据字节,忽略
        }
        if(c == '$') {  // 回车:回左缘,不换带
            x = 0;
            ++i;
            continue;
        }
        if(c == '-') {  // 换行:下一条 6 行带,回左缘
            ++band;
            x = 0;
            ++i;
            continue;
        }
        if(c >= 0x3f && c <= 0x7e) {  // 数据字节:值 = c - 0x3f,6 位掩码
            emit(c - 0x3f);
            ++i;
            continue;
        }
        ++i;  // 空白/换行/未知字节:跳过
    }
    if(oom) return false;

    const int w = (int)std::max(max_x, (size_t)declared_w);
    const int h = (int)std::max(rows.size(), (size_t)declared_h);
    if(w <= 0 || h <= 0) return false;
    if((size_t)w * (size_t)h > max_pixels) return false;

    out.w = w;
    out.h = h;
    out.argb.assign((size_t)w * (size_t)h, 0);
    for(size_t py = 0; py < rows.size(); ++py) {
        const std::vector<uint32_t> &r = rows[py];
        const size_t n = std::min(r.size(), (size_t)w);
        for(size_t px = 0; px < n; ++px) out.argb[py * (size_t)w + px] = r[px];
    }
    return true;
}

}  // namespace term
