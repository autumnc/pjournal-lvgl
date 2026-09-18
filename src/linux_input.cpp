#include "linux_input.h"

#include <lvgl.h>

#include <cstdint>
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

struct InputState {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool left_shift_down = false;
    bool left_shift_used = false;
};

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
        if(code >= KEY_0 && code <= KEY_9) {
            int n = code == KEY_0 ? 0 : code - KEY_1 + 1;
            return APP_KEY_FILE_BASE + n;
        }
        if(code >= KEY_A && code <= KEY_Z) return code - KEY_A + 1;
    }
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
    case KEY_SPACE: return ' ';
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
    if(code >= KEY_A && code <= KEY_Z) {
        int ch = 'a' + code - KEY_A;
        return st.shift ? ch - 32 : ch;
    }
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
            dispatch_key(ch);
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

static void modifier_tap_thread(std::string device) {
    int fd = open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) return;
    bool left_down = false;
    bool used = false;
    input_event ev {};
    while(read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if(ev.type != EV_KEY) continue;
        bool down = ev.value != 0;
        if(ev.code == KEY_LEFTSHIFT) {
            if(down) {
                left_down = true;
                used = false;
            } else {
                if(left_down && !used) dispatch_key(APP_KEY_LSHIFT_TAP);
                left_down = false;
            }
            continue;
        }
        if(left_down && down) used = true;
    }
    close(fd);
}

bool linux_input_start_modifier_taps(const std::string &device) {
    std::thread(modifier_tap_thread, device).detach();
    return true;
}
