#pragma once

#include <string>
#include <vector>

struct HttpResult {
    // ok 表示「curl 跑完了并且给出了状态码」—— HTTP 4xx/5xx 同样算 ok,状态在 status 里。
    // ok 为 false 时 error 是可以直接显示给用户的失败原因。
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

// 跑一次 curl 子进程取 HTTP 响应。
//
// 为什么要自己 fork/exec 而不是用 pjournal 的 process_utils:那个 run_capture 把子进程
// stdin 接到 /dev/null(故意的,免得子进程抢 /dev/tty),而 Flomo 的 POST/PUT 要把 JSON
// 请求体喂给 curl。更关键的是**密码只在请求体里**,请求体走 stdin —— 放进 argv 的话
// `ps` 一眼就能看到。
//
// method 是 "GET"/"POST"/"PUT"/"DELETE";headers 每条一个 "Name: value";
// body 为空则不给 curl 传 --data-binary,stdin 立刻 EOF。
HttpResult http_request(const std::string &method, const std::string &url,
                        const std::vector<std::string> &headers,
                        const std::string &body);
