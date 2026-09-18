#include "safe_file.h"

#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

bool file_exists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool ensure_dir_path(const std::string &path) {
    if(path.empty()) return false;
    std::string cur;
    size_t pos = 0;
    if(path[0] == '/') {
        cur = "/";
        pos = 1;
    }
    while(pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if(!part.empty()) {
            if(!cur.empty() && cur.back() != '/') cur += '/';
            cur += part;
            if(mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
        }
        if(slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

void repair_safe_write_file(const std::string &path) {
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";
    if(!file_exists(path) && file_exists(bak)) rename(bak.c_str(), path.c_str());
    if(file_exists(tmp)) remove(tmp.c_str());
}

std::string read_whole_file(const std::string &path) {
    repair_safe_write_file(path);
    FILE *f = fopen(path.c_str(), "rb");
    if(!f) return "";
    std::string out;
    char buf[4096];
    size_t n = 0;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static bool flush_close(FILE *f) {
    bool ok = fflush(f) == 0;
    int fd = fileno(f);
    if(fd >= 0 && fsync(fd) != 0) {
        /* Some flash-backed filesystems report fsync oddly; fflush success is
         * enough to proceed, mirroring the ESP version's recovery behavior. */
    }
    if(fclose(f) != 0) ok = false;
    return ok;
}

bool safe_write_file(const std::string &path, const std::string &content) {
    size_t slash = path.rfind('/');
    if(slash != std::string::npos && slash > 0 && !ensure_dir_path(path.substr(0, slash))) return false;

    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";
    repair_safe_write_file(path);
    remove(tmp.c_str());

    FILE *f = fopen(tmp.c_str(), "wb");
    if(!f) return false;
    const bool wrote = fwrite(content.data(), 1, content.size(), f) == content.size();
    if(!wrote || !flush_close(f)) {
        remove(tmp.c_str());
        return false;
    }

    const bool had_original = file_exists(path);
    remove(bak.c_str());
    if(had_original && rename(path.c_str(), bak.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    if(rename(tmp.c_str(), path.c_str()) != 0) {
        if(had_original) rename(bak.c_str(), path.c_str());
        remove(tmp.c_str());
        return false;
    }
    if(had_original) remove(bak.c_str());
    return true;
}
