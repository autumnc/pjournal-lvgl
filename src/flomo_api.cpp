// Flomo Web API 客户端。协议部分逐字移植自原 C1ancher/Flomo 的 src/api.cpp
// (那套已经在真机上跑通过),只把两处依赖换掉:
//   mbedtls 的 MD5  → pjournal 的 hash::md5_hex(纯 C++,不拉 OpenSSL)
//   esp_http_client → flomo_http.cpp 的 curl 子进程
#include "flomo_api.h"

#include "flomo_http.h"
#include "hash_utils.h"
#include "json_utils.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr const char *kBase = "https://flomoapp.com/api/v1";
constexpr const char *kSecret = "dbbc3dd73364b4084c3a69346e0ce2b2";

std::string now_seconds() {
    return std::to_string((long long)std::time(nullptr));
}

std::map<std::string, std::string> base_params() {
    return {{"timestamp", now_seconds()},
            {"api_key", "flomo_web"},
            {"app_version", "4.1"},
            {"platform", "web"},
            {"webp", "1"}};
}

// 把所有参数按 key 排序(靠 std::map 的顺序)、跳过空值,用 & 连起来再拼上密钥取 MD5。
std::string sign(const std::map<std::string, std::string> &p) {
    std::string raw;
    bool first = true;
    for(const auto &[k, v] : p) {
        if(v.empty()) continue;
        if(!first) raw += '&';
        first = false;
        raw += k + "=" + v;
    }
    return hash::md5_hex(raw + kSecret);
}

