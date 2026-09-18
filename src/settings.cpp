#include "settings.h"

#include "safe_file.h"

#include <cstdlib>
#include <map>
#include <mutex>

Settings g_settings;

static std::mutex g_mutex;
static std::map<std::string, std::string> g_cache;

static std::string home_dir() {
    const char *home = getenv("HOME");
    return home && *home ? home : "/root";
}

std::string Settings::settings_dir() const {
    return home_dir() + "/.pjournal-lvgl/settings";
}

bool Settings::begin() {
    return ensure_dir_path(settings_dir());
}

std::string Settings::get(const std::string &key, const std::string &def) const {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(key);
    if(it != g_cache.end()) return it->second.empty() ? def : it->second;
    std::string path = settings_dir() + "/" + key;
    std::string value = read_whole_file(path);
    while(!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    g_cache[key] = value;
    return value.empty() ? def : value;
}

void Settings::set(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensure_dir_path(settings_dir());
    if(safe_write_file(settings_dir() + "/" + key, value)) g_cache[key] = value;
}

std::string Settings::journal_dir() const { return get("journal_dir", home_dir() + "/pjournal"); }
std::string Settings::theme() const { return get("theme", "dark"); }
std::string Settings::font_file() const { return get("font_file", ""); }
int Settings::font_size() const {
    int n = atoi(get("font_size", "24").c_str());
    if(n < 12) n = 12;
    if(n > 72) n = 72;
    return n;
}
bool Settings::markdown_render() const { return get("md_render", "1") != "0"; }
bool Settings::version_history() const { return get("version_history", "1") != "0"; }
bool Settings::first_line_indent() const { return get("first_line_indent", "0") == "1"; }
bool Settings::auto_save() const { return get("auto_save", "0") == "1"; }
bool Settings::recovery_draft() const { return get("recovery_draft", "1") != "0"; }
std::string Settings::app_mode() const { return get("app_mode", "journal"); }
std::string Settings::home_view() const { return get("home_view", "week"); }
std::string Settings::input_mode() const { return get("input_mode", "builtin"); }
std::string Settings::editor_mode() const { return get("editor_mode", "normal"); }
std::string Settings::editor_orientation() const { return get("editor_orientation", "horizontal"); }
bool Settings::vertical_reference_line() const { return get("vertical_ref_line", "0") == "1"; }
std::string Settings::vertical_reference_line_style() const {
    std::string s = get("vertical_ref_line_style", "solid");
    if(s != "dash" && s != "dot") return "solid";
    return s;
}
std::string Settings::wlan_interface() const {
    const char *env = getenv("PJOURNAL_WLAN");
    return env && *env ? env : get("wlan_interface", "wlan0");
}
