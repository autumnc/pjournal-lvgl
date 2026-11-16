#include "clip_web.h"

#include "clip_history.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{false};
std::atomic<int> g_listen_fd{-1};
std::thread g_thread;
uint16_t g_port = 80;

// 网页是编译进程序的常量,不占运行时内存;条目内容直接从历史缓冲发送。
const char PAGE[] = R"raw(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>pjournal 网络剪贴板</title>
<style>
body{font-family:system-ui,sans-serif;max-width:820px;margin:0 auto;padding:12px;font-size:14px}
h1{font-size:18px;margin:0 0 8px}
#msg{padding:6px;border-radius:4px;margin:8px 0;display:none}
.ok{background:#d4edda;color:#155724}.err{background:#f8d7da;color:#721c24}
table{width:100%;border-collapse:collapse;margin-bottom:10px}
th,td{padding:6px;border-bottom:1px solid #eee;text-align:left;font-size:13px}
.pv{cursor:pointer;color:#2563eb;word-break:break-all}
button{padding:3px 10px;font-size:12px;cursor:pointer;margin-right:4px}
textarea{width:100%;height:220px;font-family:monospace;font-size:13px;box-sizing:border-box}
.row{display:flex;gap:8px;margin:8px 0;flex-wrap:wrap}
</style></head><body>
<h1>pjournal 网络剪贴板</h1>
<div id="msg"></div>
<table><thead><tr><th>#</th><th>内容（首行）</th><th>字节</th><th>操作</th></tr></thead>
<tbody id="list"></tbody></table>
<div class="row">
<button onclick="newEntry()">新建</button>
<button onclick="save()">保存</button>
<button onclick="del()">删除当前条目</button>
</div>
<textarea id="txt" placeholder="在 PC 上粘贴文本，点“保存”即写入设备剪贴板历史（最新的在最前）"></textarea>
<script>
var cur=-1;
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
function msg(t,ok){var e=document.getElementById('msg');e.textContent=t;e.className=ok?'ok':'err';e.style.display='block';setTimeout(function(){e.style.display='none'},2500)}
function load(){
 fetch('/api/list').then(r=>r.json()).then(d=>{
  var h='';
  d.forEach(e=>{h+='<tr><td>'+e.i+'</td><td class="pv" onclick="edit('+e.i+')">'+esc(e.preview||'（空）')+'</td><td>'+e.len+'</td><td><button onclick="edit('+e.i+')">编辑</button><button onclick="delAt('+e.i+')">删除</button></td></tr>'});
  if(!d.length)h='<tr><td colspan="4">（暂无条目）</td></tr>';
  document.getElementById('list').innerHTML=h;
 }).catch(()=>msg('加载失败',false));
}
function edit(i){fetch('/api/item?i='+i).then(r=>r.text()).then(t=>{cur=i;document.getElementById('txt').value=t;msg('已载入第 '+i+' 条')}).catch(()=>msg('读取失败',false))}
function newEntry(){cur=-1;document.getElementById('txt').value='';msg('输入新文本后点保存')}
function save(){
 var body=document.getElementById('txt').value;
 if(!body){msg('内容为空',false);return}
 var url=cur<0?'/api/add':'/api/item?i='+cur;
 fetch(url,{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:body})
 .then(r=>r.json()).then(d=>{if(d.ok){msg('已保存',true);load()}else msg('保存失败: '+(d.error||''),false)})
 .catch(()=>msg('保存失败',false));
}
function del(){if(cur>=0)delAt(cur);else msg('先选择一条',false)}
function delAt(i){if(!confirm('删除第 '+i+' 条?'))return;fetch('/api/delete?i='+i,{method:'POST'}).then(r=>r.json()).then(d=>{
 if(d.ok){if(cur===i)cur=-1;else if(cur>i)cur--;if(cur<0)document.getElementById('txt').value='';}
 msg(d.ok?'已删除':('删除失败: '+(d.error||'')),d.ok);load()})}
load();
</script></body></html>)raw";

bool send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    while(len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if(n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

void send_response(int fd, int code, const char *status, const char *ctype,
                   const std::string &body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, status, ctype, (unsigned)body.size());
    if(n <= 0) return;
    if(!send_all(fd, hdr, (size_t)n)) return;
    send_all(fd, body.data(), body.size());
}

void send_json(int fd, const std::string &json) {
    send_response(fd, 200, "OK", "application/json; charset=utf-8", json);
}

void send_ok(int fd) { send_json(fd, "{\"ok\":true}"); }

void send_error(int fd, const char *msg) {
    std::string out = "{\"ok\":false,\"error\":\"";
    out += msg;
    out += "\"}";
    send_json(fd, out);
}

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

bool read_request(int fd, Request &req) {
    std::string buf;
    char tmp[2048];
    size_t header_end = std::string::npos;
    while((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
        if(buf.size() > 16384) return false;
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if(n <= 0) return false;
        buf.append(tmp, (size_t)n);
    }

    const std::string header = buf.substr(0, header_end);
    const std::string rest = buf.substr(header_end + 4);
    size_t line_end = header.find("\r\n");
    std::string first = header.substr(0, line_end == std::string::npos ? header.size() : line_end);
    size_t sp1 = first.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : first.find(' ', sp1 + 1);
    if(sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = first.substr(0, sp1);
    std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qpos = target.find('?');
    req.path = target.substr(0, qpos);
    req.query = qpos == std::string::npos ? "" : target.substr(qpos + 1);

    size_t content_len = 0;
    size_t pos = line_end == std::string::npos ? header.size() : line_end + 2;
    while(pos < header.size()) {
        size_t e = header.find("\r\n", pos);
        std::string line = header.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        size_t colon = line.find(':');
        if(colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            for(char &c : key) c = (char)tolower((unsigned char)c);
            if(key == "content-length") content_len = (size_t)strtoul(line.c_str() + colon + 1, nullptr, 10);
        }
        if(e == std::string::npos) break;
        pos = e + 2;
    }
    if(content_len > CLIPHIST_MAX_BYTES) return false;

    req.body = rest;
    while(req.body.size() < content_len) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if(n <= 0) return false;
        req.body.append(tmp, (size_t)n);
    }
    if(req.body.size() > content_len) req.body.resize(content_len);
    return true;
}

bool query_index(const std::string &query, int &out) {
    size_t pos = query.find("i=");
    if(pos == std::string::npos) return false;
    pos += 2;
    size_t end = query.find('&', pos);
    std::string v = query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if(v.empty()) return false;
    char *stop = nullptr;
    long n = strtol(v.c_str(), &stop, 10);
    if(stop == v.c_str() || *stop != '\0') return false;
    if(n < 0 || n >= CLIPHIST_MAX_ENTRIES) return false;
    out = (int)n;
    return true;
}

// 首行预览(最多 64 字节,UTF-8 边界),按 JSON 字符串转义。
std::string json_preview(const char *p, uint32_t len) {
    std::string out;
    size_t k = 0;
    while(k < len && k < 64) {
        unsigned char c = (unsigned char)p[k];
        if(c == '\n' || c == '\r') break;
        size_t step = 1;
        if((c & 0xE0) == 0xC0) step = 2;
        else if((c & 0xF0) == 0xE0) step = 3;
        else if((c & 0xF8) == 0xF0) step = 4;
        if(k + step > len || k + step > 64) break;
        if(c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if(c < 0x20) {
            out += ' ';
        } else {
            out.append(p + k, step);
        }
        k += step;
    }
    return out;
}

void handle_list(int fd) {
    const int n = cliphist_count();
    std::string out = "[";
    for(int i = 0; i < n; ++i) {
        const char *p = cliphist_text(i);
        if(!p) continue;
        const uint32_t len = cliphist_len(i);
        if(out.size() > 1) out += ',';
        out += "{\"i\":" + std::to_string(i);
        out += ",\"len\":" + std::to_string(len);
        out += ",\"linewise\":";
        out += cliphist_linewise(i) ? "1" : "0";
        out += ",\"preview\":\"" + json_preview(p, len) + "\"}";
    }
    out += "]";
    send_json(fd, out);
}

void handle_item_get(int fd, const Request &req) {
    int i = 0;
    if(!query_index(req.query, i)) {
        send_error(fd, "bad index");
        return;
    }
    const char *p = cliphist_text(i);
    if(!p) {
        send_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found");
        return;
    }
    send_response(fd, 200, "OK", "text/plain; charset=utf-8",
                  std::string(p, cliphist_len(i)));
}

void handle_item_post(int fd, const Request &req) {
    int i = 0;
    if(!query_index(req.query, i)) {
        send_error(fd, "bad index");
        return;
    }
    if(req.body.empty()) {
        send_error(fd, "body empty/too large");
        return;
    }
    if(cliphist_replace_raw(i, req.body.data(), req.body.size())) send_ok(fd);
    else send_error(fd, "replace failed");
}

void handle_add(int fd, const Request &req) {
    if(req.body.empty()) {
        send_error(fd, "body empty/too large");
        return;
    }
    // PC 传来的文本按普通文本处理(不用 vi 的整行语义)。
    if(cliphist_add_raw(req.body.data(), req.body.size(), false)) send_ok(fd);
    else send_error(fd, "add failed");
}

void handle_delete(int fd, const Request &req) {
    int i = 0;
    if(!query_index(req.query, i)) {
        send_error(fd, "bad index");
        return;
    }
    // 浏览器可能拿着过期的索引(比如另开标签页删过),越界要明确报错。
    if(i >= cliphist_count()) {
        send_error(fd, "index out of range");
        return;
    }
    cliphist_remove(i);
    send_ok(fd);
}

void serve_connection(int fd) {
    Request req;
    if(!read_request(fd, req)) {
        send_error(fd, "bad request");
        return;
    }
    if(req.path == "/" || req.path == "/index.html") {
        send_response(fd, 200, "OK", "text/html; charset=utf-8", std::string(PAGE));
    } else if(req.path == "/api/list" && req.method == "GET") {
        handle_list(fd);
    } else if(req.path == "/api/item" && req.method == "GET") {
        handle_item_get(fd, req);
    } else if(req.path == "/api/item" && req.method == "POST") {
        handle_item_post(fd, req);
    } else if(req.path == "/api/add" && req.method == "POST") {
        handle_add(fd, req);
    } else if(req.path == "/api/delete" && req.method == "POST") {
        handle_delete(fd, req);
    } else {
        send_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found");
    }
}

// 一次只处理一条连接:浏览器并发请求会排在 backlog 里依次处理。
// 历史表因此天然串行,不需要额外加锁。
void accept_loop() {
    while(g_running.load()) {
        int listen_fd = g_listen_fd.load();
        if(listen_fd < 0) break;
        int fd = accept(listen_fd, nullptr, nullptr);
        if(fd < 0) {
            if(!g_running.load()) break;
            usleep(20000);
            continue;
        }
        struct timeval tv {};
        tv.tv_sec = 2;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serve_connection(fd);
        shutdown(fd, SHUT_WR);
        close(fd);
    }
}

}  // namespace

bool clip_web_active() { return g_running.load(); }
uint16_t clip_web_port() { return g_port; }

bool clip_web_start(uint16_t port) {
    if(g_running.load()) return true;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if(bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return false;
    }
    signal(SIGPIPE, SIG_IGN);

    g_port = port;
    g_listen_fd.store(fd);
    g_running.store(true);
    try {
        g_thread = std::thread(accept_loop);
    } catch(...) {
        g_running.store(false);
        g_listen_fd.store(-1);
        close(fd);
        return false;
    }
    return true;
}

void clip_web_stop() {
    if(!g_running.exchange(false)) return;
    int fd = g_listen_fd.exchange(-1);
    if(fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if(g_thread.joinable()) g_thread.join();
}
