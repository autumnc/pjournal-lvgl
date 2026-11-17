#include "journal_template.h"

#include "safe_file.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <dirent.h>
#include <sys/stat.h>

// 由 CMake 的 pjournal_embed_binary() 生成(Templates/*.md)。
extern "C" {
extern const uint8_t pj_tpl0_start[] asm("_binary_pj_tpl0_start");
extern const uint8_t pj_tpl0_end[] asm("_binary_pj_tpl0_end");
extern const uint8_t pj_tpl1_start[] asm("_binary_pj_tpl1_start");
extern const uint8_t pj_tpl1_end[] asm("_binary_pj_tpl1_end");
extern const uint8_t pj_tpl2_start[] asm("_binary_pj_tpl2_start");
extern const uint8_t pj_tpl2_end[] asm("_binary_pj_tpl2_end");
extern const uint8_t pj_tpl3_start[] asm("_binary_pj_tpl3_start");
extern const uint8_t pj_tpl3_end[] asm("_binary_pj_tpl3_end");
extern const uint8_t pj_tpl4_start[] asm("_binary_pj_tpl4_start");
extern const uint8_t pj_tpl4_end[] asm("_binary_pj_tpl4_end");
extern const uint8_t pj_tpl5_start[] asm("_binary_pj_tpl5_start");
extern const uint8_t pj_tpl5_end[] asm("_binary_pj_tpl5_end");
}

struct BuiltinTemplate {
    const char *name;
    const uint8_t *data;
    size_t size;
};

// 顺序必须与 CMakeLists.txt 的 pjournal_embed_binary 调用一致。
static const BuiltinTemplate kBuiltinTemplates[] = {
    {"子弹笔记日记.md", pj_tpl0_start, (size_t)(pj_tpl0_end - pj_tpl0_start)},
    {"晨间日记.md", pj_tpl1_start, (size_t)(pj_tpl1_end - pj_tpl1_start)},
    {"极简日记模板.md", pj_tpl2_start, (size_t)(pj_tpl2_end - pj_tpl2_start)},
    {"项目学习日记.md", pj_tpl3_start, (size_t)(pj_tpl3_end - pj_tpl3_start)},
    {"马伯庸日记模板.md", pj_tpl4_start, (size_t)(pj_tpl4_end - pj_tpl4_start)},
    {"马伯庸日记模板-紧凑.md", pj_tpl5_start, (size_t)(pj_tpl5_end - pj_tpl5_start)},
};

static const char *TEMPLATE_SETTING_KEY = "journal_template";
// 「无模板」的哨兵值。用不可能是合法文件名的写法,和"从未选择"(空值,
// 默认取列表第一个)区分开,老设备升级后模板日记仍然默认开启。
static const char *TEMPLATE_NONE_VALUE = "__none__";

// 模板会被整份读进内存,限制大小避免异常文件拖垮内存。
static const off_t kMaxTemplateBytes = 64 * 1024;

std::string journal_template_dir() {
    return home_dir() + "/Templates";
}

static bool has_markdown_ext(const std::string &name) {
    size_t dot = name.rfind('.');
    if(dot == std::string::npos || dot == 0) return false;
    std::string ext = name.substr(dot);
    for(char &c : ext)
        if(c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return ext == ".md" || ext == ".markdown";
}

static bool safe_template_name(const std::string &name) {
    if(name.empty() || name == "." || name == "..") return false;
    return name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

static void replace_all(std::string &text, const std::string &from, const std::string &to) {
    if(from.empty()) return;
    size_t pos = 0;
    while((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::vector<std::string> journal_template_list() {
    std::vector<std::string> out;
    const std::string dir = journal_template_dir();
    DIR *d = opendir(dir.c_str());
    if(!d) return out;
    struct dirent *de;
    while((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if(name.empty() || name[0] == '.' || !has_markdown_ext(name)) continue;
        struct stat st;
        if(stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string journal_template_display_name(const std::string &filename) {
    size_t dot = filename.rfind('.');
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

std::string journal_template_selected() {
    return g_settings.get(TEMPLATE_SETTING_KEY);
}

void journal_template_select(const std::string &filename) {
    if(safe_template_name(filename)) g_settings.set(TEMPLATE_SETTING_KEY, filename);
}

bool journal_template_enabled() {
    return journal_template_selected() != TEMPLATE_NONE_VALUE;
}

void journal_template_select_none() {
    g_settings.set(TEMPLATE_SETTING_KEY, TEMPLATE_NONE_VALUE);
}

std::string journal_template_expand(const std::string &text, time_t now) {
    struct tm tmv = {};
    localtime_r(&now, &tmv);

    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
    static const char *weekdays[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
    };
    const char *weekday = weekdays[tmv.tm_wday >= 0 && tmv.tm_wday < 7 ? tmv.tm_wday : 0];

    std::string out = text;
    replace_all(out, "{{date}}", date);
    replace_all(out, "{{ date }}", date);
    replace_all(out, "{{weekday}}", weekday);
    replace_all(out, "{{ weekday }}", weekday);
    return out;
}

bool journal_template_load(const std::string &filename, std::string &out) {
    if(!safe_template_name(filename) || !has_markdown_ext(filename)) return false;
    std::string path = journal_template_dir() + "/" + filename;
    struct stat st;
    if(stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > kMaxTemplateBytes)
        return false;
    std::string text = read_whole_file(path);
    // 兼容 Windows 编辑器的 BOM / CRLF,避免正文里混入不可见字符。
    if(text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
       (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    out = journal_template_expand(text, time(nullptr));
    return true;
}

void journal_template_ensure_defaults() {
    const std::string dir = journal_template_dir();
    struct stat st;
    // 只在 Templates 目录完全不存在时初始化;目录已存在时不动用户的文件
    // (包括用户删光后自己建的空目录)。
    if(stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return;
    if(!ensure_dir_path(dir)) return;

    for(const auto &tpl : kBuiltinTemplates) {
        std::string path = dir + "/" + tpl.name;
        if(file_exists(path)) continue;
        std::string content((const char *)tpl.data, tpl.size);
        safe_write_file(path, content);
    }
}

bool journal_template_load_selected(std::string &out, std::string &error) {
    if(!journal_template_enabled()) {
        error = "模板日记已关闭";
        return false;
    }
    std::string filename = journal_template_selected();
    if(filename.empty()) {
        std::vector<std::string> list = journal_template_list();
        if(list.empty()) {
            error = "未找到模板 (~/Templates)";
            return false;
        }
        filename = list.front();
        journal_template_select(filename);
    }
    if(!journal_template_load(filename, out)) {
        // 模板被删除或改名:退回第一个仍存在的模板。
        std::vector<std::string> list = journal_template_list();
        if(list.empty()) {
            error = "未找到模板 (~/Templates)";
            return false;
        }
        filename = list.front();
        journal_template_select(filename);
        if(!journal_template_load(filename, out)) {
            error = "模板读取失败";
            return false;
        }
    }
    return true;
}
