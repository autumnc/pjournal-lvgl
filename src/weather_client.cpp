#include "weather_client.h"

#include "flomo_http.h"
#include "json_parser.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

static const char *WEATHER_API_URL = "https://uapis.cn/api/v1/misc/weather?city=";
static constexpr int kWeatherTimeoutMs = 10000;
// uapis 的免费接口带自适应并发门控,高峰期会随机回 429。官方建议指数退避
// 最多重试三次;429 响应很快,重试成本低,能把偶发失败压下去。
static constexpr int kWeatherAttempts = 4;
static constexpr int kRetryBaseDelayMs = 800;

static std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for(unsigned char c : s) {
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// 天气文本会直接插进编辑器的一行里:去掉换行/制表符并限制长度,
// 防止异常响应破坏正文的行结构或把超长文本灌进去。
static std::string sanitize_weather_text(const std::string &in) {
    std::string out;
    out.reserve(std::min<size_t>(in.size(), 96));
    for(size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        size_t len = (c < 0x80) ? 1
                   : ((c >> 5) == 0x6) ? 2
                   : ((c >> 4) == 0xE) ? 3
                   : ((c >> 3) == 0x1E) ? 4
                                        : 1;
        if(i + len > in.size()) len = 1;  // 截断的 UTF-8 序列按单字节处理
        if(out.size() + len > 96) break;
        if(len == 1 && (c == '\r' || c == '\n' || c == '\t')) out += ' ';
        else out.append(in, i, len);
        i += len;
    }
    for(char &ch : out)
        if(ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    size_t b = 0, e = out.size();
    while(b < e && isspace((unsigned char)out[b])) b++;
    while(e > b && isspace((unsigned char)out[e - 1])) e--;
    return out.substr(b, e - b);
}

WeatherResult weather_fetch_current(const std::string &city) {
    WeatherResult result;
    size_t begin = 0, end = city.size();
    while(begin < end && isspace((unsigned char)city[begin])) begin++;
    while(end > begin && isspace((unsigned char)city[end - 1])) end--;
    if(begin == end) {
        result.error = "请先设置天气地区";
        return result;
    }
    std::string query_city = city.substr(begin, end - begin);
    std::string url = std::string(WEATHER_API_URL) + url_encode(query_city);

    HttpResult http;
    for(int attempt = 0; attempt < kWeatherAttempts; ++attempt) {
        if(attempt > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kRetryBaseDelayMs << (attempt - 1)));
        http = http_request("GET", url,
                            {"Accept: application/json", "User-Agent: pjournal-lvgl/1.0"},
                            "", kWeatherTimeoutMs);
        // 连不上/超时再重试也没用;只有 429 和 5xx 值得退避后再来。
        if(!http.ok || (http.status != 429 && http.status < 500)) break;
    }
    if(!http.ok) {
        result.error = http.error.empty() ? "网络请求失败" : http.error;
        return result;
    }
    if(http.status == 429) {
        result.error = "天气服务繁忙,请稍后重试";
        return result;
    }
    if(http.status != 200) {
        result.error = "天气服务不可用";
        return result;
    }

    JsonValue root = JsonValue::parse(http.body);
    if(!root.isObject() || !root["weather"].isString()) {
        result.error = "天气数据解析失败";
        return result;
    }

    std::string text = sanitize_weather_text(root["weather"].asString());
    if(text.empty()) {
        result.error = "天气数据为空";
        return result;
    }

    char temp[24] = "";
    if(root["temperature"].isNumber()) {
        double t = root["temperature"].asNumber();
        if(std::isfinite(t)) {
            if(std::fabs(t - std::round(t)) < 0.05)
                snprintf(temp, sizeof(temp), "%d℃", (int)std::lround(t));
            else
                snprintf(temp, sizeof(temp), "%.1f℃", t);
        }
    }

    result.summary = text;
    if(temp[0]) {
        result.summary += ' ';
        result.summary += temp;
    }
    result.success = true;
    return result;
}
