#include "file_edit.h"
#include "settings_manager.h"
#include "safe_file.h"
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

static const char *TAG = "FileEdit";
static const char *LAST_FILE_KEY = "file_edit_last";

bool g_fileEdit = false;

std::string fileEditDir() {
    return "/sdcard/files";
}

static bool isHiddenOrTemp(const std::string &name) {
    return name.empty() || name[0] == '.' ||
           (name.size() >= 4 && name.substr(name.size() - 4) == ".tmp") ||
           (name.size() >= 4 && name.substr(name.size() - 4) == ".bak");
}

std::string fileEditSanitizeName(const std::string &name) {
    std::string out;
    for (unsigned char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c < 0x20) continue;
        out.push_back((char)c);
    }
    size_t start = out.find_first_not_of(" \t\r\n");
    size_t end = out.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) out.clear();
    else out = out.substr(start, end - start + 1);
    if (out.empty()) out = "untitled.txt";
    if (out.find('.') == std::string::npos) out += ".txt";
    return out;
}

std::string fileEditPath(const std::string &name) {
    return fileEditDir() + "/" + fileEditSanitizeName(name);
}

std::vector<FileEditEntry> fileEditList() {
    ensureDirPath(fileEditDir());
    std::vector<FileEditEntry> entries;
    DIR *d = opendir(fileEditDir().c_str());
    if (!d) return entries;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (isHiddenOrTemp(name)) continue;
        std::string path = fileEditDir() + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        entries.push_back({name, (size_t)st.st_size});
    }
    closedir(d);
    std::sort(entries.begin(), entries.end(),
              [](const FileEditEntry &a, const FileEditEntry &b) {
                  return a.name < b.name;
              });
    return entries;
}

std::string fileEditLastName() {
    std::string name = fileEditSanitizeName(g_settings.getString(LAST_FILE_KEY, "notes.txt"));
    if (!fileExists(fileEditPath(name))) {
        auto entries = fileEditList();
        if (!entries.empty()) name = entries.front().name;
    }
    return name;
}

void fileEditSetLastName(const std::string &name) {
    g_settings.setString(LAST_FILE_KEY, fileEditSanitizeName(name));
}

bool fileEditInit() {
    if (!ensureDirPath(fileEditDir())) return false;
    std::string last = fileEditLastName();
    if (!fileExists(fileEditPath(last)) && !safeWriteFile(fileEditPath(last), "")) {
        ESP_LOGE(TAG, "Cannot create %s", fileEditPath(last).c_str());
        return false;
    }
    fileEditSetLastName(last);
    return true;
}

std::string fileEditLoad(const std::string &name) {
    std::string path = fileEditPath(name);
    repairSafeWriteFile(path);
    return readWholeFile(path);
}

bool fileEditSave(const std::string &name, const std::string &text) {
    std::string clean = fileEditSanitizeName(name);
    bool ok = safeWriteFile(fileEditPath(clean), text);
    if (ok) fileEditSetLastName(clean);
    return ok;
}

bool fileEditCreate(const std::string &name) {
    std::string path = fileEditPath(name);
    if (fileExists(path)) return false;
    return safeWriteFile(path, "");
}

bool fileEditRemove(const std::string &name) {
    std::string clean = fileEditSanitizeName(name);
    bool ok = remove(fileEditPath(clean).c_str()) == 0;
    if (ok && fileEditLastName() == clean) {
        auto entries = fileEditList();
        fileEditSetLastName(entries.empty() ? "notes.txt" : entries.front().name);
    }
    return ok;
}

bool fileEditRename(const std::string &oldName, const std::string &newName) {
    std::string oldClean = fileEditSanitizeName(oldName);
    std::string newClean = fileEditSanitizeName(newName);
    if (oldClean == newClean || fileExists(fileEditPath(newClean))) return false;
    bool ok = rename(fileEditPath(oldClean).c_str(), fileEditPath(newClean).c_str()) == 0;
    if (ok && fileEditLastName() == oldClean) fileEditSetLastName(newClean);
    return ok;
}
