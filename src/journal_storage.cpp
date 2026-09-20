#include "journal_storage.h"

#include "safe_file.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <set>
#include <sys/stat.h>

JournalStorage g_journal;
static std::recursive_mutex g_journal_mutex;

static bool journal_ext(const std::string &fn) {
    size_t dot = fn.rfind('.');
    if(dot == std::string::npos) return false;
    std::string ext = fn.substr(dot);
    return ext == ".txt" || ext == ".md";
}

static std::string stem_of(const std::string &fn) {
    size_t dot = fn.rfind('.');
    return dot == std::string::npos ? fn : fn.substr(0, dot);
}

static bool safe_name(const std::string &fn) {
    return !fn.empty() && fn.find('/') == std::string::npos && fn.find("..") == std::string::npos;
}

static std::vector<std::string> list_journal_files(const std::string &dir) {
    std::vector<std::string> files;
    DIR *d = opendir(dir.c_str());
    if(!d) return files;
    dirent *de = nullptr;
    while((de = readdir(d)) != nullptr) {
        std::string fn = de->d_name;
        if(fn.empty() || fn[0] == '.' || !journal_ext(fn)) continue;
        files.push_back(fn);
    }
    closedir(d);
    std::sort(files.begin(), files.end(), std::greater<std::string>());
    return files;
}

static std::string history_dir_for(const std::string &base, const std::string &filename) {
    return base + "/.history/" + stem_of(filename);
}

static bool save_history(const std::string &base, const std::string &filename, const std::string &old_content) {
    std::string dir = history_dir_for(base, filename);
    if(!ensure_dir_path(dir)) return false;
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", &local);
    std::string path = dir + "/" + ts + ".txt";
    for(int i = 1; file_exists(path) && i < 100; ++i) path = dir + "/" + ts + "_" + std::to_string(i) + ".txt";
    bool ok = safe_write_file(path, old_content);

    DIR *d = opendir(dir.c_str());
    if(d) {
        std::vector<std::string> files;
        dirent *de = nullptr;
        while((de = readdir(d)) != nullptr) {
            std::string fn = de->d_name;
            if(safe_name(fn) && journal_ext(fn)) files.push_back(fn);
        }
        closedir(d);
        std::sort(files.begin(), files.end());
        while(files.size() > 10) {
            remove((dir + "/" + files.front()).c_str());
            files.erase(files.begin());
        }
    }
    return ok;
}

bool JournalStorage::begin() {
    ensure_dir();
    return true;
}

std::string JournalStorage::base_path() const {
    return g_settings.journal_dir();
}

void JournalStorage::ensure_dir() {
    ensure_dir_path(base_path());
}

bool JournalStorage::save_entry(const std::string &text) {
    std::lock_guard<std::recursive_mutex> lock(g_journal_mutex);
    ensure_dir();
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", &local);
    return safe_write_file(base_path() + "/" + ts + ".txt", text);
}

bool JournalStorage::save_entry_raw(const std::string &filename, const std::string &content, bool create_history) {
    if(!safe_name(filename) || !journal_ext(filename)) return false;
    std::lock_guard<std::recursive_mutex> lock(g_journal_mutex);
    ensure_dir();
    const std::string path = base_path() + "/" + filename;
    if(create_history && g_settings.version_history() && file_exists(path)) {
        std::string old = read_whole_file(path);
        if(old != content) save_history(base_path(), filename, old);
    }
    return safe_write_file(path, content);
}

std::vector<JournalEntry> JournalStorage::list_entries() {
    std::lock_guard<std::recursive_mutex> lock(g_journal_mutex);
    ensure_dir();
    std::vector<JournalEntry> entries;
    for(const auto &fn : list_journal_files(base_path())) {
        JournalEntry e;
        e.filename = fn;
        e.date = fn.size() >= 10 ? fn.substr(0, 10) : "";
        e.full_text = read_whole_file(base_path() + "/" + fn);
        size_t first_nl = e.full_text.find('\n');
        std::string first = first_nl == std::string::npos ? e.full_text : e.full_text.substr(0, first_nl);
        if(first.find("提示词:") == 0) e.title = "提示写作";
        else if(first.find("自由写作") != std::string::npos) e.title = "自由写作";
        else e.title = first.empty() ? "日记" : first.substr(0, 32);
        size_t body_start = e.full_text.find("\n\n");
        std::string body = body_start == std::string::npos ? e.full_text : e.full_text.substr(body_start + 2);
        size_t pos = 0;
        while(pos < body.size()) {
            size_t nl = body.find('\n', pos);
            std::string line = nl == std::string::npos ? body.substr(pos) : body.substr(pos, nl - pos);
            if(!line.empty() && line.find("日期:") != 0 && line.find("字数:") != 0 && line.find("提示词:") != 0 && line != "自由写作") {
                e.preview = line.substr(0, 80);
                break;
            }
            if(nl == std::string::npos) break;
            pos = nl + 1;
        }
        entries.push_back(e);
    }
    return entries;
}

