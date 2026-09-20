#include "linux_input.h"

#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

extern "C" void app_ui_handle_key(int key);
extern "C" void app_ui_request_quit();
extern "C" bool app_ui_is_main_screen();

static constexpr int APP_KEY_UP = 0x80;
static constexpr int APP_KEY_DOWN = 0x81;
static constexpr int APP_KEY_LEFT = 0x82;
static constexpr int APP_KEY_RIGHT = 0x83;
static constexpr int APP_KEY_IME_TOGGLE = 0x84;
static constexpr int APP_KEY_CTRL_ENTER = 0x85;
static constexpr int APP_KEY_SHIFT_UP = 0x86;
static constexpr int APP_KEY_SHIFT_DOWN = 0x87;
static constexpr int APP_KEY_SHIFT_LEFT = 0x88;
static constexpr int APP_KEY_SHIFT_RIGHT = 0x89;
static constexpr int APP_KEY_CTRL_I = 0x8A;
static constexpr int APP_KEY_FULLWIDTH_TOGGLE = 0x8B;
static constexpr int APP_KEY_TRAD_TOGGLE = 0x8C;
static constexpr int APP_KEY_LSHIFT_TAP = 0x8D;
static constexpr int APP_KEY_HOME = 0x8E;
static constexpr int APP_KEY_END = 0x8F;
static constexpr int APP_KEY_FILE_BASE = 0x90;
static constexpr int APP_KEY_PAGE_UP = 0xA0;
static constexpr int APP_KEY_PAGE_DOWN = 0xA1;
static constexpr int APP_KEY_SEARCH = 0xA2;
static constexpr int APP_KEY_HELP = 0xA3;
static constexpr int APP_KEY_REDO = 0xA4;
static constexpr int APP_KEY_IM_SWITCH = 0xA5;

struct InputState {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool left_shift_down = false;
    bool left_shift_used = false;
};

// 控制台这条路上,tty 只送出按 K_XLATE 键表翻译后的字节,修饰键的信息到这里就丢了:
// Shift+Space 和裸空格一样是 0x20,Ctrl+Shift+F 和 Ctrl+F 一样是 0x06。所以另开线程直接
// 读输入设备,把修饰键状态记下来,补发那几个 tty 表达不出来的组合键,同时让 tty 线程把
// 重复的那一个字节丢掉。两个线程共享这几个标志,必须用原子量。
static std::atomic<bool> g_shift_held{false};
static std::atomic<bool> g_ctrl_held{false};
static std::atomic<bool> g_modifier_tap_live{false};

static bool console_byte_swallowed(uint8_t ch) {
    if(!g_modifier_tap_live.load() || !g_shift_held.load()) return false;
    if(ch == ' ') return true;
    return g_ctrl_held.load() && (ch == 0x06 || ch == 0x1A || ch == 0x1F);
}

// 快捷编辑文件槽位:F1→1.txt … F9→9.txt,F10→0.txt。入参是 1..10 的键序号。
static int fkey_file_slot(int fkey_no) { return fkey_no % 10; }

static int shifted_digit(int code) {
    switch(code) {
    case KEY_1: return '!';
    case KEY_2: return '@';
    case KEY_3: return '#';
    case KEY_4: return '$';
    case KEY_5: return '%';
    case KEY_6: return '^';
    case KEY_7: return '&';
    case KEY_8: return '*';
    case KEY_9: return '(';
    case KEY_0: return ')';
    default: return 0;
    }
}

// 字母键在 input 码里是分三段排的(Q..P 是 16..25,A..L 是 30..38,Z..M 是 44..50),
// 按 KEY_A 等差推出来的字符是错的,必须查表。
static int letter_of(int code) {
    static const char qrow[] = "qwertyuiop";
    static const char arow[] = "asdfghjkl";
    static const char zrow[] = "zxcvbnm";
    if(code >= KEY_Q && code <= KEY_P) return qrow[code - KEY_Q];
    if(code >= KEY_A && code <= KEY_L) return arow[code - KEY_A];
    if(code >= KEY_Z && code <= KEY_M) return zrow[code - KEY_Z];
    return 0;
}

