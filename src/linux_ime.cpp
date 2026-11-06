#include "linux_ime.h"

#include "IME.h"
#include "fcitx5_ime.h"
#include "settings.h"

#include <atomic>
#include <chrono>

#ifdef PJOURNAL_HAS_YONG
#include "yong_ime.h"
#endif

LinuxIme g_linux_ime;

static int64_t monotonic_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// 主循环空闲时判断"用户已经停手":最后一次按键时间与上次落盘时间。
static std::atomic<int64_t> g_last_ime_key_us{0};
static int64_t g_last_userdict_flush_us = 0;
static const int64_t IME_IDLE_FLUSH_AFTER_US = 400000;
static const int64_t IME_FLUSH_MIN_INTERVAL_US = 200000;

// 设置里选了哪个后端就把这一层整个让给那边。方法签名和语义都不变,所以
// src/app_ui.cpp 里二十多个调用点一行都不用改。
static bool use_fcitx5() {
    return g_settings.input_mode() == "fcitx5_rime";
}

#ifdef PJOURNAL_HAS_YONG
static bool use_yong() {
    return g_settings.input_mode() == "yong";
}
#else
// 没编 yong 后端(/home/ywz/yong 不在):选了也退化成内置输入法
static bool use_yong() { return false; }
#endif

bool LinuxIme::begin() {
    if(use_fcitx5()) return fcitx5_ime().begin();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().begin();
#endif
    IME::getInstance().setPageSize(5);
    return IME::getInstance().begin();
}

bool LinuxIme::active() const {
    if(use_fcitx5()) return fcitx5_ime().active();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().active();
#endif
    return IME::getInstance().active();
}

bool LinuxIme::composing() const {
    if(use_fcitx5()) return fcitx5_ime().composing();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().composing();
#endif
    return IME::getInstance().composing();
}

void LinuxIme::toggle() {
    if(use_fcitx5()) {
        fcitx5_ime().toggle();
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().toggle();
        return;
    }
#endif
    IME::getInstance().toggle();
}

void LinuxIme::set_active(bool on) {
    if(use_fcitx5()) {
        fcitx5_ime().set_active(on);
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().set_active(on);
        return;
    }
#endif
    IME::getInstance().setActive(on);
}

bool LinuxIme::handle_key(int key, std::string &out) {
    if(key == '\r') key = '\n';
    g_last_ime_key_us.store(monotonic_us());
    if(use_fcitx5()) return fcitx5_ime().handle_key(key, out);
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().handle_key(key, out);
#endif
    return IME::getInstance().handleKey(key, out);
}

bool LinuxIme::switch_schema() {
    if(use_fcitx5()) return fcitx5_ime().switch_schema();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().switch_schema();
#endif
    return false;
}

std::string LinuxIme::composition() const {
    if(use_fcitx5()) return fcitx5_ime().composition();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().composition();
#endif
    return IME::getInstance().displayCode();
}

const std::vector<std::string> &LinuxIme::candidates() const {
    if(use_fcitx5()) return fcitx5_ime().candidates();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().candidates();
#endif
    return IME::getInstance().candidates();
}

std::string LinuxIme::schema_name() const {
    if(use_fcitx5()) return fcitx5_ime().schema_name();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().schema_name();
#endif
    return std::string();
}

int LinuxIme::current_page() const {
    if(use_fcitx5()) return fcitx5_ime().current_page();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().current_page();
#endif
    return IME::getInstance().currentPage();
}

int LinuxIme::total_pages() const {
    if(use_fcitx5()) return fcitx5_ime().total_pages();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().total_pages();
#endif
    return IME::getInstance().totalPages();
}

int LinuxIme::total_candidates() const {
    if(use_fcitx5()) return fcitx5_ime().total_candidates();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().total_candidates();
#endif
    return IME::getInstance().totalCandidates();
}

int LinuxIme::page_size() const {
    if(use_fcitx5()) return fcitx5_ime().page_size();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().page_size();
#endif
    return IME::getInstance().pageSize();
}

void LinuxIme::set_page_size(int n) {
    if(use_fcitx5()) {
        fcitx5_ime().set_page_size(n);
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().set_page_size(n);
        return;
    }
#endif
    IME::getInstance().setPageSize(n);
}

int LinuxIme::highlight_index() const {
    if(use_fcitx5()) return fcitx5_ime().highlight_index();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().highlight_index();
#endif
    return IME::getInstance().highlightIdx();
}

