#include "safe_file.h"

#include <string>

bool safeWriteFile(const std::string &path, const std::string &content) {
    return safe_write_file(path, content);
}

void repairSafeWriteFile(const std::string &path) {
    repair_safe_write_file(path);
}

std::string readWholeFile(const std::string &path) {
    return read_whole_file(path);
}

bool fileExists(const std::string &path) {
    return file_exists(path);
}

bool ensureDirPath(const std::string &path) {
    return ensure_dir_path(path);
}
