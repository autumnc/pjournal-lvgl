#pragma once

// 设置界面的配置。**这个文件不依赖 LVGL**,所以能在宿主机上直接编来测
// (见 tests/settings_test.cpp)。
//
// 一个 key=value 一行的文本文件,放在 $PJOURNAL_TERMINAL_CONFIG 或
// ~/.pjournal-lvgl/settings/terminal.conf。
// 能上手改 —— 界面里调不出来的(或者改坏了的)用编辑器修。
//
// 取值优先级:**默认值 < 配置文件**。

#include <string>

namespace term {

struct TermSettings {
    // 正文。空 = $HOME/.fonts/Go-LavaPropo-NF-R.ttf。
    std::string font_path;
    // 粗体/斜体单独的字面。空 = 用正文那份合成。
    std::string font_bold_path;
    std::string font_italic_path;

    // 30 号在 1024x600 上约 68 列 x 20 行;界面 20 号给候选条/状态/设置。
    int font_size = 30;
    int ui_font_size = 20;  // 0 = 自动(正文的 3/5)

    std::string theme_id = "typix";

    int cursor_style = 0;  // 0=块 1=下划线 2=竖线
    bool cursor_blink = true;

    int scrollback = 1000;  // 回看行数
};

// 全局那一份。主程序启动时 settings_load() 填好,之后设置界面直接改它。
TermSettings &term_settings();

// $PJOURNAL_TERMINAL_CONFIG,没有就是 $HOME/.pjournal-lvgl/settings/terminal.conf。
std::string settings_path();

// 从文件读。**先把结构体复位成默认值再套文件里的键**,所以没写到的键就是默认值,
// 不是上一次加载残留的。文件不存在/读不动不算错,返回 false。
bool settings_load(const std::string &path);

// 写回。走 safe_file 的 .tmp/.bak 原子替换(设备是闪存,断电不能写坏)。
bool settings_save(const std::string &path);

}  // namespace term