static int map_key(int code, const InputState &st) {
    if(st.ctrl) {
        if(code == KEY_SPACE) return APP_KEY_IME_TOGGLE;
        if(code == KEY_ENTER) return APP_KEY_CTRL_ENTER;
        if(code == KEY_I) return APP_KEY_CTRL_I;
        if(code == KEY_SLASH) return APP_KEY_SEARCH;
        if(code == KEY_MINUS) return APP_KEY_FULLWIDTH_TOGGLE;
        if(code == KEY_F) return st.shift ? APP_KEY_TRAD_TOGGLE : 0x06;
        if(code == KEY_Z) return st.shift ? APP_KEY_REDO : 0x1A;
        if(code == KEY_QUESTION || (st.shift && code == KEY_SLASH)) return APP_KEY_HELP;
        int c = letter_of(code);
        if(c) return c - 'a' + 1;
    }
    // F1-F10 直接对应快捷编辑的 10 个文件(F1→1.txt … F9→9.txt,F10→0.txt)。控制台模式下
    // 真键盘的 F 键由 tty 送出字符串序列(见 parse_csi_key),这里覆盖 PJOURNAL_INPUT 那条纯 evdev 路。
    if(!st.ctrl && !st.alt && code >= KEY_F1 && code <= KEY_F10)
        return APP_KEY_FILE_BASE + fkey_file_slot(code - KEY_F1 + 1);
    switch(code) {
    case KEY_UP: return st.shift ? APP_KEY_SHIFT_UP : APP_KEY_UP;
    case KEY_DOWN: return st.shift ? APP_KEY_SHIFT_DOWN : APP_KEY_DOWN;
    case KEY_LEFT: return st.shift ? APP_KEY_SHIFT_LEFT : APP_KEY_LEFT;
    case KEY_RIGHT: return st.shift ? APP_KEY_SHIFT_RIGHT : APP_KEY_RIGHT;
    case KEY_HOME: return APP_KEY_HOME;
    case KEY_END: return APP_KEY_END;
    case KEY_PAGEUP: return APP_KEY_PAGE_UP;
    case KEY_PAGEDOWN: return APP_KEY_PAGE_DOWN;
    case KEY_ESC: return 27;
    case KEY_BACKSPACE: return 8;
    case KEY_DELETE: return 127;
    case KEY_ENTER: return '\n';
    case KEY_TAB: return '\t';
    case KEY_SPACE: return st.shift ? APP_KEY_FULLWIDTH_TOGGLE : ' ';
    case KEY_MINUS: return st.shift ? '_' : '-';
    case KEY_EQUAL: return st.shift ? '+' : '=';
    case KEY_LEFTBRACE: return st.shift ? '{' : '[';
    case KEY_RIGHTBRACE: return st.shift ? '}' : ']';
    case KEY_BACKSLASH: return st.shift ? '|' : '\\';
    case KEY_SEMICOLON: return st.shift ? ':' : ';';
    case KEY_APOSTROPHE: return st.shift ? '"' : '\'';
    case KEY_GRAVE: return st.shift ? '~' : '`';
    case KEY_COMMA: return st.shift ? '<' : ',';
    case KEY_DOT: return st.shift ? '>' : '.';
    case KEY_SLASH: return st.shift ? '?' : '/';
    default: break;
    }
    int ch = letter_of(code);
    if(ch) return st.shift ? ch - 32 : ch;
    if(code >= KEY_1 && code <= KEY_0) {
        int shifted = shifted_digit(code);
        if(st.shift && shifted) return shifted;
        if(code == KEY_0) return '0';
        return '1' + code - KEY_1;
    }
    return 0;
}

