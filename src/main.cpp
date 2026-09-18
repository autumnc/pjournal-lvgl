#include "app_ui.h"
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
#include <thread>
#include <unistd.h>

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
    g_journal.begin();
    g_fonts.scan("/root/.fonts");

    lv_init();
    lv_tick_set_cb(tick_ms);

    const char *fb = default_framebuffer();
    prepare_framebuffer(fb);
    lv_display_t *disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, fb);
    lv_display_set_resolution(disp, 1024, 600);
    lv_linux_fbdev_set_force_refresh(disp, true);

    lv_freetype_init(512);
    app_ui_create();

    const char *input = getenv("PJOURNAL_INPUT");
    if(input && *input) {
        linux_input_start(input);
    } else {
        bool console = linux_input_start_console();
        if(console) linux_input_start_modifier_taps("/dev/input/event0");
        else linux_input_start("/dev/input/event0");
    }
    linux_input_start("/dev/input/event1");

    while(!app_ui_should_quit()) {
        app_ui_tick();
        lv_timer_handler();
        usleep(5000);
    }
    linux_console_restore();
    return 0;
}
