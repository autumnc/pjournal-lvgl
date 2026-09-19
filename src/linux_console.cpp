#include "linux_console.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/keyboard.h>
#include <linux/vt.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int s_tty_fd = -1;
static bool s_have_termios = false;
static termios s_old_termios {};
static int s_old_kb_mode = K_XLATE;
static bool s_have_kb_mode = false;
static bool s_graphics = false;

static bool is_linux_console(int fd) {
    int mode = 0;
    return ioctl(fd, KDGETMODE, &mode) == 0;
}

static std::string active_vt_path() {
    const char *env = getenv("PJOURNAL_TTY");
    if(env && *env) return env;

    int ctty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if(ctty >= 0) {
        if(is_linux_console(ctty)) {
            close(ctty);
            return "/dev/tty";
        }
        close(ctty);
    }

    int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if(fd >= 0) {
        vt_stat st {};
        if(ioctl(fd, VT_GETSTATE, &st) == 0 && st.v_active > 0) {
            close(fd);
            return "/dev/tty" + std::to_string(st.v_active);
        }
        close(fd);
    }
    return "/dev/tty1";
}

void linux_console_restore() {
    if(s_tty_fd >= 0) {
        if(s_have_kb_mode) ioctl(s_tty_fd, KDSKBMODE, s_old_kb_mode);
        // 这里必须无条件切回 KD_TEXT,不能恢复启动时读到的那个模式。若读到的本来就是
        // KD_GRAPHICS,说明上一轮 app 没能正常收尾(被 SIGKILL、掉电),照抄回去就是
        // 把「冻在最后一帧、键盘全进隐身 shell」的状态一代代传下去。切回文字模式后
        // 还要再写一次,内核才会重画控制台;否则屏幕上一直留着 app 的最后一帧,看着
        // 像还在跑,会被当成「键盘没反应」。
        if(s_graphics) {
            ioctl(s_tty_fd, KDSETMODE, KD_TEXT);
            const char *show = "\033[?25h\033[0m\033[2J\033[H[pjournal-lvgl exited]\n";
            write(s_tty_fd, show, strlen(show));
        }
        if(s_have_termios) tcsetattr(s_tty_fd, TCSANOW, &s_old_termios);
        close(s_tty_fd);
        s_tty_fd = -1;
    }
    s_have_termios = false;
    s_have_kb_mode = false;
    s_graphics = false;
}

static void signal_restore(int sig) {
    linux_console_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

bool linux_console_enter_graphics() {
    if(s_tty_fd >= 0) return true;

    std::string tty = active_vt_path();
    s_tty_fd = open(tty.c_str(), O_RDWR | O_CLOEXEC);
    if(s_tty_fd < 0) s_tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if(s_tty_fd < 0) s_tty_fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if(s_tty_fd < 0) return false;

    if(tcgetattr(s_tty_fd, &s_old_termios) == 0) {
        s_have_termios = true;
        termios raw = s_old_termios;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG);
        raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
        raw.c_oflag &= (tcflag_t)~(OPOST);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(s_tty_fd, TCSANOW, &raw);
    }

    const char *hide_clear = "\033[?25l\033[2J\033[H";
    write(s_tty_fd, hide_clear, strlen(hide_clear));
    int kb_mode = K_XLATE;
    if(ioctl(s_tty_fd, KDGKBMODE, &kb_mode) == 0) {
        s_old_kb_mode = kb_mode;
        s_have_kb_mode = true;
    }
    if(ioctl(s_tty_fd, KDSETMODE, KD_GRAPHICS) == 0) s_graphics = true;

    int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if(nullfd >= 0) {
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
        if(nullfd > STDERR_FILENO) close(nullfd);
    }

    static bool registered = false;
    if(!registered) {
        atexit(linux_console_restore);
        signal(SIGINT, signal_restore);
        signal(SIGTERM, signal_restore);
        signal(SIGSEGV, signal_restore);
        registered = true;
    }
    return true;
}
