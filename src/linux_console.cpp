#include "linux_console.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
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

// 还原过程不重入:第二次进来时 s_tty_fd 可能已经被关掉、甚至那个 fd 号已经被别的
// 东西复用了 —— 再 close/ioctl 一次就是误伤。信号来得密集时(先 SIGSEGV 再 SIGABRT)
// 真的会撞上。
static volatile sig_atomic_t s_restoring = 0;

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

// 真正干活的那一段。会从信号上下文里被调用,所以只能用异步信号安全的调用
// (ioctl / write / tcsetattr / close / strlen 都是),不能分配、不能加锁、
// 不能碰 std::string。reason 非空时替代常规的退出提示。
static void console_restore_body(const char *reason) {
    if(s_tty_fd < 0) return;
    if(s_have_kb_mode) ioctl(s_tty_fd, KDSKBMODE, s_old_kb_mode);
    // 这里必须无条件切回 KD_TEXT,不能恢复启动时读到的那个模式。若读到的本来就是
    // KD_GRAPHICS,说明上一轮 app 没能正常收尾(被 SIGKILL、掉电),照抄回去就是
    // 把「冻在最后一帧、键盘全进隐身 shell」的状态一代代传下去。切回文字模式后
    // 还要再写一次,内核才会重画控制台;否则屏幕上一直留着 app 的最后一帧,看着
    // 像还在跑,会被当成「键盘没反应」。
    if(s_graphics) {
        ioctl(s_tty_fd, KDSETMODE, KD_TEXT);
        const char *show = "\033[?25h\033[0m\033[2J\033[H";
        write(s_tty_fd, show, strlen(show));
        if(reason && *reason) {
            const char *tag = "[pjournal-lvgl] ";
            write(s_tty_fd, tag, strlen(tag));
            write(s_tty_fd, reason, strlen(reason));
            write(s_tty_fd, "\n", 1);
        } else {
            const char *bye = "[pjournal-lvgl exited]\n";
            write(s_tty_fd, bye, strlen(bye));
        }
    }
    // termios 同理:不能照抄启动时读到的状态。读到 raw 说明上一轮没正常收尾(被停住的
    // fbterm-mod 之类把 tty 留在 raw 上),照抄回去就是「键盘无回显、回车不换行」一代代
    // 传下去。只保留波特率这些,规范位一律强制回来。
    if(s_have_termios) {
        termios t = s_old_termios;
        t.c_iflag |= ICRNL | IXON;
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(s_tty_fd, TCSANOW, &t);
    }
    close(s_tty_fd);
    s_tty_fd = -1;
}

void linux_console_restore() {
    if(s_restoring) return;
    s_restoring = 1;
    console_restore_body(nullptr);
    s_have_termios = false;
    s_have_kb_mode = false;
    s_graphics = false;
    s_restoring = 0;
}

// 崩溃/异常退出的收尾:先把控制台还给用户,再把原因写上去。
static void console_panic(const char *reason) {
    if(!s_restoring) {
        s_restoring = 1;
        console_restore_body(reason);
    }
    _exit(1);
}

static void signal_restore(int sig) {
    // 还原完再按默认动作死一遍,退出信号保持原样(诊断时有用)。
    if(!s_restoring) {
        s_restoring = 1;
        console_restore_body(nullptr);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// 未捕获的 C++ 异常(某处 substr 抛 std::out_of_range 之类)默认走 terminate → abort,
// 而 abort 没有任何 handler 收尾:控制台就留在 KD_GRAPHICS 上 —— 屏幕冻在最后一帧、
// 键盘全落进看不见的 shell,从用户角度看就是「死机」。这里先还原控制台,再把异常
// 的 what() 写上去,至少留个能查的线索。
static void on_terminate() {
    char msg[256];
    msg[0] = '\0';
    if(std::current_exception()) {
        try {
            throw;
        } catch(const std::exception &e) {
            snprintf(msg, sizeof(msg), "uncaught exception: %s", e.what());
        } catch(...) {
            snprintf(msg, sizeof(msg), "uncaught exception (not a std::exception)");
        }
    } else {
        snprintf(msg, sizeof(msg), "std::terminate");
    }
    console_panic(msg);
}

static void install_signal_handlers() {
    static bool done = false;
    if(done) return;
    done = true;

    // 崩溃类信号的 handler 要跑在**独立的栈**上:栈溢出引起的 SIGSEGV 如果在溢出那条
    // 栈上执行 handler,handler 自己立刻又炸一次,控制台就永远留在 KD_GRAPHICS 上。
    // 别写 SIGSTKSZ —— glibc 2.34 起它不再是编译期常量,file-scope 数组会编不过。
    static char alt_stack[64 * 1024];
    stack_t ss {};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    sigaltstack(&ss, nullptr);

    const int fatal[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    for(int sig : fatal) {
        struct sigaction sa {};
        sa.sa_handler = signal_restore;
        sa.sa_flags = SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
    }
    const int stop[] = {SIGINT, SIGTERM};
    for(int sig : stop) {
        struct sigaction sa {};
        sa.sa_handler = signal_restore;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
    }
    std::set_terminate(on_terminate);
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
        install_signal_handlers();
        registered = true;
    }
    return true;
}
