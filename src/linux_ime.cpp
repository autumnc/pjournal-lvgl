#include "linux_ime.h"

#include "IME.h"

LinuxIme g_linux_ime;

bool LinuxIme::begin() {
    IME::getInstance().setPageSize(9);
    return IME::getInstance().begin();
}

bool LinuxIme::active() const {
    return IME::getInstance().active();
}

bool LinuxIme::composing() const {
    return IME::getInstance().composing();
}

void LinuxIme::toggle() {
    IME::getInstance().toggle();
}

void LinuxIme::set_active(bool on) {
    IME::getInstance().setActive(on);
}

bool LinuxIme::handle_key(int key, std::string &out) {
    if(key == '\r') key = '\n';
    return IME::getInstance().handleKey(key, out);
}

std::string LinuxIme::composition() const {
    return IME::getInstance().displayCode();
}

const std::vector<std::string> &LinuxIme::candidates() const {
    return IME::getInstance().candidates();
}

int LinuxIme::current_page() const {
    return IME::getInstance().currentPage();
}

int LinuxIme::total_pages() const {
    return IME::getInstance().totalPages();
}

int LinuxIme::total_candidates() const {
    return IME::getInstance().totalCandidates();
}

int LinuxIme::page_size() const {
    return IME::getInstance().pageSize();
}

int LinuxIme::highlight_index() const {
    return IME::getInstance().highlightIdx();
}

bool LinuxIme::fullwidth() const {
    return IME::getInstance().fullwidth();
}

bool LinuxIme::trad() const {
    return IME::getInstance().trad();
}

bool LinuxIme::english() const {
    return IME::getInstance().english();
}

void LinuxIme::toggle_fullwidth() {
    IME::getInstance().toggleFullwidth();
}

void LinuxIme::toggle_trad() {
    IME::getInstance().toggleTrad();
}

void LinuxIme::toggle_english() {
    IME::getInstance().toggleEnglish();
}

void LinuxIme::set_display_width(int px) {
    IME::getInstance().setDisplayWidth(px);
}

void LinuxIme::set_width_fn(WidthFn fn) {
    IME::getInstance().setWidthFn(fn);
}