std::string urlenc(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for(unsigned char c : s) {
        if(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string query(const std::map<std::string, std::string> &p) {
    std::string q;
    bool first = true;
    for(const auto &[k, v] : p) {
        if(!first) q += '&';
        first = false;
        q += urlenc(k) + "=" + urlenc(v);
    }
    return q;
}

std::string body_json(const std::map<std::string, std::string> &p) {
    std::string body = "{";
    bool first = true;
    for(const auto &[k, v] : p) {
        if(!first) body += ",";
        first = false;
        body += "\"" + json_escape(k) + "\":\"" + json_escape(v) + "\"";
    }
    body += "}";
    return body;
}

std::string snippet(const std::string &s) {
    std::string out = s.substr(0, 80);
    for(char &c : out)
        if(c == '\n' || c == '\r' || c == '\t') c = ' ';
    return out;
}

ApiResult parse_response(int status, const std::string &body) {
    if(status < 200 || status >= 300)
        return {false, "HTTP " + std::to_string(status) + " " + snippet(body), {}};
    if(body.empty()) return {true, "", {}};
    JsonValue root = JsonValue::parse(body);
    int code = -1;
    if(root["code"].isNumber()) code = root["code"].asInt(-1);
    else if(root["code"].isString()) code = std::atoi(root["code"].asString().c_str());
    if(code == 0) return {true, "", root["data"]};
    std::string msg = root["message"].asString(root["msg"].asString());
    if(msg.empty()) msg = "API code " + std::to_string(code) + " " + snippet(body);
    return {false, msg, root};
}

std::string html_escape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for(char c : text) {
        switch(c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c; break;
        }
    }
    return out;
}

// **粗** → <strong>,__下划线__ → <u>,==高亮== → <mark>
std::string apply_inline_formats(std::string s) {
    static const struct {
        const char *marker;
        const char *open;
        const char *close;
    } kFormats[] = {
        {"**", "<strong>", "</strong>"},
        {"__", "<u>", "</u>"},
        {"==", "<mark>", "</mark>"},
    };
    for(const auto &f : kFormats) {
        size_t mlen = std::strlen(f.marker);
        size_t pos = 0;
        while((pos = s.find(f.marker, pos)) != std::string::npos) {
            size_t end = s.find(f.marker, pos + mlen);
            if(end == std::string::npos) break;
            std::string inner = s.substr(pos + mlen, end - pos - mlen);
            s.replace(pos, end - pos + mlen, std::string(f.open) + inner + f.close);
            pos += std::strlen(f.open) + inner.size();
        }
    }
    return s;
}

// 有序列表的标号:一串数字后面跟 ". " 或 "、"。命中就把 content 指到标号之后,
// 返回是否命中。
bool ordered_marker_end(const std::string &s, size_t &content) {
    size_t i = 0;
    while(i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if(i == 0 || i >= s.size()) return false;
    if(s[i] == '.') {
        if(i + 1 >= s.size() || s[i + 1] != ' ') return false;
        content = i + 2;
        return true;
    }
    // U+3001 '、'(UTF-8 E3 80 81),后面可以再跟一个空格
    if(i + 2 < s.size() && (unsigned char)s[i] == 0xE3 && (unsigned char)s[i + 1] == 0x80 &&
       (unsigned char)s[i + 2] == 0x81) {
        content = i + 3;
        if(content < s.size() && s[content] == ' ') ++content;
        return true;
    }
    return false;
}

}  // namespace

ApiResult FlomoApi::request(const std::string &method, const std::string &path,
                            const std::map<std::string, std::string> &extra) {
    std::map<std::string, std::string> params = base_params();
    for(const auto &kv : extra) params[kv.first] = kv.second;
    params["sign"] = sign(params);

    std::string url = std::string(kBase) + "/" + path;
    // GET / DELETE 把参数(含 sign)挂在 query 上;PUT / POST 才走 body。
    // DELETE 也要带 sign,少一个参数服务器直接判签名错。
    bool has_body = !(method == "GET" || method == "DELETE");
    std::string body;
    if(has_body) body = body_json(params);
    else url += "?" + query(params);

    std::vector<std::string> headers;
    if(!token_.empty()) headers.push_back("Authorization: Bearer " + token_);

    HttpResult h = http_request(method, url, headers, body);
    if(!h.ok) return {false, h.error, {}};
    return parse_response(h.status, h.body);
}

ApiResult FlomoApi::login(const std::string &email, const std::string &password) {
    FlomoApi api("");
    return api.request("POST", "user/login_by_email",
                       {{"email", email},
                        {"password", password},
                        {"wechat_union_id", ""},
                        {"wechat_oa_open_id", ""}});
}

ApiResult FlomoApi::list_updated(long long cursorUpdatedAt, const std::string &cursorSlug) {
    std::map<std::string, std::string> p{
        {"limit", "200"},
        {"latest_updated_at", std::to_string(cursorUpdatedAt)},
        {"tz", "8:0"},
    };
    if(!cursorSlug.empty()) p["latest_slug"] = cursorSlug;
    return request("GET", "memo/updated/", p);
}

ApiResult FlomoApi::get_memo(const std::string &slug) {
    return request("GET", "memo/" + slug, {});
}

ApiResult FlomoApi::related(const std::string &slug) {
    return request("GET", "memo/" + slug + "/recommended", {{"type", "1"}});
}

ApiResult FlomoApi::daily_review_slugs() {
    return request("GET", "memo/notify_of_today/", {});
}

ApiResult FlomoApi::user_me() {
    return request("GET", "user/me", {});
}

ApiResult FlomoApi::tag_tree() {
    return request("GET", "tag/tree", {});
}

ApiResult FlomoApi::rename_tag(const std::string &oldTag, const std::string &newTag) {
    return request("POST", "tag/rename",
                   {{"old_tag", oldTag}, {"new_tag", newTag}, {"tz", "8:0"}});
}

ApiResult FlomoApi::create_memo(const std::string &content) {
    return request("PUT", "memo",
                   {{"content", text_to_html(content)}, {"source", "web"}, {"tz", "8:0"}});
}

ApiResult FlomoApi::update_memo(const std::string &slug, const std::string &content, int pin) {
    std::map<std::string, std::string> p{
        {"content", text_to_html(content)}, {"source", "web"}, {"tz", "8:0"}};
    if(pin >= 0) p["pin"] = std::to_string(pin);
    return request("PUT", "memo/" + slug, p);
}

ApiResult FlomoApi::delete_memo(const std::string &slug) {
    return request("DELETE", "memo/" + slug, {});
}

std::string html_to_text(const std::string &html) {
    std::string out;
    bool tag = false;
    int listKind = 0;      // 1 <ul>,2 <ol>
    int orderCounter = 0;
    for(size_t i = 0; i < html.size(); ++i) {
        // 空段落是一个空行,不是两个换行
        if(html.compare(i, 13, "<p><br /></p>") == 0) { out += '\n'; i += 12; continue; }
        if(html.compare(i, 12, "<p><br/></p>") == 0) { out += '\n'; i += 11; continue; }
        if(html.compare(i, 11, "<p><br></p>") == 0) { out += '\n'; i += 10; continue; }
        // 把 text_to_html 吃掉的 markdown 记号还原回来,编辑才能往返
        if(html.compare(i, 8, "<strong>") == 0) { out += "**"; i += 7; continue; }
        if(html.compare(i, 9, "</strong>") == 0) { out += "**"; i += 8; continue; }
        if(html.compare(i, 3, "<u>") == 0) { out += "__"; i += 2; continue; }
        if(html.compare(i, 4, "</u>") == 0) { out += "__"; i += 3; continue; }
        if(html.compare(i, 6, "<mark>") == 0) { out += "=="; i += 5; continue; }
        if(html.compare(i, 7, "</mark>") == 0) { out += "=="; i += 6; continue; }
        if(html.compare(i, 4, "<ul>") == 0) { listKind = 1; i += 3; continue; }
        if(html.compare(i, 5, "</ul>") == 0) { listKind = 0; i += 4; continue; }
        if(html.compare(i, 4, "<ol>") == 0) { listKind = 2; orderCounter = 0; i += 3; continue; }
        if(html.compare(i, 5, "</ol>") == 0) { listKind = 0; i += 4; continue; }
        if(html.compare(i, 4, "<li>") == 0) {
            out += listKind == 2 ? std::to_string(++orderCounter) + ". " : "- ";
            i += 3;
            continue;
        }
        if(html.compare(i, 5, "</li>") == 0) { out += '\n'; i += 4; continue; }
        if(html.compare(i, 4, "<br>") == 0) { out += '\n'; i += 3; continue; }
        if(html.compare(i, 5, "<br/>") == 0) { out += '\n'; i += 4; continue; }
        if(html.compare(i, 6, "<br />") == 0) { out += '\n'; i += 5; continue; }
        if(html.compare(i, 4, "</p>") == 0) { out += '\n'; i += 3; continue; }
        if(html[i] == '<') { tag = true; continue; }
        if(html[i] == '>') { tag = false; continue; }
        if(!tag) out += html[i];
    }
    static const struct {
        const char *entity;
        const char *plain;
    } kEntities[] = {
        {"&nbsp;", " "}, {"&lt;", "<"},   {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"},
    };
    for(const auto &e : kEntities) {
        size_t pos = 0;
        size_t elen = std::strlen(e.entity);
        while((pos = out.find(e.entity, pos)) != std::string::npos) {
            out.replace(pos, elen, e.plain);
            pos += std::strlen(e.plain);
        }
    }
    while(!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// 纯文本 → Flomo 的 HTML。空行变 <p><br></p>;"- " / "* " 连续行成 <ul>;
// "1. " / "1、" 连续行成 <ol>。行内的 **粗** / __下划线__ / ==高亮== 变成标签。
// 其余(代码块、反引号、标题……)一律当普通文字留着。
std::string text_to_html(const std::string &text) {
    if(text.empty()) return "<p><br></p>";

    std::vector<std::string> lines;
    size_t start = 0;
    for(size_t i = 0; i <= text.size(); ++i) {
        if(i == text.size() || text[i] == '\n') {
            std::string line = text.substr(start, i - start);
            if(!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            start = i + 1;
        }
    }

    std::string out;
    int listKind = 0;   // 0 无,1 <ul>,2 <ol>
    for(const std::string &line : lines) {
        size_t from = line.find_first_not_of(" \t");
        size_t to = line.find_last_not_of(" \t");
        std::string trimmed = from == std::string::npos ? "" : line.substr(from, to - from + 1);
        int kind = 0;
        size_t content = 0;
        if(trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*') && trimmed[1] == ' ') {
            kind = 1;
            content = 2;
        } else if(ordered_marker_end(trimmed, content)) {
            kind = 2;
        }
        if(kind != 0) {
            if(listKind != kind) {
                out += listKind == 1 ? "</ul>" : (listKind == 2 ? "</ol>" : "");
                out += kind == 1 ? "<ul>" : "<ol>";
                listKind = kind;
            }
            out += "<li>" + apply_inline_formats(html_escape(trimmed.substr(content))) + "</li>";
        } else {
            out += listKind == 1 ? "</ul>" : (listKind == 2 ? "</ol>" : "");
            listKind = 0;
            if(trimmed.empty()) out += "<p><br></p>";
            else out += "<p>" + apply_inline_formats(html_escape(trimmed)) + "</p>";
        }
    }
    out += listKind == 1 ? "</ul>" : (listKind == 2 ? "</ol>" : "");
    return out;
}

std::vector<std::string> extract_tags(const std::string &text) {
    std::vector<std::string> tags;
    for(size_t i = 0; i < text.size(); ++i) {
        if(text[i] != '#') continue;
        size_t j = i + 1;
        while(j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '#') ++j;
        if(j > i + 1) tags.push_back(text.substr(i + 1, j - i - 1));
        i = j;
    }
    return tags;
}

Memo memo_from_json(const JsonValue &v) {
    Memo m;
    m.slug = v["slug"].asString();
    m.contentHtml = v["content"].asString();
    m.contentText = html_to_text(m.contentHtml);
    m.createdAt = v["created_at"].asString();
    m.updatedAt = v["updated_at"].asString();
    m.deleted = !v["deleted_at"].isNull() && !v["deleted_at"].asString().empty();
    const JsonValue &pin = v["pin"];
    if(pin.isNumber()) m.pin = pin.asInt(0);
    else if(pin.isString()) m.pin = std::atoi(pin.asString().c_str());
    const JsonValue &tags = v["tags"];
    if(tags.isArray())
        for(size_t i = 0; i < tags.size(); ++i) m.tags.push_back(tags[i].asString());
    if(m.tags.empty()) m.tags = extract_tags(m.contentText);
    return m;
}

long long flomo_cursor_from_stamp(const std::string &stamp) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if(std::sscanf(stamp.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
        return 0;
    std::tm tm {};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    // 墙上时间是 UTC+8:先把它当 UTC 折成秒,再减 8 小时得到真正的 unix 时间。
    time_t as_utc = timegm(&tm);
    if(as_utc == (time_t)-1) return 0;
    return (long long)as_utc - 8LL * 3600LL;
}

Memo FlomoApi::memo_from_result(const ApiResult &r) {
    Memo m = memo_from_json(r.data);
    if(!m.slug.empty()) return m;
    m = memo_from_json(r.data["memo"]);
    if(!m.slug.empty()) return m;
    return memo_from_json(r.data["data"]);
}
