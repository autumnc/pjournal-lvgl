#include "backlight.h"

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

std::string backlight_device() {
    const char *env = getenv("PJOURNAL_BACKLIGHT");
    if(env && *env) return env;
    DIR *d = opendir("/sys/class/backlight");
    if(!d) return "";
    std::string found;
    while(dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if(name.empty() || name[0] == '.') continue;
        found = "/sys/class/backlight/" + name;
        break;
    }
    closedir(d);
    return found;
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
    return write_int(backlight_device() + "/brightness", raw);
}

int backlight_current_percent() {
    const int max = backlight_max();
    if(max <= 0) return 0;
    int raw = 0;
    if(!read_int(backlight_device() + "/brightness", &raw)) return 0;
    int percent = (int)(((long)raw * 100 + max / 2) / max);
    if(percent < 1) percent = 1;
    if(percent > 100) percent = 100;
    return percent;
}
