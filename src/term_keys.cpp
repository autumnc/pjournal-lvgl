#include "term_keys.h"

namespace term {

std::string bytes_for(int key, bool app_cursor_keys) {
    if(key <= 0) return std::string();

    // 这几个的码位落在字节区间里,但含义和「原样发这个字节」不一样,先挑出来。
    if(key == 8) return std::string(1, '\x7f');  // 终端的 VERASE 是 0x7f,不是 8
    if(key == 127) return "\033[3~";             // Delete
    if(key == '\n') return "\r";                 // Enter 发 CR(pty 那边的 ICRNL 会转成 NL)
    if(key < 0x80) return std::string(1, (char)key);

    switch(key) {
    case KEY_UP: return app_cursor_keys ? "\033OA" : "\033[A";
    case KEY_DOWN: return app_cursor_keys ? "\033OB" : "\033[B";
    case KEY_RIGHT: return app_cursor_keys ? "\033OC" : "\033[C";
    case KEY_LEFT: return app_cursor_keys ? "\033OD" : "\033[D";
    // Shift+左右没有回看用途,当普通方向键发下去,别变成哑键。
    case KEY_SHIFT_RIGHT: return app_cursor_keys ? "\033OC" : "\033[C";
    case KEY_SHIFT_LEFT: return app_cursor_keys ? "\033OD" : "\033[D";
    case KEY_HOME: return app_cursor_keys ? "\033OH" : "\033[H";
    case KEY_END: return app_cursor_keys ? "\033OF" : "\033[F";
    case KEY_PAGE_UP: return "\033[5~";
    case KEY_PAGE_DOWN: return "\033[6~";

    case KEY_CTRL_I: return "\t";
    case KEY_CTRL_ENTER: return "\r";

    // 输入法没开(或没组字)的时候 Shift+空格就是个空格,别变成哑键。
    case KEY_FULLWIDTH_TOGGLE: return " ";
    // Ctrl+/ 在 xterm 里就是 0x1f(readline 的 undo)。
    case KEY_SEARCH: return std::string(1, '\x1f');

    case KEY_FILE_BASE + 1: return "\033OP";     // F1
    case KEY_FILE_BASE + 2: return "\033OQ";     // F2
    case KEY_FILE_BASE + 3: return "\033OR";     // F3
    case KEY_FILE_BASE + 4: return "\033OS";     // F4
    case KEY_FILE_BASE + 5: return "\033[15~";   // F5
    case KEY_FILE_BASE + 6: return "\033[17~";   // F6
    case KEY_FILE_BASE + 7: return "\033[18~";   // F7
    case KEY_FILE_BASE + 8: return "\033[19~";   // F8
    case KEY_FILE_BASE + 9: return "\033[20~";   // F9

    default: break;
    }
    // F10(菜单)、回看、背光、输入法开关这些不进 pty。
    return std::string();
}

int scroll_lines(int key, int rows) {
    (void)rows;
    switch(key) {
    case KEY_SHIFT_UP: return 1;
    case KEY_SHIFT_DOWN: return -1;
    default: return 0;
    }
}

}  // namespace term