std::vector<std::pair<std::string, time_t>> JournalStorage::list_file_mtimes() {
    std::lock_guard<std::recursive_mutex> lock(g_journal_mutex);
    std::vector<std::pair<std::string, time_t>> out;
    for(const auto &fn : list_journal_files(base_path())) {
        struct stat st {};
        if(stat((base_path() + "/" + fn).c_str(), &st) == 0) out.push_back({fn, st.st_mtime});
    }
    return out;
}

std::string JournalStorage::read_entry(const std::string &filename) {
    if(!safe_name(filename) || !journal_ext(filename)) return "";
    std::lock_guard<std::recursive_mutex> lock(g_journal_mutex);
    return read_whole_file(base_path() + "/" + filename);
}

bool JournalStorage::delete_entry(const std::string &filename) {
    if(!safe_name(filename) || !journal_ext(filename)) return false;
    std::lock_guard<std::recursive_mutex> lock(g_journal_mutex);
    return remove((base_path() + "/" + filename).c_str()) == 0;
}

bool JournalStorage::has_entry(const std::string &date) {
    for(const auto &fn : list_journal_files(base_path())) if(fn.substr(0, 10) == date) return true;
    return false;
}

int JournalStorage::count_today() {
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", &local);
    int n = 0;
    for(const auto &fn : list_journal_files(base_path())) if(fn.substr(0, 10) == today) ++n;
    return n;
}

int JournalStorage::total_entries() {
    return (int)list_journal_files(base_path()).size();
}

int JournalStorage::streak() {
    std::set<std::string> dates;
    for(const auto &fn : list_journal_files(base_path())) if(fn.size() >= 10) dates.insert(fn.substr(0, 10));
    time_t now = time(nullptr);
    int s = 0;
    for(int i = 0; i < 365; ++i) {
        time_t t = now - i * 86400;
        tm local {};
        localtime_r(&t, &local);
        char d[16];
        strftime(d, sizeof(d), "%Y-%m-%d", &local);
        if(!dates.count(d)) break;
        ++s;
    }
    return s;
}

bool JournalStorage::save_recovery_draft(const std::string &content, const std::string &meta) {
    std::string dir = base_path() + "/.recovery";
    return ensure_dir_path(dir) && safe_write_file(dir + "/editor.tmp", content) && safe_write_file(dir + "/editor.meta", meta);
}

bool JournalStorage::load_recovery_draft(std::string &content, std::string &meta) {
    std::string dir = base_path() + "/.recovery";
    content = read_whole_file(dir + "/editor.tmp");
    meta = read_whole_file(dir + "/editor.meta");
    return !content.empty() || !meta.empty();
}

void JournalStorage::clear_recovery_draft() {
    std::string dir = base_path() + "/.recovery";
    remove((dir + "/editor.tmp").c_str());
    remove((dir + "/editor.meta").c_str());
}

std::vector<JournalHistoryVersion> JournalStorage::list_history_versions(const std::string &filename) {
    std::vector<JournalHistoryVersion> out;
    if(!safe_name(filename) || !journal_ext(filename)) return out;
    std::string dir = history_dir_for(base_path(), filename);
    DIR *d = opendir(dir.c_str());
    if(!d) return out;
    dirent *de = nullptr;
    while((de = readdir(d)) != nullptr) {
        std::string fn = de->d_name;
        if(!safe_name(fn) || !journal_ext(fn)) continue;
        struct stat st {};
        if(stat((dir + "/" + fn).c_str(), &st) == 0) out.push_back({fn, st.st_mtime, (size_t)st.st_size});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.filename > b.filename; });
    return out;
}

std::string JournalStorage::read_history_version(const std::string &filename, const std::string &history_filename) {
    if(!safe_name(filename) || !journal_ext(filename) || !safe_name(history_filename) || !journal_ext(history_filename)) return "";
    return read_whole_file(history_dir_for(base_path(), filename) + "/" + history_filename);
}

bool JournalStorage::restore_history_version(const std::string &filename, const std::string &history_filename) {
    if(!safe_name(filename) || !journal_ext(filename) || !safe_name(history_filename) || !journal_ext(history_filename)) return false;
    struct stat st {};
    if(stat((history_dir_for(base_path(), filename) + "/" + history_filename).c_str(), &st) != 0) return false;
    std::string content = read_history_version(filename, history_filename);
    return save_entry_raw(filename, content, true);
}

bool JournalStorage::delete_history_version(const std::string &filename, const std::string &history_filename) {
    if(!safe_name(filename) || !journal_ext(filename) || !safe_name(history_filename) || !journal_ext(history_filename)) return false;
    return remove((history_dir_for(base_path(), filename) + "/" + history_filename).c_str()) == 0;
}
