#include "text_utils.h"

#include <ctime>

bool utf8_valid(const std::string &s) {
    size_t i = 0;
    while(i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t n = 0;
        if(c <= 0x7F) n = 1;
        else if((c & 0xE0) == 0xC0) n = 2;
        else if((c & 0xF0) == 0xE0) n = 3;
        else if((c & 0xF8) == 0xF0) n = 4;
        else return false;
        if(i + n > s.size()) return false;
        for(size_t j = 1; j < n; ++j) if(((unsigned char)s[i + j] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

int utf8_char_count(const std::string &s) {
    int count = 0;
    for(size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = 1;
        if((c & 0xE0) == 0xC0) n = 2;
        else if((c & 0xF0) == 0xE0) n = 3;
        else if((c & 0xF8) == 0xF0) n = 4;
        i += n;
        ++count;
    }
    return count;
}

std::string extract_body(const std::string &content) {
    size_t body = content.find("\n\n");
    return body == std::string::npos ? content : content.substr(body + 2);
}

std::string make_journal_text(const std::string &prompt, const std::string &body) {
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
    std::string out;
    if(prompt.empty()) out = "自由写作\n";
    else out = "提示词: " + prompt + "\n";
    out += "日期: ";
    out += date;
    out += "\n字数: ";
    out += std::to_string(utf8_char_count(body));
    out += "\n\n";
    out += body;
    return out;
}
