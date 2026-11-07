#include "term_theme.h"

namespace term {

namespace {

// typix 就是原来的那套配色,palette 是从 term_ui.cpp 里逐字搬过来的 —— 默认外观
// 必须一模一样,换了主题表不该顺手把老用户的屏幕改了色。
//
// 其余几套的 16 色取自各家配色方案公开的 ANSI 定义(Solarized / Gruvbox / Nord /
// Dracula),这样在别的机器上认得的颜色这边也对得上。id 和 label 尽量和
// pjournal-lvgl-typix 的 k_themes 对齐(那边只有 bg/fg/accent,没有 16 色)。
const TermTheme kThemes[] = {
    {"typix", "Typix",
     {0x101014, 0xcc5555, 0x55aa55, 0xccaa44, 0x5588cc, 0xaa66cc, 0x44aaaa, 0xd0d0d0, 0x555a66,
      0xff6b6b, 0x7fdc7f, 0xffd479, 0x7fb8ff, 0xd29bff, 0x6fdfd6, 0xffffff},
     0xd0d0d0, 0x1c1c26, 0xe6e6f0, 0xffd479},

    {"light", "白底黑字",
     {0xffffff, 0xcc0000, 0x4e9a06, 0xc4a000, 0x3465a4, 0x75507b, 0x06989a, 0x555753, 0xa0a0a0,
      0xef2929, 0x8ae234, 0xfce94f, 0x729fcf, 0xad7fa8, 0x34e2e2, 0x000000},
     0x000000, 0xe4e4e8, 0x1c1c26, 0x8a6b3f},

    {"contrast", "高对比",
     {0x000000, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff, 0x808080,
      0xff8080, 0x80ff80, 0xffff80, 0x8080ff, 0xff80ff, 0x80ffff, 0xffffff},
     0xffffff, 0x000000, 0xffffff, 0xffff00},

    {"solarized_dark", "Solarized 暗",
     {0x073642, 0xdc322f, 0x859900, 0xb58900, 0x268bd2, 0xd33682, 0x2aa198, 0xeee8d5, 0x002b36,
      0xcb4b16, 0x586e75, 0x657b83, 0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3},
     0x839496, 0x073642, 0x93a1a1, 0x2aa198},

    {"gruvbox_dark", "Gruvbox 暗",
     {0x282828, 0xcc241d, 0x98971a, 0xd79921, 0x458588, 0xb16286, 0x689d6a, 0xa89984, 0x928374,
      0xfb4934, 0xb8bb26, 0xfabd2f, 0x83a598, 0xd3869b, 0x8ec07c, 0xebdbb2},
     0xebdbb2, 0x3c3836, 0xebdbb2, 0xfabd2f},

    {"nord", "Nord",
     {0x3b4252, 0xbf616a, 0xa3be8c, 0xebcb8b, 0x81a1c1, 0xb48ead, 0x88c0d0, 0xe5e9f0, 0x4c566a,
      0xbf616a, 0xa3be8c, 0xebcb8b, 0x81a1c1, 0xb48ead, 0x8fbcbb, 0xeceff4},
     0xd8dee9, 0x3b4252, 0xd8dee9, 0x88c0d0},

    {"dracula", "Dracula",
     {0x21222c, 0xff5555, 0x50fa7b, 0xf1fa8c, 0xbd93f9, 0xff79c6, 0x8be9fd, 0xf8f8f2, 0x6272a4,
      0xff6e6e, 0x69ff94, 0xffffa5, 0xd6acff, 0xff92df, 0xa4ffff, 0xffffff},
     0xf8f8f2, 0x44475a, 0xf8f8f2, 0xff79c6},
};

constexpr size_t kThemeCount = sizeof kThemes / sizeof *kThemes;

size_t g_current = 0;

}  // namespace

const TermTheme *theme_table(size_t &count) {
    count = kThemeCount;
    return kThemes;
}

const TermTheme &theme_by_id(const std::string &id) {
    for(size_t i = 0; i < kThemeCount; ++i)
        if(id == kThemes[i].id) return kThemes[i];
    return kThemes[0];
}

const TermTheme &theme_current() { return kThemes[g_current]; }

size_t theme_index() { return g_current; }

void theme_set_index(size_t i) { g_current = i < kThemeCount ? i : 0; }

void theme_set_by_id(const std::string &id) {
    for(size_t i = 0; i < kThemeCount; ++i) {
        if(id == kThemes[i].id) {
            g_current = i;
            return;
        }
    }
    g_current = 0;
}

}  // namespace term
