#pragma once

#include <string>

bool utf8_valid(const std::string &s);
int utf8_char_count(const std::string &s);
std::string extract_body(const std::string &content);
std::string make_journal_text(const std::string &prompt, const std::string &body);
// 从已有日记的头部取回「提示词」;不是提示写作返回空串。
std::string journal_prompt(const std::string &content);
