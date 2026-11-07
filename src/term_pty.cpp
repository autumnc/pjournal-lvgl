#include "term_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <string>
#include <vector>

extern char **environ;

namespace term {

namespace {

// 子进程里只能跑 async-signal-safe 的调用。这里把要传给 execve 的环境在父进程里
// 备好:多线程进程 fork 之后,子进程里连 setenv/malloc 都不能碰(会去抢别的线程可能
// 持着的锁)。
std::vector<std::string> build_env(const std::string &home, const std::string &term_env,
                                   const std::string &locale) {
    std::vector<std::string> env;
    for(char **e = environ; e && *e; ++e) {
        const char *kv = *e;
        if(strncmp(kv, "TERM=", 5) == 0 || strncmp(kv, "COLORTERM=", 10) == 0 ||
           strncmp(kv, "HOME=", 5) == 0 || strncmp(kv, "LANG=", 5) == 0 ||
           strncmp(kv, "LC_ALL=", 7) == 0)
            continue;
        env.emplace_back(kv);
    }
    env.push_back("TERM=" + term_env);
    env.push_back("COLORTERM=truecolor");  // 真彩应用靠这个判断能不能用 38;2;r;g;b
    if(!home.empty()) env.push_back("HOME=" + home);
    env.push_back("LANG=" + locale);
    env.push_back("LC_ALL=" + locale);
    return env;
}

void reset_child_signals() {
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);

    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    for(int sig : {SIGPIPE, SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT}) sigaction(sig, &sa, nullptr);
}

}  // namespace

Pty::~Pty() {
    close();
}

bool Pty::spawn(const std::string &shell, int cols, int rows, const std::string &home,
                const std::string &term_env, const std::string &locale) {
    close();

    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if(master < 0) return false;
    if(grantpt(master) != 0 || unlockpt(master) != 0) {
        ::close(master);
        return false;
    }

    char slave_path[256];
    if(ptsname_r(master, slave_path, sizeof(slave_path)) != 0) {
        ::close(master);
        return false;
    }

    // 窗口尺寸要在 fork 之前设好:子进程 exec 出来的 shell 一启动就要读 TIOCGWINSZ
    // 算 COLUMNS/LINES,晚一步第一个提示符的折行位置就是错的。
    struct winsize ws {};
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);
    ioctl(master, TIOCSWINSZ, &ws);

    // IUTF8:没有它 canonical 模式下的退格只删一个**字节**,汉字会剩下半个残码。
    // 终端里打中文这是必须的。
    struct termios tio {};
    if(tcgetattr(master, &tio) == 0) {
        tio.c_iflag |= IUTF8;
        tcsetattr(master, TCSANOW, &tio);
    }

    // exec 失败要能报出来。O_CLOEXEC 让管道的写端在 exec 成功时自动关闭,父进程那边
    // read 立刻拿到 EOF;失败时子进程往里写一个 errno,父进程读到就知道是哪个错了。
    int errpipe[2] = {-1, -1};
    if(pipe2(errpipe, O_CLOEXEC) != 0) {
        ::close(master);
        return false;
    }

    // argv/envp 必须在 fork 之前建好:fork 之后子进程里不能再分配,也不能 setenv。
    const std::vector<std::string> env = build_env(home, term_env, locale);
    std::vector<char *> envp;
    envp.reserve(env.size() + 1);
    for(const auto &s : env) envp.push_back(const_cast<char *>(s.c_str()));
    envp.push_back(nullptr);

    char *argv[2] = {const_cast<char *>(shell.c_str()), nullptr};

    const pid_t pid = fork();
    if(pid < 0) {
        ::close(master);
        ::close(errpipe[0]);
        ::close(errpipe[1]);
        return false;
    }

    if(pid == 0) {
        // ---- 子进程 ----
        // 这里只调用 async-signal-safe 的系统调用。pjournal 是多线程进程,这些线程
        // 可能在 fork 的瞬间正持着 malloc/stdio 的锁 —— 任何一次分配都会死锁。
        ::close(errpipe[0]);
        setsid();

        const int slave = open(slave_path, O_RDWR);
        if(slave < 0) {
            const int e = errno;
            (void)!::write(errpipe[1], &e, sizeof(e));
            _exit(127);
        }
        // 抢在 open 之后立刻认领控制终端:中间任何一次 open tty 都可能被别人抢先。
        ioctl(slave, TIOCSCTTY, 0);

        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if(slave > 2) ::close(slave);
        ::close(master);

        reset_child_signals();

        // 设备内核是 3.10,没有 close_range;固定上限扫一遍,免得把 master 之类
        // 从父进程继承来的 fd 漏给 shell。errpipe[1] 留着报 exec 失败。
        for(int fd = 3; fd < 1024; ++fd)
            if(fd != errpipe[1]) ::close(fd);

        if(!home.empty()) chdir(home.c_str());

        execve(shell.c_str(), argv, envp.data());

        const int e = errno;
        (void)!::write(errpipe[1], &e, sizeof(e));
        _exit(127);
    }

    // ---- 父进程 ----
    ::close(errpipe[1]);
    _pid = pid;
    _master = master;
    _child_exited = false;
    _exec_failed = false;
    _pending.clear();

    int child_errno = 0;
    const ssize_t got = ::read(errpipe[0], &child_errno, sizeof(child_errno));
    ::close(errpipe[0]);
    if(got == (ssize_t)sizeof(child_errno)) {
        // execv 没成功。把子进程收掉,免得留个僵尸。
        int st = 0;
        (void)waitpid(_pid, &st, 0);
        _pid = -1;
        _child_exited = true;
        _exec_failed = true;
        ::close(_master);
        _master = -1;
        errno = child_errno;
        return false;
    }

    const int fl = fcntl(_master, F_GETFL, 0);
    if(fl >= 0) fcntl(_master, F_SETFL, fl | O_NONBLOCK);
    return true;
}

