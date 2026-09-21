#pragma once

#include <string>
#include <vector>

namespace process {

// 跑一条命令并把它的 stdout+stderr 收回来。**一定有超时**:超时就 SIGKILL 并把
// 已经读到的部分返回。子进程的 stdin 接的是 /dev/null(不接会继承 /dev/tty,
// 交互式子命令能把键盘字节抢走)。默认 15s,网络类的调用自己传更长的。
std::string run_capture(const std::vector<std::string> &args, int timeout_ms = 15000);

bool command_exists(const char *name);

}
