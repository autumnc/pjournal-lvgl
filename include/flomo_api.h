#pragma once

#include "flomo_model.h"

#include "json_parser.h"

#include <map>
#include <string>
#include <vector>

struct ApiResult {
    bool ok = false;
    std::string message;   // 失败时给人看的一句话
    JsonValue data;        // 成功时响应里的 data
};

class FlomoApi {
public:
    explicit FlomoApi(std::string token) : token_(std::move(token)) {}

    static ApiResult login(const std::string &email, const std::string &password);

    // 增量同步:memo/updated/ 按 updated_at 升序翻页。cursor 用 UTC+8 的 unix 秒。
    ApiResult list_updated(long long cursorUpdatedAt, const std::string &cursorSlug);
    ApiResult get_memo(const std::string &slug);
    ApiResult related(const std::string &slug);
    ApiResult daily_review_slugs();
    ApiResult user_me();
    ApiResult tag_tree();
    ApiResult rename_tag(const std::string &oldTag, const std::string &newTag);

    ApiResult create_memo(const std::string &content);
    ApiResult update_memo(const std::string &slug, const std::string &content, int pin = -1);
    ApiResult delete_memo(const std::string &slug);

    // 返回的 JsonValue 里可能直接是 memo,也可能包在 memo/data 里(创建接口就是这样)。
    static Memo memo_from_result(const ApiResult &r);

private:
    ApiResult request(const std::string &method, const std::string &path,
                      const std::map<std::string, std::string> &params);

    std::string token_;
};

// 纯函数,协议层面的格式转换,与网络无关。
std::string html_to_text(const std::string &html);
std::string text_to_html(const std::string &text);
std::vector<std::string> extract_tags(const std::string &text);
Memo memo_from_json(const JsonValue &v);

// "YYYY-MM-DD HH:MM:SS"(UTC+8) → unix 秒。解析失败返回 0。
long long flomo_cursor_from_stamp(const std::string &stamp);