void Pty::close() {
    if(_master < 0) {
        _pending.clear();
        return;
    }
    _pending.clear();

    if(_pid > 0 && !_child_exited) {
        // 关终端 = 挂断。进程组整个一起收(setsid 之后 pgrp == pid,前台任务也在里面)。
        kill(-_pid, SIGHUP);
        for(int i = 0; i < 20; ++i) {
            int st = 0;
            if(waitpid(_pid, &st, WNOHANG) == _pid) {
                _child_exited = true;
                break;
            }
            usleep(10000);  // 最多等 200ms
        }
        if(!_child_exited) kill(-_pid, SIGKILL);
    }

    ::close(_master);
    _master = -1;
    _pid = -1;
}

bool Pty::read(std::string &out) {
    if(_master < 0) return false;

    char buf[8192];
    size_t total = 0;
    while(total < kMaxReadPerCall) {
        const ssize_t n = ::read(_master, buf, sizeof(buf));
        if(n > 0) {
            out.append(buf, (size_t)n);
            total += (size_t)n;
            continue;
        }
        if(n == 0) {
            _child_exited = true;
            return false;
        }
        if(errno == EINTR) continue;
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            // 顺手收一下僵尸,主循环就不用单独管 waitpid
            int st = 0;
            if(_pid > 0 && waitpid(_pid, &st, WNOHANG) == _pid) _child_exited = true;
            return true;
        }
        // EIO 是 pty 的「对端全关了」,和 EOF 一个意思
        _child_exited = true;
        return false;
    }
    // 摸到每帧上限了:对端可能还在刷(比如 yes),剩下的留给下一圈,
    // 不然 UI 会一直卡在这里读。
    return true;
}

void Pty::write(const std::string &data) {
    if(_master < 0 || data.empty()) return;
    _pending += data;
    flush();
}

void Pty::flush() {
    if(_master < 0 || _pending.empty()) return;

    size_t off = 0;
    while(off < _pending.size()) {
        const ssize_t n = ::write(_master, _pending.data() + off, _pending.size() - off);
        if(n > 0) {
            off += (size_t)n;
            continue;
        }
        if(n < 0 && errno == EINTR) continue;
        break;  // EAGAIN / 出错:剩下的留给下一圈
    }
    if(off > 0) _pending.erase(0, off);
    if(_pending.size() > kMaxPending) _pending.erase(0, _pending.size() - kMaxPending);
}

void Pty::set_winsize(int cols, int rows) {
    if(_master < 0) return;
    struct winsize ws {};
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);
    ioctl(_master, TIOCSWINSZ, &ws);
    // TIOCSWINSZ 只改内核里的记录,不会替我们发信号(那是 pty 主设备驱动给从设备
    // 前台组发的,方向反了),所以得自己补一发。
    if(_pid > 0 && !_child_exited) kill(-_pid, SIGWINCH);
}

}  // namespace term
