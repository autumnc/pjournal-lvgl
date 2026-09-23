#pragma once

#include <string>

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

private:
    std::string settings_dir() const;
};

extern Settings g_settings;
