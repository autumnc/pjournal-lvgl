#pragma once

#include <string>

bool file_exists(const std::string &path);
bool ensure_dir_path(const std::string &path);
bool safe_write_file(const std::string &path, const std::string &content);
void repair_safe_write_file(const std::string &path);
std::string read_whole_file(const std::string &path);
