#pragma once

// pjournal 的 AppKey → 写进 pty 的字节。
//
// pjournal 的 linux_input(.cpp) 已经把 evdev/tty 事件翻译成 AppKey 和字节,
// 这里只负责最后一段:哪些键要变成 VT 序列、哪些键根本不该进 shell。
// 值直接引用 include/key_codes.h,和输入层保持一致。

#include "key_codes.h"

#include <string>

namespace term {

// F1-F9 落在 KEY_FILE_BASE 之后的槽位里;F10 就是 KEY_FILE_BASE 本身,留给终端菜单。
enum : int {
    kKeyMenu = KEY_FILE_BASE,
    kKeyF1 = KEY_FILE_BASE + 1,
    kKeyF9 = KEY_FILE_BASE + 9,
};

// 回看键 → 要滚多少行(正值 = 往更早看)。不是回看键返回 0。
// 到头的两头用一个大数表示,set_view_offset() 自己会夹。
std::string bytes_for(int key, bool app_cursor_keys);
int scroll_lines(int key, int rows);

}  // namespace term
