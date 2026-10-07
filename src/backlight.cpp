#include "backlight.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>

// sysfs 的普通读写。不能借道 safe_file:那套是先写临时文件再 rename,而 sysfs
// 里的节点是内核现造的,rename 过去只会失败。
static bool write_int(const std::string &path, int value) {
    FILE *f = fopen(path.c_str(), "w");
    if(!f) return false;
    const bool wrote = fprintf(f, "%d", value) > 0;
    const bool closed = fclose(f) == 0;
    return wrote && closed;
}

static bool read_int(const std::string &path, int *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if(!f) return false;
    int value = -1;
    const bool ok = fscanf(f, "%d", &value) == 1;
    fclose(f);
    if(ok && out) *out = value;
    return ok;
}

static std::atomic<bool> g_power_off {false};
static std::atomic<int> g_saved_raw {0};

std::string backlight_device() {
    static std::string cached;
    static bool probed = false;
    const char *env = getenv("PJOURNAL_BACKLIGHT");
    if(env && *env) return env;
    if(probed) return cached;
    probed = true;
    DIR *d = opendir("/sys/class/backlight");
    if(!d) return "";
    while(dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if(name.empty() || name[0] == '.') continue;
        cached = "/sys/class/backlight/" + name;
        break;
    }
    closedir(d);
    return cached;
}

int backlight_max() {
    static bool probed = false;
    static int cached = 0;
    if(!probed) {
        probed = true;
        const std::string dir = backlight_device();
        if(!dir.empty()) read_int(dir + "/max_brightness", &cached);
        if(cached <= 0) cached = 0;
    }
    return cached;
}

bool backlight_available() { return backlight_max() > 0; }

bool backlight_set_percent(int percent) {
    const int max = backlight_max();
    if(max <= 0) return false;
    if(percent < 1) percent = 1;
    if(percent > 100) percent = 100;
    int raw = (int)((long)percent * max / 100);
    if(raw < 1) raw = 1;
    if(raw > max) raw = max;
    if(!write_int(backlight_device() + "/brightness", raw)) return false;
    g_power_off = false;
    return true;
}

int backlight_current_percent() {
    if(g_power_off.load()) return 0;
    const int max = backlight_max();
    if(max <= 0) return 0;
    int raw = 0;
    if(!read_int(backlight_device() + "/brightness", &raw)) return 0;
    int percent = (int)(((long)raw * 100 + max / 2) / max);
    if(percent < 1) percent = 1;
    if(percent > 100) percent = 100;
    return percent;
}

bool backlight_is_off() { return g_power_off.load(); }

bool backlight_toggle_power() {
    const int max = backlight_max();
    if(max <= 0) return true;
    const std::string node = backlight_device() + "/brightness";
    if(g_power_off.load()) {
        const int saved = g_saved_raw.load();
        const int raw = saved > 0 ? saved : max;
        if(!write_int(node, raw)) return false;
        g_power_off = false;
        return true;
    }
    int raw = 0;
    if(read_int(node, &raw) && raw > 0) g_saved_raw = raw;
    if(g_saved_raw.load() <= 0) g_saved_raw = max;
    if(!write_int(node, 0)) return false;
    g_power_off = true;
    return true;
}
