#include "flomo_http.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kTotalTimeoutMs = 40000;   // 父进程自己的兜底,比 curl 的 -m 宽一点
constexpr int kMaxBodyBytes = 4 * 1024 * 1024;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void close_pair(int p[2]) {
    if(p[0] >= 0) close(p[0]);
    if(p[1] >= 0) close(p[1]);
    p[0] = p[1] = -1;
}

std::string trimmed(const std::string &s) {
    size_t b = 0, e = s.size();
    while(b < e && (s[b] == '\n' || s[b] == '\r' || s[b] == ' ' || s[b] == '\t')) ++b;
    while(e > b && (s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

}  // namespace

HttpResult http_request(const std::string &method, const std::string &url,
                        const std::vector<std::string> &headers,
                        const std::string &body) {
    HttpResult res;
    if(static_cast<int>(body.size()) > kMaxBodyBytes) {
        res.error = "请求体过大";
        return res;
    }

    int in_pipe[2] = {-1, -1};    // 父写 → 子 stdin(curl 的请求体)
    int out_pipe[2] = {-1, -1};   // 子 stdout → 父读(响应正文 + 状态码)
    int err_pipe[2] = {-1, -1};   // 子 stderr → 父读(curl 的报错文字)
    if(pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        res.error = std::string("pipe 失败: ") + strerror(errno);
        close_pair(in_pipe);
        close_pair(out_pipe);
        close_pair(err_pipe);
        return res;
    }
    // 只给写端设非阻塞:读端在 POLLIN 之后 read 一定能拿到数据,不会堵;写端即使 poll
    // 说可写,一次写多了同样会堵住,所以必须非阻塞,靠 EAGAIN 下一轮再补。
    int flags = fcntl(in_pipe[1], F_GETFL, 0);
    if(flags >= 0) fcntl(in_pipe[1], F_SETFL, flags | O_NONBLOCK);

    std::vector<std::string> args = {
        "curl", "-s", "-S",
        "-m", "30",
        "--connect-timeout", "10",
        "-X", method,
        // 正文之后紧接一个换行再跟状态码,父进程用最后一个换行切开。不用 -D 存头文件是
        // 因为那样要落一个临时文件,反过来更容易出错。
        "-w", "\n%{http_code}",
        "-H", "User-Agent: c1flomo-typix/0.1",
    };
    if(!body.empty()) {
        args.push_back("-H");
        args.push_back("Content-Type: application/json");
        args.push_back("--data-binary");
        args.push_back("@-");
    }
    for(const std::string &h : headers) {
        args.push_back("-H");
        args.push_back(h);
    }
    args.push_back(url);

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for(std::string &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if(pid < 0) {
        res.error = std::string("fork 失败: ") + strerror(errno);
        close_pair(in_pipe);
        close_pair(out_pipe);
        close_pair(err_pipe);
        return res;
    }

    if(pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close_pair(in_pipe);
        close_pair(out_pipe);
        close_pair(err_pipe);
        execvp("curl", argv.data());
        _exit(127);   // exec 失败;父进程按退出码 127 报「找不到 curl」
    }

    close(in_pipe[0]);
    in_pipe[0] = -1;
    close(out_pipe[1]);
    out_pipe[1] = -1;
    close(err_pipe[1]);
    err_pipe[1] = -1;

    size_t sent = 0;
    bool in_open = !body.empty();
    bool out_open = true, err_open = true;
    if(!in_open) {
        close(in_pipe[1]);
        in_pipe[1] = -1;
    }
    std::string out, errbuf;
    bool timed_out = false;
    const int64_t deadline = now_ms() + kTotalTimeoutMs;

    while(in_open || out_open || err_open) {
        pollfd pf[3];
        int n = 0, i_in = -1, i_out = -1, i_err = -1;
        if(in_open) {
            pf[n].fd = in_pipe[1];
            pf[n].events = POLLOUT;
            i_in = n++;
        }
        if(out_open) {
            pf[n].fd = out_pipe[0];
            pf[n].events = POLLIN;
            i_out = n++;
        }
        if(err_open) {
            pf[n].fd = err_pipe[0];
            pf[n].events = POLLIN;
            i_err = n++;
        }
        int64_t remain = deadline - now_ms();
        if(remain <= 0) {
            timed_out = true;
            break;
        }
        int pr = poll(pf, (nfds_t)n, (int)remain);
        if(pr < 0) {
            if(errno == EINTR) continue;
            break;
        }
        if(pr == 0) {
            timed_out = true;
            break;
        }

        if(i_in >= 0 && (pf[i_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(in_pipe[1], body.data() + sent, body.size() - sent);
            if(w > 0) sent += (size_t)w;
            // EAGAIN 是正常的(内核写缓冲满了),下一轮 poll 还会说可写。真正的错误
            // (EPIPE 等)或写完了就收掉这端,让 curl 那边看到 EOF。
            if(sent >= body.size() ||
               (w < 0 && errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)) {
                close(in_pipe[1]);
                in_pipe[1] = -1;
                in_open = false;
            }
        }
        if(i_out >= 0 && (pf[i_out].revents & (POLLIN | POLLERR | POLLHUP))) {
            char buf[4096];
            ssize_t r = read(out_pipe[0], buf, sizeof(buf));
            if(r > 0) out.append(buf, (size_t)r);
            else {
                close(out_pipe[0]);
                out_pipe[0] = -1;
                out_open = false;
            }
        }
        if(i_err >= 0 && (pf[i_err].revents & (POLLIN | POLLERR | POLLHUP))) {
            char buf[1024];
            ssize_t r = read(err_pipe[0], buf, sizeof(buf));
            if(r > 0) errbuf.append(buf, (size_t)r);
            else {
                close(err_pipe[0]);
                err_pipe[0] = -1;
                err_open = false;
            }
        }
    }

    if(timed_out) {
        kill(pid, SIGKILL);
        // 收尸,免得留僵尸。SIGKILL 之后 waitpid 立刻就回。
        while(waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        res.error = "请求超时";
    } else {
        int status = 0;
        while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            res.error = code == 127 ? "找不到 curl 命令"
                                    : ("curl 退出码 " + std::to_string(code));
            std::string e = trimmed(errbuf);
            if(!e.empty()) res.error += ": " + e.substr(0, 120);
        } else {
            // curl 用 -w 在正文末尾追加了 "\n<状态码>",最后一个换行之后就是它。
            size_t nl = out.rfind('\n');
            if(nl == std::string::npos) {
                res.error = "curl 输出无法解析";
            } else {
                res.status = atoi(out.c_str() + nl + 1);
                res.body = out.substr(0, nl);
                res.ok = res.status > 0;
                if(!res.ok) res.error = "HTTP 状态码解析失败";
            }
        }
    }

    // 超时那条路上的四个 fd 可能还没关;不关也不会泄漏到下一次调用(是局部变量),
    // 但顺手收干净。
    close_pair(in_pipe);
    close_pair(out_pipe);
    close_pair(err_pipe);
    return res;
}
