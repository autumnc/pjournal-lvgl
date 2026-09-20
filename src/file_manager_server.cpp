#include "file_manager_server.h"

#include "settings.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/time.h>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running{false};
std::atomic<int> g_listen_fd{-1};
std::thread g_accept_thread;
uint16_t g_port = 8080;
std::string g_root;  // 服务根目录:日记目录,请求路径必须落在它下面

// ── 基础工具 ─────────────────────────────────────────────────────────────

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

bool send_str(int fd, const std::string &s) { return send_all(fd, s.data(), s.size()); }

std::string url_decode(const std::string &src) {
    std::string out;
    out.reserve(src.size());
    for(size_t i = 0; i < src.size(); ++i) {
        if(src[i] == '%' && i + 2 < src.size()) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if(src[i] == '+') {
            out += ' ';
        } else {
            out += src[i];
        }
    }
    return out;
}

bool safe_upload_name(const std::string &name) {
    if(name.empty() || name == "." || name == "..") return false;
    for(unsigned char c : name) {
        if(c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(unsigned char c : s) {
        if(c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if(c == '\n') out += "\\n";
        else if(c == '\r') out += "\\r";
        else if(c == '\t') out += "\\t";
        else if(c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else out += (char)c;
    }
    return out;
}

std::string format_size(off_t size) {
    char buf[32];
    if(size < 1024) snprintf(buf, sizeof(buf), "%lld B", (long long)size);
    else if(size < 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f KB", size / 1024.0);
    else snprintf(buf, sizeof(buf), "%.1f MB", size / (1024.0 * 1024.0));
    return buf;
}

// 只允许访问根目录内的路径;先归一化再比较前缀,避免 "/root/pjournal2" 混入。
bool is_safe_path(const std::string &path) {
    if(path.empty() || path[0] != '/') return false;
    if(g_root.empty()) return false;
    std::vector<std::string> parts;
    size_t i = 0;
    while(i < path.size()) {
        size_t j = path.find('/', i);
        if(j == std::string::npos) j = path.size();
        std::string seg = path.substr(i, j - i);
        if(seg == "..") return false;
        if(!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    std::string norm = "/";
    for(size_t k = 0; k < parts.size(); ++k) {
        if(k) norm += '/';
        norm += parts[k];
    }
    if(norm == g_root) return true;
    return norm.size() > g_root.size() && norm.compare(0, g_root.size(), g_root) == 0 && norm[g_root.size()] == '/';
}

// ── 内嵌页面 ─────────────────────────────────────────────────────────────

const char *HTML_PAGE = R"raw(<!DOCTYPE html>
<html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>文件管理</title>
<style>
body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:12px;font-size:14px}
h1{font-size:18px;margin:0 0 12px}
#breadcrumb{margin-bottom:8px;color:#666}
table{width:100%;border-collapse:collapse}
th,td{padding:6px 8px;text-align:left;border-bottom:1px solid #eee;font-size:13px}
th{background:#f5f5f5;font-weight:600}
.dir{color:#2563eb;cursor:pointer}
.dir:hover{text-decoration:underline}
.act{white-space:nowrap}
.act button{margin:0 2px;padding:2px 8px;font-size:12px;cursor:pointer}
#toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:12px 0}
#toolbar input[type=text]{width:120px;padding:3px 6px}
#toolbar input[type=file]{font-size:12px}
#msg{padding:6px;margin:8px 0;border-radius:4px;display:none}
.ok{background:#d4edda;color:#155724}
.err{background:#f8d7da;color:#721c24}
</style></head><body>
<h1>pjournal - 文件管理</h1>
<div id="breadcrumb"></div>
<div id="toolbar">
<input type="file" id="fileInput">
<button onclick="upload()">上传</button>
<input type="text" id="dirName" placeholder="文件夹名">
<button onclick="mkdir()">新建</button>
<button onclick="setToken()">密码</button>
</div>
<div id="msg"></div>
<table><thead><tr><th>名称</th><th>大小</th><th>操作</th></tr></thead>
<tbody id="list"></tbody></table>
<script>
var ROOT='__ROOT__';
var curPath=ROOT;
var token='';try{token=localStorage.getItem('pjournal_token')||''}catch(e){}
function hd(){return token?{'X-Auth-Token':token}:{}}
function setToken(){var t=prompt('文件管理密码(留空则不设)',token);if(t!==null){token=t.trim();try{localStorage.setItem('pjournal_token',token)}catch(e){}}}
function showMsg(t,ok){var e=document.getElementById('msg');e.textContent=t;e.className=ok?'ok':'err';e.style.display='block';setTimeout(function(){e.style.display='none'},3000)}
function loadDir(p){
  curPath=p;
  fetch('/api/list?path='+encodeURIComponent(p),{headers:hd()}).then(r=>r.json()).then(d=>{
    document.getElementById('breadcrumb').textContent=d.path;
    var h='';
    if(d.path!==ROOT) h+='<tr><td class="dir" onclick="loadDir(\''+esc(p.replace(/\/[^/]*$/,''))+'\')">..</td><td></td><td></td></tr>';
    d.entries.forEach(e=>{
      var fp=esc((d.path==='/'?'':d.path)+'/'+e.name);
      if(e.type==='dir') h+='<tr><td class="dir" onclick="loadDir(\''+fp+'\')">'+esc(e.name)+'/</td><td></td><td class="act"><button onclick="dlDir(\''+fp+'\')">下载</button><button onclick="del(\''+fp+'\',true)">删除</button></td></tr>';
      else h+='<tr><td>'+esc(e.name)+'</td><td>'+e.size+'</td><td class="act"><button onclick="dl(\''+fp+'\')">下载</button><button onclick="del(\''+fp+'\',false)">删除</button></td></tr>';
    });
    document.getElementById('list').innerHTML=h;
  }).catch(e=>showMsg('加载失败',false));
}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/'/g,'&#39;')}
function upload(){
  var f=document.getElementById('fileInput').files[0];if(!f)return;
  var fd=new FormData();fd.append('file',f);
  fetch('/api/upload?path='+encodeURIComponent(curPath)+'&name='+encodeURIComponent(f.name),{method:'POST',body:fd,headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'上传成功':'上传失败: '+d.error,d.ok);loadDir(curPath)})
  .catch(()=>showMsg('上传失败',false));
}
function dl(p){window.open('/api/download?path='+encodeURIComponent(p)+'&token='+encodeURIComponent(token))}
function dlDir(p){window.open('/api/download_dir?path='+encodeURIComponent(p)+'&token='+encodeURIComponent(token))}
function del(p,isDir){
  if(!confirm('确认删除?'))return;
  fetch('/api/delete?path='+encodeURIComponent(p)+'&dir='+isDir,{method:'POST',headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'删除成功':'删除失败: '+d.error,d.ok);loadDir(curPath)})
  .catch(()=>showMsg('删除失败',false));
}
function mkdir(){
  var n=document.getElementById('dirName').value.trim();if(!n)return;
  fetch('/api/mkdir?path='+encodeURIComponent(curPath+'/'+n),{method:'POST',headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'创建成功':'创建失败: '+d.error,d.ok);if(d.ok){document.getElementById('dirName').value='';loadDir(curPath)}})
  .catch(()=>showMsg('创建失败',false));
}
loadDir(ROOT);
</script></body></html>)raw";

// ── 连接与请求 ───────────────────────────────────────────────────────────

struct Conn {
    int fd = -1;
    std::string buf;  // 已收到但未消费的字节(先头部后正文)
    bool eof = false;

    bool pull() {
        while(buf.empty()) {
            if(eof) return false;
            char tmp[8192];
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if(n <= 0) {
                eof = true;
                return false;
            }
            buf.append(tmp, (size_t)n);
        }
        return true;
    }
};

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    size_t content_length = 0;

    std::string header(const std::string &key) const {
        auto it = headers.find(key);
        return it == headers.end() ? "" : it->second;
    }
};

void respond(int fd, const char *status, const char *ctype, const std::string &body,
             const std::string &extra = "") {
    std::string head = std::string("HTTP/1.1 ") + status + "\r\n";
    if(ctype) head += std::string("Content-Type: ") + ctype + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += extra;
    head += "Connection: close\r\n\r\n";
    if(!send_str(fd, head)) return;
    send_str(fd, body);
}

void respond_json(int fd, const std::string &json, const char *status = "200 OK") {
    respond(fd, status, "application/json; charset=utf-8", json);
}

void respond_json_error(int fd, const std::string &msg, const char *status = "200 OK") {
    respond_json(fd, "{\"ok\":false,\"error\":\"" + json_escape(msg) + "\"}", status);
}

std::string get_query_param(const Request &req, const std::string &key) {
    size_t pos = 0;
    while(pos < req.query.size()) {
        size_t amp = req.query.find('&', pos);
        if(amp == std::string::npos) amp = req.query.size();
        std::string pair = req.query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if(eq != std::string::npos && pair.substr(0, eq) == key)
            return url_decode(pair.substr(eq + 1));
        pos = amp + 1;
    }
    return "";
}

// 设置里配了"文件管理密码"后,每个请求都要带 X-Auth-Token 头或 ?token= 参数。
bool auth_ok(const Request &req) {
    std::string expected = g_settings.get("file_mgr_token", "");
    if(expected.empty()) return true;
    if(req.header("x-auth-token") == expected) return true;
    std::string q = get_query_param(req, "token");
    return !q.empty() && q == expected;
}

bool parse_request(Conn &conn, Request &req) {
    size_t header_end = std::string::npos;
    while(true) {
        header_end = conn.buf.find("\r\n\r\n");
        if(header_end != std::string::npos) break;
        if(conn.buf.size() > 16384) return false;  // 头部过大
        if(!conn.pull()) return false;
    }
    std::string head = conn.buf.substr(0, header_end);
    conn.buf.erase(0, header_end + 4);

    size_t line_end = head.find("\r\n");
    std::string request_line = head.substr(0, line_end == std::string::npos ? head.size() : line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.rfind(' ');
    if(sp1 == std::string::npos || sp2 == std::string::npos || sp2 <= sp1) return false;
    req.method = request_line.substr(0, sp1);
    std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qm = target.find('?');
    req.path = qm == std::string::npos ? target : target.substr(0, qm);
    req.query = qm == std::string::npos ? "" : target.substr(qm + 1);

    size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while(pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if(end == std::string::npos) end = head.size();
        std::string line = head.substr(pos, end - pos);
        size_t colon = line.find(':');
        if(colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            for(char &c : key) c = (char)tolower((unsigned char)c);
            size_t start = value.find_first_not_of(" \t");
            req.headers[key] = start == std::string::npos ? "" : value.substr(start);
        }
        pos = end + 2;
    }
    std::string cl = req.header("content-length");
    req.content_length = cl.empty() ? 0 : (size_t)strtoull(cl.c_str(), nullptr, 10);
    return true;
}

// ── ZIP(仅存储,不压缩) ──────────────────────────────────────────────────

struct ZipEntry {
    std::string rel_path;
    uint32_t crc32 = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
};

uint32_t g_crc_table[256];
bool g_crc_ready = false;
const size_t MAX_ZIP_FILES = 512;
const uint64_t MAX_ZIP_TOTAL_SIZE = 64ULL * 1024 * 1024;

void init_crc32() {
    if(g_crc_ready) return;
    for(uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for(int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

uint32_t compute_crc32(const uint8_t *data, size_t len, uint32_t crc) {
    init_crc32();
    for(size_t i = 0; i < len; i++) crc = g_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put_u16(uint8_t *buf, uint16_t v) { buf[0] = (uint8_t)v; buf[1] = (uint8_t)(v >> 8); }
void put_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)v;
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

void collect_files(const std::string &dir_path, const std::string &base_path,
                   std::vector<ZipEntry> &entries, uint64_t &total_size) {
    DIR *dir = opendir(dir_path.c_str());
    if(!dir) return;
    struct dirent *ent;
    while((ent = readdir(dir)) != nullptr) {
        if(entries.size() >= MAX_ZIP_FILES || total_size >= MAX_ZIP_TOTAL_SIZE) break;
        if(ent->d_name[0] == '.') continue;
        std::string full = dir_path + "/" + ent->d_name;
        std::string rel = base_path.empty() ? ent->d_name : base_path + "/" + ent->d_name;
        struct stat st;
        if(stat(full.c_str(), &st) != 0) continue;
        if(S_ISDIR(st.st_mode)) {
            collect_files(full, rel, entries, total_size);
        } else if(rel.size() <= 0xFFFF) {
            ZipEntry e;
            e.rel_path = rel;
            e.size = (uint32_t)st.st_size;
            entries.push_back(e);
            total_size += (uint64_t)st.st_size;
        }
    }
    closedir(dir);
}

uint64_t zip_out_size(const std::vector<ZipEntry> &entries) {
    uint64_t n = 22;  // 结尾记录
    for(const auto &e : entries) n += 30 + e.rel_path.size() + (uint64_t)e.size + 46 + e.rel_path.size();
    return n;
}

// ── 各接口 ───────────────────────────────────────────────────────────────

void handle_index(int fd) {
    std::string page = HTML_PAGE;
    size_t pos;
    while((pos = page.find("__ROOT__")) != std::string::npos)
        page.replace(pos, 8, g_root);
    respond(fd, "200 OK", "text/html; charset=utf-8", page);
}

void handle_list(int fd, const Request &req) {
    std::string path = get_query_param(req, "path");
    if(path.empty()) path = g_root;
    if(!is_safe_path(path)) {
        respond_json_error(fd, "invalid path");
        return;
    }
    DIR *dir = opendir(path.c_str());
    if(!dir) {
        respond_json_error(fd, "cannot open directory");
        return;
    }
    std::string json = "{\"path\":\"" + json_escape(path) + "\",\"entries\":[";
    struct dirent *ent;
    bool first = true;
    while((ent = readdir(dir)) != nullptr) {
        if(ent->d_name[0] == '.') continue;
        std::string full = path + "/" + ent->d_name;
        struct stat st;
        bool is_dir = false;
        off_t fsize = 0;
        if(stat(full.c_str(), &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            fsize = st.st_size;
        }
        if(!first) json += ",";
        first = false;
        json += "{\"name\":\"" + json_escape(ent->d_name) + "\",\"type\":\"" + (is_dir ? "dir" : "file") +
                "\",\"size\":\"" + format_size(fsize) + "\"}";
    }
    closedir(dir);
    json += "]}";
    respond_json(fd, json);
}

void handle_download(int fd, const Request &req) {
    std::string path = get_query_param(req, "path");
    if(!is_safe_path(path)) {
        respond(fd, "404 Not Found", "text/plain", "not found");
        return;
    }
    struct stat st;
    if(stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        respond(fd, "404 Not Found", "text/plain", "not found");
        return;
    }
    FILE *f = fopen(path.c_str(), "rb");
    if(!f) {
        respond(fd, "500 Internal Server Error", "text/plain", "cannot open file");
        return;
    }
    std::string filename = path.substr(path.rfind('/') + 1);
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
    head += "Content-Length: " + std::to_string((long long)st.st_size) + "\r\n";
    head += "Content-Disposition: attachment; filename=\"" + filename + "\"\r\n";
    head += "Connection: close\r\n\r\n";
    if(!send_str(fd, head)) {
        fclose(f);
        return;
    }
    char buf[16384];
    size_t n;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if(!send_all(fd, buf, n)) break;
    }
    fclose(f);
}

void handle_download_dir(int fd, const Request &req) {
    std::string path = get_query_param(req, "path");
    if(!is_safe_path(path)) {
        respond(fd, "404 Not Found", "text/plain", "not found");
        return;
    }
    struct stat st;
    if(stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        respond(fd, "404 Not Found", "text/plain", "not found");
        return;
    }
    std::string dir_name = path.substr(path.rfind('/') + 1);
    std::vector<ZipEntry> entries;
    uint64_t total_size = 0;
    collect_files(path, "", entries, total_size);
    if(entries.empty()) {
        respond_json_error(fd, "empty directory");
        return;
    }

    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n";
    head += "Content-Length: " + std::to_string(zip_out_size(entries)) + "\r\n";
    head += "Content-Disposition: attachment; filename=\"" + dir_name + ".zip\"\r\n";
    head += "Connection: close\r\n\r\n";
    if(!send_str(fd, head)) return;

    uint8_t buf[16384];
    uint32_t offset = 0;

    for(auto &e : entries) {
        e.offset = offset;
        size_t name_len = e.rel_path.size();
        std::vector<uint8_t> lfh(30 + name_len);
        put_u32(lfh.data() + 0, 0x04034b50);
        put_u16(lfh.data() + 4, 20);
        put_u16(lfh.data() + 6, 0);
        put_u16(lfh.data() + 8, 0);
        put_u16(lfh.data() + 10, 0);
        put_u16(lfh.data() + 12, 0);
        put_u32(lfh.data() + 14, 0);
        put_u32(lfh.data() + 18, 0);
        put_u32(lfh.data() + 22, 0);
        put_u16(lfh.data() + 26, (uint16_t)name_len);
        put_u16(lfh.data() + 28, 0);
        memcpy(lfh.data() + 30, e.rel_path.c_str(), name_len);

        std::string full_path = path + "/" + e.rel_path;
        FILE *f = fopen(full_path.c_str(), "rb");
        uint32_t crc = 0xFFFFFFFF;
        uint32_t fsize = 0;
        if(f) {
            size_t n;
            while((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                crc = compute_crc32(buf, n, crc);
                fsize += (uint32_t)n;
            }
            fclose(f);
        }
        crc ^= 0xFFFFFFFF;
        put_u32(lfh.data() + 14, crc);
        put_u32(lfh.data() + 18, fsize);
        put_u32(lfh.data() + 22, fsize);
        e.crc32 = crc;
        e.size = fsize;

        if(!send_all(fd, lfh.data(), lfh.size())) return;
        offset += (uint32_t)lfh.size();

        if(fsize > 0) {
            f = fopen(full_path.c_str(), "rb");
            if(!f) continue;
            size_t n;
            while((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                if(!send_all(fd, buf, n)) {
                    fclose(f);
                    return;
                }
            }
            fclose(f);
            offset += fsize;
        }
    }

    uint32_t cd_offset = offset;
    for(auto &e : entries) {
        size_t name_len = e.rel_path.size();
        std::vector<uint8_t> cdh(46 + name_len);
        put_u32(cdh.data() + 0, 0x02014b50);
        put_u16(cdh.data() + 4, 20);
        put_u16(cdh.data() + 6, 20);
        put_u16(cdh.data() + 8, 0);
        put_u16(cdh.data() + 10, 0);
        put_u16(cdh.data() + 12, 0);
        put_u16(cdh.data() + 14, 0);
        put_u32(cdh.data() + 16, e.crc32);
        put_u32(cdh.data() + 20, e.size);
        put_u32(cdh.data() + 24, e.size);
        put_u16(cdh.data() + 28, (uint16_t)name_len);
        put_u16(cdh.data() + 30, 0);
        put_u16(cdh.data() + 32, 0);
        put_u16(cdh.data() + 34, 0);
        put_u16(cdh.data() + 36, 0);
        put_u32(cdh.data() + 38, 0);
        put_u32(cdh.data() + 42, e.offset);
        memcpy(cdh.data() + 46, e.rel_path.c_str(), name_len);
        if(!send_all(fd, cdh.data(), cdh.size())) return;
        offset += (uint32_t)cdh.size();
    }
    uint32_t cd_size = offset - cd_offset;

    uint8_t eocd[22];
    put_u32(eocd + 0, 0x06054b50);
    put_u16(eocd + 4, 0);
    put_u16(eocd + 6, 0);
    put_u16(eocd + 8, (uint16_t)entries.size());
    put_u16(eocd + 10, (uint16_t)entries.size());
    put_u32(eocd + 12, cd_size);
    put_u32(eocd + 16, cd_offset);
    put_u16(eocd + 20, 0);
    send_all(fd, eocd, sizeof(eocd));
}

bool save_upload_body(Conn &conn, const Request &req, const std::string &tmp_path, size_t &written) {
    std::string boundary;
    std::string ct = req.header("content-type");
    size_t bpos = ct.find("boundary=");
    if(bpos == std::string::npos) return false;
    boundary = "--" + ct.substr(bpos + 9);

    FILE *f = fopen(tmp_path.c_str(), "wb");
    if(!f) return false;

    // 只保留可能被截断的边界尾部,避免把整个上传体驻留内存。
    const std::string hdr_sep = "\r\n\r\n";
    const std::string marker = "\r\n" + boundary;
    const size_t keep = marker.size() - 1;
    std::string window;
    size_t remaining = req.content_length;
    bool in_header = true;
    bool done = false;
    bool ok = true;
    written = 0;

    while(remaining > 0 && !done && ok) {
        if(!conn.pull()) {
            ok = false;
            break;
        }
        size_t take = std::min(remaining, conn.buf.size());
        window.append(conn.buf, 0, take);
        conn.buf.erase(0, take);
        remaining -= take;

        if(in_header) {
            size_t it = window.find(hdr_sep);
            if(it == std::string::npos) {
                if(window.size() > 8192) ok = false;
                continue;
            }
            window.erase(0, it + hdr_sep.size());
            in_header = false;
        }

        while(!done && !window.empty()) {
            size_t it = window.find(marker);
            if(it != std::string::npos) {
                if(it > 0 && fwrite(window.data(), 1, it, f) != it) ok = false;
                written += it;
                window.clear();
                done = true;
                break;
            }
            if(window.size() <= keep) break;
            size_t write_now = window.size() - keep;
            if(fwrite(window.data(), 1, write_now, f) != write_now) {
                ok = false;
                break;
            }
            written += write_now;
            window.erase(0, write_now);
        }
    }
    // 正文提前结束(没有收尾边界)时把缓冲刷出去
    if(ok && !in_header && !done && !window.empty()) {
        if(fwrite(window.data(), 1, window.size(), f) != window.size()) ok = false;
        written += window.size();
        window.clear();
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return ok;
}

void handle_upload(int fd, Conn &conn, const Request &req) {
    std::string dir = get_query_param(req, "path");
    std::string name = get_query_param(req, "name");
    if(!is_safe_path(dir) || !safe_upload_name(name)) {
        respond_json_error(fd, "invalid path or name");
        return;
    }
    const size_t MAX_UPLOAD = 16 * 1024 * 1024;
    if(req.content_length == 0) {
        respond_json_error(fd, "empty body");
        return;
    }
    if(req.content_length > MAX_UPLOAD) {
        respond_json_error(fd, "file too large");
        return;
    }
    if(req.header("content-type").find("boundary=") == std::string::npos) {
        respond_json_error(fd, "no boundary");
        return;
    }

    std::string full_path = dir + "/" + name;
    std::string tmp_path = dir + "/." + name + ".upload";
    size_t written = 0;
    if(!save_upload_body(conn, req, tmp_path, written)) {
        remove(tmp_path.c_str());
        respond_json_error(fd, "upload failed");
        return;
    }
    remove(full_path.c_str());
    if(rename(tmp_path.c_str(), full_path.c_str()) != 0) {
        remove(tmp_path.c_str());
        respond_json_error(fd, "upload failed");
        return;
    }
    respond_json(fd, "{\"ok\":true}");
}

void handle_delete(int fd, const Request &req) {
    std::string path = get_query_param(req, "path");
    std::string dir_flag = get_query_param(req, "dir");
    if(!is_safe_path(path) || path == g_root) {
        respond_json_error(fd, "invalid path");
        return;
    }
    int ret = (dir_flag == "1" || dir_flag == "true") ? rmdir(path.c_str()) : remove(path.c_str());
    if(ret == 0) respond_json(fd, "{\"ok\":true}");
    else respond_json_error(fd, "delete failed");
}

void handle_mkdir(int fd, const Request &req) {
    std::string path = get_query_param(req, "path");
    if(!is_safe_path(path)) {
        respond_json_error(fd, "invalid path");
        return;
    }
    if(mkdir(path.c_str(), 0777) == 0) respond_json(fd, "{\"ok\":true}");
    else respond_json_error(fd, "mkdir failed");
}

void serve_connection(Conn &conn) {
    Request req;
    if(!parse_request(conn, req)) return;

    int fd = conn.fd;
    // 页面本身不含数据,且要先加载它才能填密码,所以只对 /api/ 校验令牌。
    if(!auth_ok(req) && req.path.rfind("/api/", 0) == 0) {
        respond_json_error(fd, "unauthorized", "401 Unauthorized");
        return;
    }
    if(req.path == "/" || req.path == "/index.html") handle_index(fd);
    else if(req.path == "/api/list") handle_list(fd, req);
    else if(req.path == "/api/download") handle_download(fd, req);
    else if(req.path == "/api/download_dir") handle_download_dir(fd, req);
    else if(req.path == "/api/upload" && req.method == "POST") handle_upload(fd, conn, req);
    else if(req.path == "/api/delete" && req.method == "POST") handle_delete(fd, req);
    else if(req.path == "/api/mkdir" && req.method == "POST") handle_mkdir(fd, req);
    else respond(fd, "404 Not Found", "text/plain; charset=utf-8", "not found");
}

void accept_loop() {
    while(g_running.load()) {
        int listen_fd = g_listen_fd.load();
        if(listen_fd < 0) break;
        int fd = accept(listen_fd, nullptr, nullptr);
        if(fd < 0) {
            if(!g_running.load()) break;
            continue;
        }
        std::thread([fd]() {
            Conn conn;
            conn.fd = fd;
            serve_connection(conn);
            // 客户端只发了部分正文(如上传被拒)时,直接 close 会发 RST 丢掉响应;
            // 先把半关闭并读干净残留数据再关闭。
            shutdown(fd, SHUT_WR);
            struct timeval tv {2, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char tmp[4096];
            while(recv(fd, tmp, sizeof(tmp), 0) > 0) {}
            close(fd);
        }).detach();
    }
}

}  // namespace

bool file_manager_server_start(uint16_t port) {
    if(g_running.load()) return true;
    g_root = g_settings.journal_dir();
    while(g_root.size() > 1 && g_root.back() == '/') g_root.pop_back();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if(bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return false;
    }
    signal(SIGPIPE, SIG_IGN);

    g_port = port;
    g_listen_fd.store(fd);
    g_running.store(true);
    g_accept_thread = std::thread(accept_loop);
    return true;
}

void file_manager_server_stop() {
    if(!g_running.exchange(false)) return;
    int fd = g_listen_fd.exchange(-1);
    if(fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if(g_accept_thread.joinable()) g_accept_thread.join();
}

bool file_manager_server_running() { return g_running.load(); }

uint16_t file_manager_server_get_port() { return g_port; }
