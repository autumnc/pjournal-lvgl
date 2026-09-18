#include "webdav_sync.h"

#include "journal_storage.h"
#include "safe_file.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <vector>

struct RemoteFile {
    std::string name;
    time_t mtime = 0;
};

static std::string shell_quote(const std::string &s) {
    std::string out = "'";
    for(char c : s) {
        if(c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string trim(std::string s) {
    while(!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t p = s.find_first_not_of(" \t\r\n");
    return p == std::string::npos ? "" : s.substr(p);
}

static std::string run_capture(const std::string &cmd) {
    FILE *p = popen(cmd.c_str(), "r");
    if(!p) return "";
    std::string out;
    char buf[1024];
    while(fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out;
}

static std::string base_url() {
    std::string u = g_settings.get("webdav_url", "");
    while(!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

static std::string auth_arg() {
    std::string user = g_settings.get("webdav_user", "");
    std::string pass = g_settings.get("webdav_pass", "");
    if(user.empty()) return "";
    return " -u " + shell_quote(user + ":" + pass);
}

static std::string remote_url(const std::string &path) {
    return base_url() + "/" + path;
}

static bool is_journal_file(const std::string &name) {
    if(name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) return false;
    size_t dot = name.rfind('.');
    if(dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    return ext == ".txt" || ext == ".md";
}

static time_t parse_http_date(const std::string &s) {
    tm t {};
    char *end = strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S", &t);
    if(!end) return 0;
#if defined(_GNU_SOURCE) || defined(__GLIBC__)
    return timegm(&t);
#else
    char *old_tz = getenv("TZ");
    std::string old = old_tz ? old_tz : "";
    setenv("TZ", "UTC", 1);
    tzset();
    time_t out = mktime(&t);
    if(old_tz) setenv("TZ", old.c_str(), 1);
    else unsetenv("TZ");
    tzset();
    return out;
#endif
}

static std::string xml_unescape(std::string s) {
    struct Pair { const char *a; const char *b; };
    const Pair pairs[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    for(const auto &p : pairs) {
        size_t pos = 0;
        while((pos = s.find(p.a, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(p.a), p.b);
            pos += std::strlen(p.b);
        }
    }
    return s;
}

static std::string url_basename(std::string href) {
    href = xml_unescape(href);
    while(!href.empty() && href.back() == '/') href.pop_back();
    size_t slash = href.rfind('/');
    return slash == std::string::npos ? href : href.substr(slash + 1);
}

static std::string tag_value(const std::string &block, const std::string &tag) {
    size_t p = block.find(tag);
    if(p == std::string::npos) return "";
    p = block.find('>', p);
    if(p == std::string::npos) return "";
    size_t e = block.find('<', p + 1);
    if(e == std::string::npos) return "";
    return block.substr(p + 1, e - p - 1);
}

static std::vector<RemoteFile> list_remote() {
    std::vector<RemoteFile> out;
    std::string cmd = "curl -sS -X PROPFIND -H 'Depth: 1'" + auth_arg() + " " + shell_quote(remote_url("journal/"));
    std::string xml = run_capture(cmd);
    size_t pos = 0;
    while(true) {
        size_t a = xml.find("<", pos);
        if(a == std::string::npos) break;
        size_t gt = xml.find('>', a);
        if(gt == std::string::npos) break;
        if(xml.substr(a, gt - a + 1).find("response") == std::string::npos) { pos = gt + 1; continue; }
        size_t close = xml.find("</", gt);
        while(close != std::string::npos) {
            size_t close_gt = xml.find('>', close);
            if(close_gt == std::string::npos) break;
            if(xml.substr(close, close_gt - close + 1).find("response") != std::string::npos) break;
            close = xml.find("</", close_gt + 1);
        }
        if(close == std::string::npos) break;
        size_t end = xml.find('>', close);
        if(end == std::string::npos) break;
        std::string block = xml.substr(a, end - a + 1);
        std::string href = tag_value(block, "href");
        std::string name = url_basename(href);
        if(is_journal_file(name)) {
            std::string lm = tag_value(block, "getlastmodified");
            out.push_back({name, parse_http_date(lm)});
        }
        pos = end + 1;
    }
    return out;
}

static bool curl_mkcol() {
    std::string cmd = "curl -sS -o /dev/null -w '%{http_code}' -X MKCOL" + auth_arg() + " " + shell_quote(remote_url("journal/"));
    std::string code = trim(run_capture(cmd));
    return code == "201" || code == "405" || code == "301" || code == "302";
}

static bool curl_upload(const std::string &filename, const std::string &local_path) {
    std::string cmd = "curl -sS -o /dev/null -w '%{http_code}' -X PUT -T " + shell_quote(local_path) + auth_arg() + " " + shell_quote(remote_url("journal/" + filename));
    std::string code = trim(run_capture(cmd));
    return code == "200" || code == "201" || code == "204";
}

static bool curl_download(const std::string &filename, const std::string &out_path) {
    std::string cmd = "curl -sS -L -o " + shell_quote(out_path) + " -w '%{http_code}'" + auth_arg() + " " + shell_quote(remote_url("journal/" + filename));
    std::string code = trim(run_capture(cmd));
    return code == "200" || code == "203";
}

static bool curl_delete(const std::string &filename) {
    std::string cmd = "curl -sS -o /dev/null -w '%{http_code}' -X DELETE" + auth_arg() + " " + shell_quote(remote_url("journal/" + filename));
    std::string code = trim(run_capture(cmd));
    return code == "200" || code == "202" || code == "204" || code == "404";
}

static std::string state_path() {
    return g_settings.get("journal_dir", "/root/pjournal") + "/.sync_state";
}

static std::map<std::string, time_t> load_state() {
    std::map<std::string, time_t> state;
    std::istringstream in(read_whole_file(state_path()));
    std::string name;
    long long t = 0;
    while(in >> name >> t) if(is_journal_file(name)) state[name] = (time_t)t;
    return state;
}

static void save_state(const std::map<std::string, time_t> &state) {
    std::string s;
    for(const auto &p : state) s += p.first + " " + std::to_string((long long)p.second) + "\n";
    safe_write_file(state_path(), s);
}

WebdavSyncResult webdav_sync_journal() {
    if(base_url().empty()) return {false, "请先设置 WebDAV URL"};
    if(system("command -v curl >/dev/null 2>&1") != 0) return {false, "未找到 curl"};

    g_journal.begin();
    curl_mkcol();

    std::map<std::string, time_t> local;
    for(const auto &p : g_journal.list_file_mtimes()) local[p.first] = p.second;

    std::map<std::string, time_t> remote;
    for(const auto &f : list_remote()) remote[f.name] = f.mtime;

    auto prev = load_state();
    if(remote.empty() && !local.empty() && !prev.empty()) return {false, "远程列表为空，已停止以避免误删"};

    int uploaded = 0, downloaded = 0, deleted_local = 0, deleted_remote = 0, skipped = 0, failed = 0;
    std::map<std::string, time_t> next_state;
    std::set<std::string> all;
    for(const auto &p : local) all.insert(p.first);
    for(const auto &p : remote) all.insert(p.first);

    for(const auto &name : all) {
        bool le = local.count(name) > 0;
        bool re = remote.count(name) > 0;
        bool pe = prev.count(name) > 0;
        time_t lm = le ? local[name] : 0;
        time_t rm = re ? remote[name] : 0;
        time_t pm = pe ? prev[name] : 0;
        std::string local_path = g_journal.base_path() + "/" + name;

        if(le && re) {
            long diff = std::labs((long)(lm - rm));
            if((pm > 0 && lm == pm && rm == pm) || diff <= 60) {
                skipped++;
                next_state[name] = lm ? lm : rm;
            } else if(lm > rm) {
                if(curl_upload(name, local_path)) { uploaded++; next_state[name] = lm; }
                else { failed++; next_state[name] = pm ? pm : lm; }
            } else {
                std::string tmp = local_path + ".webdav.tmp";
                if(curl_download(name, tmp) && g_journal.save_entry_raw(name, read_whole_file(tmp), false)) {
                    remove(tmp.c_str());
                    struct utimbuf ut {rm, rm};
                    utime(local_path.c_str(), &ut);
                    downloaded++;
                    next_state[name] = rm;
                } else {
                    remove(tmp.c_str());
                    failed++;
                    next_state[name] = pm ? pm : lm;
                }
            }
        } else if(le && !re) {
            if(pe) {
                if(g_journal.delete_entry(name)) deleted_local++;
                else failed++;
            } else if(curl_upload(name, local_path)) {
                uploaded++;
                next_state[name] = lm;
            } else failed++;
        } else if(!le && re) {
            if(pe) {
                if(curl_delete(name)) deleted_remote++;
                else failed++;
            } else {
                std::string tmp = local_path + ".webdav.tmp";
                if(curl_download(name, tmp) && g_journal.save_entry_raw(name, read_whole_file(tmp), false)) {
                    remove(tmp.c_str());
                    struct utimbuf ut {rm, rm};
                    utime(local_path.c_str(), &ut);
                    downloaded++;
                    next_state[name] = rm;
                } else {
                    remove(tmp.c_str());
                    failed++;
                }
            }
        }
    }
    save_state(next_state);
    std::string msg = "上传" + std::to_string(uploaded) + " 下载" + std::to_string(downloaded) +
                      " 跳过" + std::to_string(skipped) + " 删除本地" + std::to_string(deleted_local) +
                      " 删除远端" + std::to_string(deleted_remote);
    if(failed > 0) msg += " 失败" + std::to_string(failed);
    return {failed == 0, msg};
}
