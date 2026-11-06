#pragma once

#include <string>

std::string home_dir();

class Settings {
public:
    bool begin();
    std::string get(const std::string &key, const std::string &def = "") const;
    void set(const std::string &key, const std::string &value);

    std::string journal_dir() const;
    std::string theme() const;
    std::string font_file() const;
    std::string font_bold_file() const;
    std::string font_italic_file() const;
    int font_size() const;
    int ime_font_size() const;
    int ui_font_size() const;
    bool markdown_render() const;
    bool version_history() const;
    bool first_line_indent() const;
    bool auto_save() const;
    bool recovery_draft() const;
    std::string app_mode() const;
    std::string home_view() const;
    std::string input_mode() const;
    std::string editor_mode() const;
    std::string editor_orientation() const;
    bool vertical_reference_line() const;
    std::string vertical_reference_line_style() const;
    std::string wlan_interface() const;
    int backlight_percent() const;

    // 内置输入法设置。键名与 ESP32 版一致(移植过来的 IME.cpp 原样调用驼峰 getter)。
    std::string imeFuzzy() const;          // "zcs" / "+nl" / "+nasal" / "all" / "off"
    std::string imePredictMode() const;    // "always" / "space" / "off"
    bool imeSentence() const;
    bool imeDocContext() const;
    bool imeCandidateHighlight() const;
    std::string letterSelKey() const;      // "off"/"tab"/"lctrl"/"lwin"/"lalt"/"ralt"/"rwin"/"rctrl"
    bool imeDebug() const;

private:
    std::string settings_dir() const;
};

extern Settings g_settings;
