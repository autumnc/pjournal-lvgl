#include "process_utils.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace process {

namespace {

long long now_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 读 fd 到 EOF,总共最多等 deadline_ms 毫秒;超时返回 false。
// 必须走 poll 而不是裸 read:子进程要是一直把 stdout 攥着不放(卡在网络上、等锁),
// 裸 read 永远不返回,而 run_capture 是在主线程上调的 —— 那就直接冻屏了。
bool read_until_eof(int fd, int timeout_ms, std::string &out) {
    const long long deadline = now_ms() + timeout_ms;
    char buf[1024];
    for(;;) {
        const long long left = deadline - now_ms();
        if(left <= 0) return false;
        struct pollfd p {};
        p.fd = fd;
        p.events = POLLIN;
        const int pr = poll(&p, 1, (int)left);
        if(pr == 0) return false;
        if(pr < 0) {
            if(errno == EINTR) continue;
            return true;
        }
        const ssize_t n = read(fd, buf, sizeof(buf));
        if(n > 0) {
            out.append(buf, (size_t)n);
            continue;
        }
        if(n == 0) return true;
        if(errno == EINTR) continue;
        return true;
    }
}

}  // namespace

std::string run_capture(const std::vector<std::string> &args, int timeout_ms) {
    if(args.empty()) return "";
    int fds[2];
    if(pipe(fds) != 0) return "";

    // 用 posix_spawn 而不是 fork+exec:这是多线程程序,子进程里再 malloc(execvp 和
    // argv 数组都要)一旦撞上别的线程正拿着 malloc 锁就是死等,父进程则卡在 read 上
    // —— 又是一个冻屏来源。posix_spawn 的重定向在内核里做,子进程不碰任何锁。
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // 子进程的 stdin 接 /dev/null:不接就继承 app 的 /dev/tty,一个会读 stdin 的子命令
    // 能把键盘字节抢走,表现成「按键失灵」。
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    // dup2 之后再关:别把刚 dup 过去的那两个标准 fd 关掉。
    if(fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO)
        posix_spawn_file_actions_addclose(&fa, fds[1]);
    if(fds[0] != STDIN_FILENO) posix_spawn_file_actions_addclose(&fa, fds[0]);

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for(const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if(rc != 0) {
        close(fds[0]);
        return "";
    }

    std::string out;
    const bool eof = read_until_eof(fds[0], timeout_ms, out);
    close(fds[0]);
    if(!eof) kill(pid, SIGKILL);  // 卡住了就别再等它自己退

    // 收尸也有上限:子进程可能早就把 stdout 关了(daemon 化的就长这样)却一直不退出,
    // waitpid(..., 0) 会和裸 read 一样永远不返回。给 2 秒宽限,到点补一刀。
    // 这里杀不到 daemon 本身 —— 它已经和我们的子进程脱钩了,只是拿着同一个管道写端。
    const long long reap_deadline = now_ms() + 2000;
    int status = 0;
    pid_t w;
    while((w = waitpid(pid, &status, WNOHANG)) == 0 && now_ms() < reap_deadline) usleep(2000);
    if(w == 0) {
        kill(pid, SIGKILL);
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    return out;
}

bool command_exists(const char *name) {
    const char *path = getenv("PATH");
    if(!path || !*path) return access(name, X_OK) == 0;
    std::string paths = path;
    size_t pos = 0;
    while(pos <= paths.size()) {
        size_t end = paths.find(':', pos);
        std::string dir = end == std::string::npos ? paths.substr(pos) : paths.substr(pos, end - pos);
        if(dir.empty()) dir = ".";
        if(access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        if(end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

}
