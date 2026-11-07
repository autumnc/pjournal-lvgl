#pragma once

// PTY:起一个 shell 挂在伪终端上,主循环读它的输出喂给 VT 解析器、把键盘字节写回去。
//
// 为什么是 fork 而不是 posix_spawn:posix_spawn 没有「先 setsid 再 TIOCSCTTY」这个
// 组合,而那正是让 pty 成为子进程**控制终端**的必要条件 —— 少了它,Ctrl+C 不会变成
// SIGINT 送到前台进程组,vim/top 这类程序一个都跑不起来。
//
// fork 的代价是「子进程里不能碰分配器」。所以 argv/envp 在父进程里建好,子进程那段
// 只用 async-signal-safe 的系统调用(execve 而不是 execvp,免得 PATH 搜索去 malloc)。
// pjournal 是多线程进程(输入法/同步),因此这个约束是硬要求。

#include <sys/types.h>

#include <string>

namespace term {

class Pty {
public:
    Pty() = default;
    ~Pty();
    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;

    bool spawn(const std::string &shell, int cols, int rows, const std::string &home,
               const std::string &term_env, const std::string &locale);
    void close();

    bool valid() const { return _master >= 0; }
    int master() const { return _master; }
    pid_t pid() const { return _pid; }
    bool child_exited() const { return _child_exited; }
    bool shell_missing() const { return _exec_failed; }

    // 把 shell 的输出读干净追加到 out。返回 false 表示对端没了(EOF/EIO)或出错,
    // 主循环这时该收摊。每次调用最多读 kMaxReadPerCall,防止刷屏输出把 UI 卡住。
    bool read(std::string &out);

    // 写。一次写不完的(pty 缓冲满)排进队列,主循环每圈 flush() 一次。
    void write(const std::string &data);
    void flush();

    // 窗口尺寸变了:通知内核 + 给前台进程组发 SIGWINCH。
    void set_winsize(int cols, int rows);

private:
    static constexpr size_t kMaxPending = 1u << 16;      // 对端不读了也不能无限涨
    static constexpr size_t kMaxReadPerCall = 1u << 18;  // 每帧上限 256KB

    int _master = -1;
    pid_t _pid = -1;
    std::string _pending;
    bool _child_exited = false;
    bool _exec_failed = false;
};

}  // namespace term
