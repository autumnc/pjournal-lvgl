#include "app_ui.h"
#include "backlight.h"
#include "font_manager.h"
#include "journal_storage.h"
#include "linux_console.h"
#include "linux_input.h"
#include "settings.h"

#include <lvgl.h>
#include <src/drivers/display/fb/lv_linux_fbdev.h>
#include <src/libs/freetype/lv_freetype.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef PJOURNAL_DEFAULT_WIDTH
#define PJOURNAL_DEFAULT_WIDTH 1024
#endif
#ifndef PJOURNAL_DEFAULT_HEIGHT
#define PJOURNAL_DEFAULT_HEIGHT 600
#endif
#ifndef PJOURNAL_DEFAULT_INPUT
#define PJOURNAL_DEFAULT_INPUT "/dev/input/event0"
#endif
#ifndef PJOURNAL_DEFAULT_EXTRA_INPUTS
#define PJOURNAL_DEFAULT_EXTRA_INPUTS "/dev/input/event1"
#endif

static uint32_t tick_ms() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

static const char *default_framebuffer() {
    const char *fb = getenv("PJOURNAL_FB");
    if(fb && *fb) return fb;
    return "/dev/fb0";
}

static int env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    if(!v || !*v) return fallback;
    char *end = nullptr;
    long parsed = strtol(v, &end, 10);
    if(end == v || parsed <= 0 || parsed > 10000) return fallback;
    return (int)parsed;
}

static void start_input_list(const char *list) {
    if(!list || !*list) return;
    std::stringstream ss(list);
    std::string device;
    while(std::getline(ss, device, ',')) {
        size_t first = device.find_first_not_of(" \t");
        size_t last = device.find_last_not_of(" \t");
        if(first == std::string::npos) continue;
        linux_input_start(device.substr(first, last - first + 1));
    }
}

static void scan_fonts() {
    const char *font_dir = getenv("PJOURNAL_FONT_DIR");
    if(font_dir && *font_dir) {
        g_fonts.scan(font_dir);
        return;
    }
    g_fonts.scan(home_dir() + "/.fonts");
    if(!g_fonts.fonts().empty()) return;
    g_fonts.scan("/usr/share/fonts");
}

static void prepare_framebuffer(const char *path) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if(fd < 0) return;
    fb_var_screeninfo vinfo {};
    fb_fix_screeninfo finfo {};
    if(ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        vinfo.xoffset = 0;
        vinfo.yoffset = 0;
        vinfo.activate = FB_ACTIVATE_NOW;
        ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo);
    }
    if(ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.smem_len > 0) {
        void *mem = mmap(nullptr, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if(mem != MAP_FAILED) {
            memset(mem, 0, finfo.smem_len);
            msync(mem, finfo.smem_len, MS_SYNC);
            munmap(mem, finfo.smem_len);
        }
    }
    close(fd);
}

int main() {
    linux_console_enter_graphics();
    g_settings.begin();
    // 存过档位才去动硬件;没设过就保留系统开机时给的那个亮度
    int backlight = g_settings.backlight_percent();
    if(backlight > 0) backlight_set_percent(backlight);
    g_journal.begin();
    scan_fonts();

    lv_init();
    lv_tick_set_cb(tick_ms);

    const char *fb = default_framebuffer();
    prepare_framebuffer(fb);
    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, fb);
    lv_display_set_resolution(disp,
                              env_int("PJOURNAL_WIDTH", PJOURNAL_DEFAULT_WIDTH),
                              env_int("PJOURNAL_HEIGHT", PJOURNAL_DEFAULT_HEIGHT));
    if(getenv("PJOURNAL_FB_FORCE_REFRESH")) lv_linux_fbdev_set_force_refresh(disp, true);

    lv_freetype_init(512);
    app_ui_create();

    const char *input = getenv("PJOURNAL_INPUT");
    if(input && *input) {
        linux_input_start(input);
    } else {
        bool console = linux_input_start_console();
        if(console) linux_input_start_console_modifiers();
        else linux_input_start(PJOURNAL_DEFAULT_INPUT);
    }
    const char *extra_inputs = getenv("PJOURNAL_INPUTS");
    start_input_list(extra_inputs && *extra_inputs ? extra_inputs : PJOURNAL_DEFAULT_EXTRA_INPUTS);

    while(!app_ui_should_quit()) {
        app_ui_tick();
        lv_timer_handler();
        usleep(5000);
    }
    linux_console_restore();
    return 0;
}
