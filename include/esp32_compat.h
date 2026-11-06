#pragma once

// ESP32 源码里的驼峰封装(实现见 src/esp32_compat.cpp)。从 port/esp32_main
// 移植过来的文件按原名 include, 避免与 include/safe_file.h 的蛇形 API 混淆。
#include <string>

bool safeWriteFile(const std::string &path, const std::string &content);
void repairSafeWriteFile(const std::string &path);
std::string readWholeFile(const std::string &path);
bool fileExists(const std::string &path);
bool ensureDirPath(const std::string &path);