static void dispatch_key_async(void *p) {
    int key = (int)(intptr_t)p;
    app_ui_handle_key(key);
}

static void dispatch_key(int key) {
    if(key) lv_async_call(dispatch_key_async, (void *)(intptr_t)key);
}

static bool read_byte(int fd, uint8_t &ch) {
    ssize_t n = read(fd, &ch, 1);
    return n == 1;
}

static int parse_csi_key(const std::string &seq) {
    if(seq == "[A") return APP_KEY_UP;
    if(seq == "[B") return APP_KEY_DOWN;
    if(seq == "[C") return APP_KEY_RIGHT;
    if(seq == "[D") return APP_KEY_LEFT;
    if(seq == "[H" || seq == "[1~") return APP_KEY_HOME;
    if(seq == "[F" || seq == "[4~") return APP_KEY_END;
    if(seq == "[5~") return APP_KEY_PAGE_UP;
    if(seq == "[6~") return APP_KEY_PAGE_DOWN;
    if(seq == "[3~") return 127;
    if(seq == "[1;2A") return APP_KEY_SHIFT_UP;
    if(seq == "[1;2B") return APP_KEY_SHIFT_DOWN;
    if(seq == "[1;2C") return APP_KEY_SHIFT_RIGHT;
    if(seq == "[1;2D") return APP_KEY_SHIFT_LEFT;
    // F 键:keymap 把 F1-F5 定成 \033[[A..\033[[E,F6-F10 定成 \033[17~..\033[21~。
    // 不认的话会掉进下面的 fallback 变成裸 Esc(在主页上等于打开设置)。
    if(seq.size() == 3 && seq[0] == '[' && seq[1] == '[' && seq[2] >= 'A' && seq[2] <= 'E')
        return APP_KEY_FILE_BASE + fkey_file_slot(seq[2] - 'A' + 1);
    if(seq.size() >= 3 && seq[0] == '[' && seq.back() == '~') {
        int n = atoi(seq.c_str() + 1);
        if(n >= 17 && n <= 21) return APP_KEY_FILE_BASE + fkey_file_slot(n - 17 + 6);
    }
    return 0;
}

static int open_console_input_fd() {
    int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if(fd >= 0) {
        int mode = 0;
        if(ioctl(fd, KDGETMODE, &mode) == 0) return fd;
        close(fd);
    }
    fd = open("/dev/tty1", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if(fd >= 0) {
        int mode = 0;
        if(ioctl(fd, KDGETMODE, &mode) == 0) return fd;
        close(fd);
    }
    return -1;
}

static void console_input_thread() {
    int fd = open_console_input_fd();
    if(fd < 0) return;

    while(true) {
        uint8_t ch = 0;
        ssize_t n = read(fd, &ch, 1);
        if(n != 1) {
            usleep(5000);
            continue;
        }

        if(ch == 0) {
            dispatch_key(APP_KEY_IME_TOGGLE);
            continue;
        }
        if(ch == 0x7f) {
            dispatch_key(8);
            continue;
        }
        if(ch != 0x1b) {
            if(!console_byte_swallowed(ch)) dispatch_key(ch);
            continue;
        }

        std::string seq;
        for(int i = 0; i < 8; ++i) {
            usleep(1000);
            uint8_t next = 0;
            if(!read_byte(fd, next)) break;
            seq.push_back((char)next);
            if((next >= 'A' && next <= 'Z') || next == '~') break;
        }
        int key = parse_csi_key(seq);
        dispatch_key(key ? key : 27);
    }
}

bool linux_input_start_console() {
    int fd = open_console_input_fd();
    if(fd < 0) return false;
    close(fd);
    std::thread(console_input_thread).detach();
    return true;
}

static void input_thread(std::string device) {
    int fd = open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) return;
    InputState st;
    input_event ev {};
    while(read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if(ev.type != EV_KEY) continue;
        bool down = ev.value != 0;
        if(ev.code == KEY_LEFTSHIFT) {
            if(down) {
                st.left_shift_down = true;
                st.left_shift_used = false;
                st.shift = true;
            } else {
                if(st.left_shift_down && !st.left_shift_used) dispatch_key(APP_KEY_LSHIFT_TAP);
                st.left_shift_down = false;
                st.shift = false;
            }
            continue;
        }
        if(ev.code == KEY_RIGHTSHIFT) { st.shift = down; continue; }
        if(ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) { st.ctrl = down; continue; }
        if(ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) { st.alt = down; continue; }
        if(!down) continue;
        if(st.left_shift_down) st.left_shift_used = true;
        if(!st.ctrl && !st.alt && ev.code == KEY_Q && app_ui_is_main_screen()) {
            app_ui_request_quit();
            continue;
        }
        int key = map_key(ev.code, st);
        dispatch_key(key);
    }
    close(fd);
}