bool LinuxIme::fullwidth() const {
    if(use_fcitx5()) return fcitx5_ime().fullwidth();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().fullwidth();
#endif
    return IME::getInstance().fullwidth();
}

bool LinuxIme::trad() const {
    if(use_fcitx5()) return fcitx5_ime().trad();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().trad();
#endif
    return IME::getInstance().trad();
}

bool LinuxIme::english() const {
    if(use_fcitx5()) return fcitx5_ime().english();
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) return yong_ime().english();
#endif
    return IME::getInstance().english();
}

void LinuxIme::toggle_fullwidth() {
    if(use_fcitx5()) {
        fcitx5_ime().toggle_fullwidth();
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().toggle_fullwidth();
        return;
    }
#endif
    IME::getInstance().toggleFullwidth();
}

void LinuxIme::toggle_trad() {
    if(use_fcitx5()) {
        fcitx5_ime().toggle_trad();
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().toggle_trad();
        return;
    }
#endif
    IME::getInstance().toggleTrad();
}

void LinuxIme::toggle_english() {
    if(use_fcitx5()) {
        fcitx5_ime().toggle_english();
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().toggle_english();
        return;
    }
#endif
    IME::getInstance().toggleEnglish();
}

void LinuxIme::set_display_width(int px) {
    if(use_fcitx5()) {
        fcitx5_ime().set_display_width(px);
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().set_display_width(px);
        return;
    }
#endif
    IME::getInstance().setDisplayWidth(px);
}

void LinuxIme::set_width_fn(WidthFn fn) {
    if(use_fcitx5()) {
        fcitx5_ime().set_width_fn(fn);
        return;
    }
#ifdef PJOURNAL_HAS_YONG
    if(use_yong()) {
        yong_ime().set_width_fn(fn);
        return;
    }
#endif
    IME::getInstance().setWidthFn(fn);
}

std::string LinuxIme::mode_label() const {
    if(use_fcitx5() || use_yong()) return std::string();
    return IME::getInstance().modeLabel();
}

bool LinuxIme::delete_mode() const {
    if(use_fcitx5() || use_yong()) return false;
    return IME::getInstance().isDeleteMode();
}

bool LinuxIme::predicting() const {
    if(use_fcitx5() || use_yong()) return false;
    return IME::getInstance().predicting();
}

bool LinuxIme::letter_select_armed() const {
    if(use_fcitx5() || use_yong()) return false;
    return IME::getInstance().letterSelectArmed();
}

std::string LinuxIme::letter_sel_keys() const {
    if(use_fcitx5() || use_yong()) return std::string();
    return IME::letterSelKeys();
}

std::string LinuxIme::take_status_message() {
    if(use_fcitx5() || use_yong()) return std::string();
    return IME::getInstance().takeStatusMessage();
}

void LinuxIme::set_document_context(const std::string &text) {
    if(use_fcitx5() || use_yong()) return;
    IME::getInstance().setDocumentContext(text);
}

void LinuxIme::handle_host_backspace() {
    if(use_fcitx5() || use_yong()) return;
    IME::getInstance().handleHostBackspace();
}

void LinuxIme::toggle_delete_mode() {
    if(use_fcitx5() || use_yong()) return;
    IME::getInstance().toggleDeleteMode();
}

void LinuxIme::reload_settings() {
    if(use_fcitx5() || use_yong()) return;
    IME::getInstance().reloadSettings();
}

void LinuxIme::flush_user_dict_idle() {
    if(use_fcitx5() || use_yong()) return;
    const int64_t last = g_last_ime_key_us.load();
    if(last == 0) return;
    const int64_t now = monotonic_us();
    if(now - last < IME_IDLE_FLUSH_AFTER_US) return;
    if(now - g_last_userdict_flush_us < IME_FLUSH_MIN_INTERVAL_US) return;
    IME &ime = IME::getInstance();
    if(!ime.hasPendingUserDictWrites()) return;
    ime.flushUserDictSavesIdle();
    g_last_userdict_flush_us = now;
}

void LinuxIme::flush_user_dict_now() {
    if(use_fcitx5() || use_yong()) return;
    // 先等异步写盘队列排空(超时 10s), 再结束写盘线程 —— 线程留着不走的话,
    // 进程退出时静态条件变量析构会一直等它, exit 卡死。
    IME::getInstance().shutdownUserDictWriter();
}
