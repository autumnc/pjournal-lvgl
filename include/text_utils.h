#pragma once

#include <string>

bool utf8_valid(const std::string &s);
int utf8_char_count(const std::string &s);
std::string extract_body(const std::string &content);
std::string make_journal_text(const std::string &prompt, const std::string &body);