bool linux_input_start(const std::string &device) {
    std::thread(input_thread, device).detach();
    return true;
}

// 控制台模式下普通按键由 tty 负责(它连键表翻译一起送来了),这里只补 tty 表达不出来
// 的那几个组合键,以及单独的左 Shift 轻点。非键盘设备不含这些键位,读了也不会动作。
static void console_modifier_thread(std::string device) {
    int fd = open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) return;
    g_modifier_tap_live = true;
    bool left_down = false;
    bool used = false;
    // Ctrl+Shift 轻点切输入法方案。combo_used 记的是「这一对按住期间按过第三个键」,
    // 有的话就是 Ctrl+Shift+F/Z// 那类组合,不能再当成轻点。
    bool combo_used = false;
    input_event ev {};
    while(read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if(ev.type != EV_KEY) continue;
        bool down = ev.value != 0;
        if(ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            if(ev.code == KEY_LEFTSHIFT) {
                if(down) {
                    left_down = true;
                    used = false;
                } else {
                    if(left_down && !used) {
                        if(g_ctrl_held.load()) {
                            if(!combo_used) dispatch_key(APP_KEY_IM_SWITCH);
                        } else {
                            dispatch_key(APP_KEY_LSHIFT_TAP);
                        }
                    }
                    left_down = false;
                }
            }
            if(down && g_ctrl_held.load()) combo_used = false;
            g_shift_held = down;
            continue;
        }
        if(ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
            if(down && g_shift_held.load()) combo_used = false;
            // 先松 Ctrl 也算轻点,但要把 used 立起来,免得随后松 Shift 又发一次
            else if(!down && g_shift_held.load() && !combo_used) {
                dispatch_key(APP_KEY_IM_SWITCH);
                used = true;
            }
            g_ctrl_held = down;
            continue;
        }
        if(!down) continue;
        if(left_down) used = true;
        if(g_ctrl_held.load() && g_shift_held.load()) combo_used = true;
        if(g_ctrl_held && g_shift_held) {
            if(ev.code == KEY_F) dispatch_key(APP_KEY_TRAD_TOGGLE);
            else if(ev.code == KEY_Z) dispatch_key(APP_KEY_REDO);
            else if(ev.code == KEY_SLASH || ev.code == KEY_QUESTION) dispatch_key(APP_KEY_HELP);
            continue;
        }
        // Ctrl+Space 走 tty 的 0x00(输入法开关),别在这里抢
        if(g_shift_held && !g_ctrl_held && ev.code == KEY_SPACE)
            dispatch_key(APP_KEY_FULLWIDTH_TOGGLE);
    }
    close(fd);
}

// 键盘未必挂在 event0(枚举顺序会变),所以整个 /dev/input/event* 都开一遍,开不了就算了。
bool linux_input_start_console_modifiers() {
    for(int i = 0; i < 16; ++i) {
        std::string path = "/dev/input/event" + std::to_string(i);
        if(access(path.c_str(), R_OK) == 0)
            std::thread(console_modifier_thread, path).detach();
    }
    return true;
}
