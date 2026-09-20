#include "app_ui.h"

#include "file_manager_server.h"
#include "font_manager.h"
#include "hash_utils.h"
#include "journal_storage.h"
#include "key_codes.h"
#include "linux_ime.h"
#include "markdown.h"
#include "outline_model.h"
#include "process_utils.h"
#include "safe_file.h"
#include "settings.h"
#include "text_utils.h"
#include "vertical_layout.h"
#include "webdav_sync.h"
#include "wifi_manager.h"

#include "IME.h"
#include "builtin_prompts.h"
#include "json_utils.h"
#include "json_parser.h"

#include <lvgl.h>
#include <src/misc/lv_text.h>
#include <src/misc/lv_text_private.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <functional>
#include <map>
#include <net/if.h>
#include <set>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <vector>

enum class Screen {
    Main,
    Editor,
    Browser,
    Viewer,
    History,
    Gtd,
    Outline,
    Sync,
    SettingText,
    Settings,
    Wifi,
    Inspiration,
    Dict,
    FileMgr,
    Help,
};

struct Theme {
    lv_color_t bg;
    lv_color_t fg;
    lv_color_t muted;
    lv_color_t accent;
    lv_color_t panel;
};

struct Action {
    char key;
    const char *title;
    const char *symbol;
    Screen screen;
};

static const Action k_actions[] = {
    {'p', "提示写作", LV_SYMBOL_FILE, Screen::Editor},
    {'f', "自由写作", LV_SYMBOL_KEYBOARD, Screen::Editor},
    {'v', "查看过往日记", LV_SYMBOL_LIST, Screen::Browser},
    {'w', "同步WebDAV", LV_SYMBOL_UPLOAD, Screen::Sync},
    {'t', "GTD任务管理", LV_SYMBOL_OK, Screen::Gtd},
    {'o', "大纲写作", LV_SYMBOL_DIRECTORY, Screen::Outline},
    {'s', "设置", LV_SYMBOL_SETTINGS, Screen::Settings},
};

static Theme g_theme;
static lv_style_t g_style;
static lv_obj_t *g_root = nullptr;
static lv_obj_t *g_status = nullptr;
static lv_obj_t *g_status_right = nullptr;  // 状态栏右侧靠右对齐的那半
static Screen g_screen = Screen::Main;
static Screen g_prev_screen = Screen::Main;
// 进设置界面之前是哪个界面。设置下面挂着一串子界面(文本编辑框、WiFi、词库、文件管理),
// 它们退回设置时也走 goto_screen(Settings),不能用 g_prev_screen 记,否则会被顶成那个
// 子界面。设置里改完东西按 Esc 要回到这里。
static Screen g_settings_return = Screen::Main;
static int g_main_sel = 0;
static int g_browser_sel = 0;
static int g_history_sel = 0;
static int g_history_scroll = 0;
static int g_history_preview_scroll = 0;
enum class GtdMode {
    List,
    Add,
    Detail,
    EditField,
    Filter,
    Rename,
    Confirm,
    AddProject,
    RenameProject,
    Picker,
    Calendar,
    Help,
    Note,
    ContextMgr,
    TagMgr,
    AddContext,
    AddTag,
    RenameContext,
    RenameTag,
    Summary,
    Archive,
};

enum class GtdConfirm { None, Task, Tasks, Project, Context, Tag, ArchiveMonth };

// GTD 的全部界面状态。数据本身放在 data(与 ESP32 的 gtd.json 同构)。
struct GtdState {
    int view = 0;
    int sel = 0;
    int scroll = 0;
    GtdMode mode = GtdMode::List;
    GtdMode helpPrev = GtdMode::List;
    GtdMode summaryPrev = GtdMode::List;
    GtdConfirm confirm = GtdConfirm::None;
    GtdMode confirmReturn = GtdMode::List;
    JsonValue data;
    std::vector<int> filtered;    // 可见任务的 tasks 数组下标(按显示顺序)
    std::vector<int> depth;       // 与 filtered 平行:树深
    std::vector<int> visPos;      // 与 filtered 平行:在 treeOrder 中的位置
    std::vector<int> treeOrder;   // 完整树的任务下标
    std::vector<int> treeDepth;   // 完整树的深度
    std::set<int> folded;         // 折叠的 treeOrder 位置
    int detailIdx = -1;           // tasks 数组下标
    int detailField = 0;
    int insertAfter = -1;
    std::string pendingParent;
    std::string pendingProject;
    std::string filterText;
    std::string filterContext;
    std::vector<std::string> filterTags;
    std::set<std::string> multiSel;  // 选中的任务 id
    int multiAnchor = -1;
    std::vector<std::string> projectList;
    int projectDrill = -1;
    std::vector<std::pair<std::string, std::string>> pickerOpts;  // value, 显示文本
    int pickerSel = 0;
    int pickerScroll = 0;
    int pickerField = -1;
    std::set<int> pickerToggled;
    int calYear = 2026;
    int calMonth = 1;
    int calSelDay = 1;
    std::vector<std::string> contextList;
    std::vector<std::string> tagList;
    int ctxSel = 0;
    int tagSel = 0;
    int helpScroll = 0;
    int summaryScroll = 0;
    std::vector<std::string> archiveMonths;
    std::vector<int> archiveCounts;
    int archiveSel = 0;
    bool archiveBrowsing = false;
    std::string archiveViewMonth;
    std::vector<JsonValue> archiveTasks;
    int archiveViewSel = 0;
    std::string renameTarget;  // 被重命名的 context/tag/project
    std::string confirmTitle;
    std::string confirmAction;
    lv_obj_t *input = nullptr;  // 文本输入模式共用的 textarea
    std::string notice;         // 一次性状态提示
};
static GtdState g_gtd;
static int g_settings_sel = 0;
static int g_settings_scroll = 0;
static int g_wifi_sel = 0;
static int g_wifi_saved_sel = 0;
static std::vector<JournalEntry> g_entries;
static std::vector<JournalHistoryVersion> g_history_versions;
static std::vector<WifiNetwork> g_wifi_entries;
static std::vector<WifiSavedNetwork> g_wifi_saved_entries;
static std::string g_view_file;
static std::string g_history_file;
static std::string g_edit_file;
static std::string g_file_edit_path;
static std::string g_prompt;
static int g_quick_slot = -1;
static std::string g_clipboard;
static std::vector<std::string> g_undo_stack;
static std::vector<std::string> g_redo_stack;
static std::string g_recovery_meta;
static size_t g_last_recovery_hash = 0;
static uint32_t g_next_recovery_ms = 0;
static bool g_recovery_checked = false;
static lv_obj_t *g_editor = nullptr;
static lv_obj_t *g_file_panel = nullptr;
static lv_obj_t *g_setting_text = nullptr;
static lv_obj_t *g_wifi_password = nullptr;
static lv_obj_t *g_inspiration_text = nullptr;
static lv_obj_t *g_ime_bar = nullptr;
static lv_obj_t *g_search_panel = nullptr;
static lv_obj_t *g_search_input = nullptr;
static lv_obj_t *g_search_replace = nullptr;
static bool g_search_focus_rep = false;     // Tab 切到的字段
static std::vector<std::pair<int,int>> g_search_matches;  // 全文匹配(字节区间)
static int g_search_cur = -1;
static std::string g_search_last_term, g_search_last_text;
static lv_obj_t *g_polish_panel = nullptr;   // AI 润色模态面板
static lv_obj_t *g_polish_preview = nullptr; // 结果预览(只读)
static lv_obj_t *g_polish_instr = nullptr;   // 补充指令输入框
static bool g_polish_instr_mode = false;     // 面板当前是「改指令」还是「看结果」
static std::string g_polish_source;          // 送润色的原文(选区或整篇)
static std::string g_polish_result;          // 润色结果
static std::string g_polish_instr_text;      // 用户补充的优化指令
static bool g_polish_is_sel = false;         // 是否润色选区
static lv_obj_t *g_md_view = nullptr;     // Markdown 叠加层的视口
static lv_obj_t *g_md_content = nullptr;  // 叠加层内容(负责滚动)
static lv_obj_t *g_md_caret = nullptr;    // 叠加层自己画的光标
static std::vector<lv_obj_t *> g_md_labels;
static std::vector<lv_obj_t *> g_md_rules;
static std::set<int> g_md_folded;         // 被折叠的标题所在逻辑行
static int g_md_max_w = 0;                // 编辑器正文宽度(排版用)
static int g_md_view_h = 0;
static std::vector<lv_area_t> g_md_sel_rects;  // 选区高亮块(每帧由 refresh 重算)
static std::vector<lv_area_t> g_md_box_rects;  // 反白底色块(`code` / 链接)
static std::vector<lv_area_t> g_md_dot_rects;  // 着重号小方点(==高亮==)
static lv_obj_t *g_focus_dim[2] = {nullptr, nullptr};  // 聚焦模式:光标行上下的压暗罩
static lv_area_t g_md_focus_band {};                   // 聚焦模式:当前行的亮带(内容层坐标)
static bool g_md_focus_band_on = false;

static std::string meta_value(const std::string &meta, const char *key) {
    std::string prefix = std::string(key) + "=";
    size_t pos = meta.find(prefix);
    if(pos == std::string::npos) return "";
    pos += prefix.size();
    size_t end = meta.find('\n', pos);
    return meta.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

static bool journal_filename_ok(const std::string &fn) {
    if(fn.empty() || fn[0] == '.' || fn.find('/') != std::string::npos || fn.find("..") != std::string::npos) return false;
    size_t dot = fn.rfind('.');
    if(dot == std::string::npos) return false;
    std::string ext = fn.substr(dot);
    return ext == ".txt" || ext == ".md";
}
static std::vector<lv_area_t> g_md_wave_rects;  // 书名波浪线(《书名》)

struct FilePanelState {
    bool active = false;
    bool prompt = false;
    bool rename = false;
    bool confirmDelete = false;
    int sel = 0;
    int scroll = 0;
    std::vector<std::string> entries;
    std::string input;
    std::string message;
};
static FilePanelState g_file_panel_state;

// 只读渲染(阅读视图 / 历史预览):与编辑器叠加层共用行级排版,但没有光标/选区/折叠。
// 定义在编辑器模块里,这里先声明给上面的 render_viewer / render_history 用。
static lv_obj_t *g_ro_view = nullptr;
static lv_obj_t *g_ro_content = nullptr;
static std::vector<lv_obj_t *> g_ro_labels;
static std::vector<lv_obj_t *> g_ro_rules;
static std::vector<int> g_viewer_line_y;  // 阅读视图每个源行的 y(按行滚动)
static int g_viewer_scroll = 0;           // 阅读视图首行行号
static void md_readonly_create(int x, int y, int w, int h);
static void md_render_readonly(const std::string &text, int content_w, int line_space,
                               std::vector<int> *line_y_out);

// 竖排编辑器(文字方向 = 竖排)叠加层。和 Markdown 叠加层一样,lv_textarea
// 依旧是唯一数据源,这一层只负责按「格」重排并自绘光标/选区/参考线。
static lv_obj_t *g_vt_view = nullptr;
static lv_obj_t *g_vt_caret = nullptr;
static std::vector<lv_obj_t *> g_vt_cols;   // 每列一个标签(字符以 \n 竖排)
static std::vector<lv_obj_t *> g_vt_marks;  // 选区高亮块(池化复用)
static std::vector<lv_obj_t *> g_vt_cells;  // 带样式的列改用逐格标签
static std::vector<lv_obj_t *> g_vt_prompt_cols;  // 竖排提示词自己的列标签
static int g_ed_sel_anchor = -1;            // 编辑器选区锚点(字节),-1 = 无选区;横排/竖排共用
static int g_vt_scroll = 0;                 // 首列列号
static VerticalLayoutMetrics g_vt_m;        // 当前排版度量
static VerticalData g_vt_data;              // 上一帧的格数据(导航复用)
static MdDoc g_vt_doc;                      // 上一帧的行拆分(导航复用)
static int g_vt_h = 0;
static std::vector<lv_area_t> g_vt_box_rects;  // 反白格底色块
static std::vector<lv_area_t> g_vt_deco_rects;  // 竖排下划线/删除线/着重号
static size_t g_editor_clean_hash = 0;      // 上次保存/载入时正文的哈希
static bool g_editor_exit_asking = false;   // 退出询问框是否显示
static int g_editor_exit_target = 0;        // 0=主面板 1=大纲 2=快捷编辑(设置)
static lv_obj_t *g_editor_exit_dlg = nullptr;
static bool g_quit_asking = false;          // Ctrl+Q 退出 app 的确认框是否显示
static lv_obj_t *g_quit_dlg = nullptr;
static Screen g_history_return = Screen::Browser;
static bool g_history_preview = false;
static bool g_history_confirm_restore = false;
static bool g_history_confirm_delete = false;
static std::string g_setting_key;
static std::string g_setting_title;
enum class OutlineMode {
    Projects,      // 项目列表
    Browse,        // 大纲树
    Detail,        // 节点详情面板
    AddProject,
    AddHeading,
    AddSub,
    EditText,      // 编辑标题/关键词/备注(单行)
    Filter,        // 筛选输入(以字符串为准,每次按键重绘)
    Summary,       // 摘要浮层
    Help,
    BookmarkMgr,
    Confirm,
    TagMgr,
    AddTag,
    RenameTag,
    Picker,        // 状态/标签选择浮层
};

struct OutlineState {
    OutlineMode mode = OutlineMode::Projects;
    OutlineMode helpPrev = OutlineMode::Browse;
    int sel = 0;        // Projects:项目下标;Browse:filtered 下标
    int scroll = 0;
    std::vector<std::string> projects;
    std::string project;      // 当前项目名(空 = 在项目列表)
    JsonValue data;           // { nodes, bookmarks, tags }
    std::vector<int> filtered;  // 可见节点在 nodes 中的下标
    std::set<int> folded;
    std::string filterText;
    std::vector<std::string> filterTags;

    lv_obj_t *input = nullptr;   // 文本输入模式共用的 textarea
    std::string editBuf;         // Filter 模式的输入缓冲(无 textarea)
    int editIdx = -1;            // EditText 作用的节点下标
    bool editingTitle = false;
    bool editingKeyword = false;
    int pendingLevel = 0;
    int insertAfter = -1;

    int detailIdx = -1;
    int detailField = 0;
    int summaryIdx = -1;
    int summaryScroll = 0;
    int helpScroll = 0;
    int bmSel = 0;
    std::vector<std::string> tagList;
    int tagSel = 0;
    std::string renameTag;

    std::vector<std::string> pickerVal;   // value
    std::vector<std::string> pickerDisp;  // 显示文本
    int pickerSel = 0;
    int pickerField = -1;
    std::set<int> pickerToggled;

    std::string confirmMsg;
    int confirmAction = 0;   // 1=删标题 2=删项目 3=清除文件关联
    int confirmIdx = -1;
    std::string editPath;    // 大纲正文文件的绝对路径(交给编辑器时设置)
};
static OutlineState g_ol;
static bool g_wifi_saved_mode = false;
enum class InspirationMode { List, EditContent, EditKeywords, Search };
static JsonValue g_inspiration_data;
static std::vector<int> g_inspiration_filtered;
static InspirationMode g_inspiration_mode = InspirationMode::List;
static int g_inspiration_sel = 0;
static int g_inspiration_scroll = 0;
static int g_inspiration_edit_idx = -1;
static std::string g_inspiration_query;
enum class DictMode { Choose, List, Search, Add };
static DictMode g_dict_mode = DictMode::Choose;
static int g_dict_kind = IME::FIXED_DICT;
static int g_dict_sel = 0;
static int g_dict_scroll = 0;
static std::set<int> g_dict_selected;
static std::string g_dict_query;
static std::vector<int> g_dict_filtered;
static std::vector<IME::UserEntryView> g_dict_entries;
static lv_obj_t *g_dict_text = nullptr;
static std::string g_sync_message;
static bool g_sync_done = false;
static bool g_should_quit = false;
static lv_font_t *g_ui_font = nullptr;
static lv_font_t *g_icon_font = nullptr;
static lv_font_t *g_editor_font = nullptr;
static lv_font_t *g_editor_bold_font = nullptr;    // 真粗体字体;未配置为 nullptr(改用描两遍)
static lv_font_t *g_editor_italic_font = nullptr;  // 真斜体字体;未配置为 nullptr
static lv_font_t *g_editor_faux_italic = nullptr;  // 正文路径 + FreeType 斜切,伪斜体免费
static lv_font_t *g_ime_font = nullptr;  // 候选条字号独立于正文,由设置里的输入法字号决定
static const lv_font_t *g_current_font = nullptr;

// 正文行距。着重号画在字脚下面,行挨太紧会蹭到下一行,必须留出余量。
static constexpr int k_md_line_space = 6;

static void render();
static void goto_screen(Screen s);
static void save_editor_text();
static void editor_follow_cursor();
static void editor_md_refresh();
static void editor_md_create(int x, int y, int w, int h, int content_w);
static void editor_record_undo();
static void editor_vt_refresh();
static void editor_vt_create(int x, int y, int w, int h);
static bool editor_vertical();
static void editor_sel_style_plain();

// 编辑模式四选一:正常 / 打字机 / 聚焦 / 打字机聚焦。聚焦那两种只是在打字机之上多盖一层
// 暗化罩,所以拆成两个谓词,typing 相关的代码照旧只问 editor_typewriter()。
static bool editor_typewriter() {
    const std::string m = g_settings.editor_mode();
    return m == "typewriter" || m == "typewriter_focus";
}
static bool editor_focus() {
    const std::string m = g_settings.editor_mode();
    return m == "focus" || m == "typewriter_focus";
}
static std::string editor_mode_label() {
    const std::string m = g_settings.editor_mode();
    if(m == "typewriter") return "打字机";
    if(m == "focus") return "聚焦";
    if(m == "typewriter_focus") return "打字机聚焦";
    return "正常";
}

static std::string time_label(time_t t) {
    tm local {};
    localtime_r(&t, &local);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return buf;
}

static std::string trim_copy(std::string s) {
    while(!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t start = 0;
    while(start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) ++start;
    return start ? s.substr(start) : s;
}

// 沿用 tmux 状态栏 /root/bin/batt.sh 的 Nerd Font 电池图标:
// F702(满)…F70B(空)是电量格,F701 闪电表示充电、F700 表示放电。
static const char *k_bat_level[10] = {
    "\xef\x9c\x8b", "\xef\x9c\x8a", "\xef\x9c\x89", "\xef\x9c\x88", "\xef\x9c\x87",
    "\xef\x9c\x86", "\xef\x9c\x85", "\xef\x9c\x84", "\xef\x9c\x83", "\xef\x9c\x82",
};

static std::string battery_percent_label() {
    // 逐键刷新状态栏时会反复调用,电池读得慢又变得少,这里缓存 30 秒
    static std::string cache;
    static uint32_t next_ms = 0;
    uint32_t now = lv_tick_get();
    if(!cache.empty() && (int32_t)(now - next_ms) < 0) return cache;

    const char *base = "/sys/class/power_supply";
    DIR *d = opendir(base);
    if(!d) return cache = std::string(k_bat_level[0]) + "\xef\x9c\x80--%";
    std::string cap, status;
    dirent *de = nullptr;
    while((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if(name == "." || name == "..") continue;
        std::string dir = std::string(base) + "/" + name;
        std::string c = trim_copy(read_whole_file(dir + "/capacity"));
        if(c.empty()) continue;
        std::string type = trim_copy(read_whole_file(dir + "/type"));
        if(type == "Mains" || type == "USB" || type == "USB_C") continue;
        cap = c;
        status = trim_copy(read_whole_file(dir + "/status"));
        if(type == "Battery") break;
    }
    closedir(d);
    int pct = cap.empty() ? -1 : atoi(cap.c_str());
    int level = pct < 0 ? 0 : (pct - 5) / 10;
    if(level < 0) level = 0;
    if(level > 9) level = 9;
    const char *sign = status == "Charging" ? "\xef\x9c\x81" : "\xef\x9c\x80";
    cache = (pct < 0 ? std::string(k_bat_level[0]) : std::string(k_bat_level[level])) + sign +
            (pct < 0 ? "--" : std::to_string(pct)) + "%";
    next_ms = now + 30000;
    return cache;
}

// 状态栏 WiFi 图标,字形沿用 tmux 那套 Nerd 码位:
// U+3237 满格 = 已连接,U+F1EB 线框 = 接口在但没连上,U+3239 斜杠 = WiFi 关闭。
static std::string wifi_status_label() {
    // 逐键刷新状态栏时要读 sysfs + fork wpa_cli,缓存 5 秒
    static std::string cache;
    static uint32_t next_ms = 0;
    uint32_t now = lv_tick_get();
    if(!cache.empty() && (int32_t)(now - next_ms) < 0) return cache;

    // 关 WiFi 后 wifi_switch 会 ip link down 甚至卸载驱动,所以先看链路:
    // 目录没了或 IFF_UP 没置位都算关闭,这时也不用去打 wpa_cli。
    std::string sys = "/sys/class/net/" + g_settings.wlan_interface();
    struct stat st {};
    bool link_up = false;
    if(stat(sys.c_str(), &st) == 0) {
        std::string flags = trim_copy(read_whole_file(sys + "/flags"));
        link_up = (strtoul(flags.c_str(), nullptr, 0) & IFF_UP) != 0;
    }
    if(!link_up) cache = "\xe3\x88\xb9";
    else if(g_wifi.status().connected) cache = "\xe3\x88\xb7";
    else cache = "\xef\x87\xab";
    next_ms = now + 5000;
    return cache;
}

static std::string clock_label() {
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char buf[48];
    strftime(buf, sizeof(buf), "%H:%M", &local);
    return buf;
}

// 状态栏右侧统一顺序:时间 [输入法状态] WiFi 电池,主面板没有输入框所以省掉中括号那段
static std::string main_status_text() {
    return clock_label() + " " + wifi_status_label() + " " + battery_percent_label();
}

static std::string history_filename_label(const std::string &fn) {
    if(fn.size() >= 17) {
        std::string s = fn.substr(0, 10) + " " + fn.substr(11, 2) + ":" + fn.substr(13, 2) + ":" + fn.substr(15, 2);
        return s;
    }
    return fn;
}

static std::string home_dir() {
    const char *home = getenv("HOME");
    return home && *home ? home : "/root";
}

static std::string gtd_dir() {
    return home_dir() + "/.pjournal-lvgl/gtd";
}

static std::string quick_dir() {
    return g_settings.journal_dir() + "/quick";
}

static std::string quick_file(int slot) {
    return quick_dir() + "/" + std::to_string(slot) + ".txt";
}

static std::string file_edit_dir() {
    return g_settings.journal_dir() + "/files";
}

static std::string file_edit_clean_name(std::string name) {
    std::string out;
    for(unsigned char c : name) {
        if(c == '/' || c == '\\' || c == ':' || c < 0x20) continue;
        out.push_back((char)c);
    }
    size_t s = out.find_first_not_of(" \t\r\n");
    size_t e = out.find_last_not_of(" \t\r\n");
    if(s == std::string::npos) out.clear();
    else out = out.substr(s, e - s + 1);
    if(out.empty()) out = "notes.txt";
    if(out.find('.') == std::string::npos) out += ".txt";
    return out;
}

static std::string file_edit_path_for(const std::string &name) {
    return file_edit_dir() + "/" + file_edit_clean_name(name);
}

// 按字符退格:输入法提交的是 UTF-8 多字节,直接 pop_back 会把一个汉字切成半个,存成文件名坏掉
static void utf8_pop_back(std::string &s) {
    if(s.empty()) return;
    size_t n = s.size();
    while(n > 0 && ((unsigned char)s[n - 1] & 0xC0) == 0x80) --n;  // 跳过续字节
    if(n > 0) --n;                                                 // 再吃掉首字节
    s.erase(n);
}

static bool path_is_file(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::vector<std::string> file_edit_entries() {
    ensure_dir_path(file_edit_dir());
    std::vector<std::string> v;
    DIR *d = opendir(file_edit_dir().c_str());
    if(!d) return v;
    dirent *de = nullptr;
    while((de = readdir(d)) != nullptr) {
        std::string n = de->d_name;
        if(n.empty() || n[0] == '.') continue;
        if(n.size() >= 4 && (n.substr(n.size() - 4) == ".tmp" || n.substr(n.size() - 4) == ".bak")) continue;
        if(path_is_file(file_edit_dir() + "/" + n)) v.push_back(n);
    }
    closedir(d);
    std::sort(v.begin(), v.end());
    return v;
}

static std::string file_edit_last_name() {
    std::string n = file_edit_clean_name(g_settings.get("file_edit_last", "notes.txt"));
    if(!path_is_file(file_edit_path_for(n))) {
        auto entries = file_edit_entries();
        if(!entries.empty()) n = entries.front();
    }
    return n;
}

static void file_edit_set_last(const std::string &name) {
    g_settings.set("file_edit_last", file_edit_clean_name(name));
}

static void file_edit_ensure_default() {
    ensure_dir_path(file_edit_dir());
    std::string n = file_edit_last_name();
    if(!path_is_file(file_edit_path_for(n))) safe_write_file(file_edit_path_for(n), "");
    file_edit_set_last(n);
}

static std::string gtd_file() {
    return gtd_dir() + "/gtd.json";
}

static std::string gtd_archive_dir() {
    return gtd_dir() + "/archive";
}

static const char *k_gtd_views[] = {"收集箱", "下一步", "等待", "项目", "已完成"};
static const char *k_gtd_status[] = {"todo", "doing", "done", "waiting"};
static const char *k_gtd_status_disp[] = {"待办", "进行中", "已完成", "等待中"};
static const char *k_gtd_priority[] = {"A", "B", "C"};
static const char *k_gtd_priority_disp[] = {"A 高", "B 中", "C 低"};

// 详情页字段。type: s 文本 p 优先级 t 状态 j 项目 d 日期 c 情境 g 标签 n 数字 m 备注
struct GtdField {
    const char *label;
    const char *key;
    char type;
};

static const GtdField k_gtd_fields[] = {
    {"标题", "title", 's'},      {"优先级", "priority", 'p'}, {"状态", "status", 't'},
    {"项目", "project", 'j'},    {"截止日期", "due", 'd'},     {"情境", "context", 'c'},
    {"标签", "tags", 'g'},       {"进度", "progress", 'n'},    {"备注", "note", 'm'},
};
static const int k_gtd_field_count = (int)(sizeof(k_gtd_fields) / sizeof(k_gtd_fields[0]));

static const char *k_gtd_help[] = {
    "── 列表 ──",
    "↑↓     上下移动",
    "j/k    上移/下移任务",
    "h/l    提/降层级",
    "z/Z    折叠/展开 · 全部折叠展开",
    "a/i    添加任务/子任务",
    "r      重命名",
    "Enter  详情",
    "Space  切换状态",
    "Shift+↑↓ 多选",
    "d      删除(多选时批量)",
    "←/→    切换视图",
    "1-5    切换视图",
    "/      筛选",
    "c/t    情境/标签管理",
    "s      摘要",
    "A      归档管理",
    "n      新建项目  r 重命名项目",
    "?      本帮助",
    "",
    "── 详情 ──",
    "↑↓     上下字段",
    "Enter  编辑字段",
    "Space  切换状态/优先级",
    "s      摘要",
    "Esc    返回",
    "",
    "── 通用 ──",
    "Esc/q  返回/取消",
    "?      帮助",
};

static std::string today_string() {
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
    return buf;
}

static std::string make_id() {
    time_t now = time(nullptr);
    return std::to_string((long long)now) + "_" + std::to_string(rand() % 10000);
}

static std::string random_builtin_prompt() {
    if(BUILTIN_PROMPT_COUNT <= 0) return "今天发生了什么？";
    return BUILTIN_PROMPTS[rand() % BUILTIN_PROMPT_COUNT];
}

static std::string extract_deepseek_content(const std::string &response) {
    size_t p = response.find("\"content\":\"");
    if(p == std::string::npos) return "";
    p += 11;
    std::string out;
    bool esc = false;
    for(size_t i = p; i < response.size(); ++i) {
        char c = response[i];
        if(esc) {
            switch(c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
                if(i + 4 < response.size()) i += 4;
                out += '?';
                break;
            default: out += c; break;
            }
            esc = false;
        } else if(c == '\\') {
            esc = true;
        } else if(c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

static bool deepseek_chat(const std::string &body, std::string &out, std::string &err) {
    std::string key = g_settings.get("deepseek_key", "");
    if(key.empty()) {
        err = "请先在设置中配置Deepseek Key";
        return false;
    }
    if(!process::command_exists("curl")) {
        err = "未找到 curl";
        return false;
    }
    std::string path = "/tmp/pjournal-deepseek-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(rand() % 10000) + ".json";
    if(!safe_write_file(path, body)) {
        err = "无法写入请求文件";
        return false;
    }
    std::string response = process::run_capture({
        "curl", "-sS", "-m", "45",
        "-H", "Content-Type: application/json",
        "-H", "Authorization: Bearer " + key,
        "--data-binary", "@" + path,
        "https://api.deepseek.com/chat/completions",
    });
    remove(path.c_str());
    out = extract_deepseek_content(response);
    if(out.empty()) {
        err = response.empty() ? "Deepseek 无响应" : "Deepseek 响应解析失败";
        return false;
    }
    return true;
}

static bool deepseek_generate_prompt(std::string &prompt, std::string &err) {
    std::string ctx;
    std::string exp = g_settings.get("personal_exp", "");
    std::string hob = g_settings.get("personal_hob", "");
    if(!exp.empty()) ctx += "我的经历:" + exp + ";";
    if(!hob.empty()) ctx += "我的爱好:" + hob + ";";
    if(ctx.empty()) ctx = "一个普通用户";
    std::string body = "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是一个日记写作助手。根据用户的背景和爱好，生成一个富有洞见和启发性的日记写作提示，不超过56字，只输出提示。\"},"
        "{\"role\":\"user\",\"content\":\"我的背景：" + json_escape(ctx) + "。请生成一个写作提示。\"}],"
        "\"max_tokens\":100,\"temperature\":0.9}";
    return deepseek_chat(body, prompt, err);
}

static bool deepseek_polish_text(const std::string &text, const std::string &custom_instr,
                                 std::string &polished, std::string &err) {
    if(text.empty()) {
        err = "文本为空";
        return false;
    }
    if(text.size() > 8192) {
        err = "文本过长，请分段润色";
        return false;
    }
    static const char *default_system =
        "你是一个中文文本润色助手。请轻度润色用户文本，修正不通顺和断句问题，保留原文风格、事实和段落结构。只输出润色后的文本。";
    std::string system = g_settings.get("polish_prompt", "");
    if(system.empty()) system = default_system;
    if(!custom_instr.empty()) system += "。另外，用户额外要求：" + custom_instr;
    std::string body = "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + json_escape(system) + "\"},"
        "{\"role\":\"user\",\"content\":\"" + json_escape(text) + "\"}],"
        "\"max_tokens\":2000,\"temperature\":0.3}";
    return deepseek_chat(body, polished, err);
}

static std::string html_escape(const std::string &s) {
    std::string out;
    for(char c : s) {
        if(c == '&') out += "&amp;";
        else if(c == '<') out += "&lt;";
        else if(c == '>') out += "&gt;";
        else if(c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

static std::string text_to_flomo_html(const std::string &text) {
    std::string out;
    size_t pos = 0;
    while(pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        if(line.empty()) out += "<p><br></p>";
        else out += "<p>" + html_escape(line) + "</p>";
        if(nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

static bool flomo_sign(const std::string &params, std::string &sign, std::string &err) {
    std::string raw = params + "dbbc3dd73364b4084c3a69346e0ce2b2";
    sign = hash::md5_hex(raw);
    if(sign.empty()) {
        err = "Flomo签名失败";
        return false;
    }
    return true;
}

static bool flomo_send_text(const std::string &text, std::string &msg) {
    std::string token = g_settings.get("flomo_token", "");
    if(token.empty()) {
        msg = "请先在设置中配置Flomo Token";
        return false;
    }
    if(text.empty()) {
        msg = "正文为空";
        return false;
    }
    if(!process::command_exists("curl")) {
        msg = "未找到 curl";
        return false;
    }
    std::string html = text_to_flomo_html(text) + "\n\n<p>#日记</p>";
    time_t now = time(nullptr);
    std::string ts = std::to_string((long long)now);
    std::string params = "api_key=flomo_web&app_version=4.1&content=" + html +
                         "&platform=web&source=web&timestamp=" + ts + "&tz=8:0&webp=1";
    std::string sign;
    if(!flomo_sign(params, sign, msg)) return false;
    std::string body = "{\"timestamp\":\"" + ts + "\",\"api_key\":\"flomo_web\",\"app_version\":\"4.1\","
        "\"platform\":\"web\",\"webp\":\"1\",\"content\":\"" + json_escape(html) + "\","
        "\"source\":\"web\",\"tz\":\"8:0\",\"sign\":\"" + sign + "\"}";
    std::string path = "/tmp/pjournal-flomo-" + ts + "-" + std::to_string(rand() % 10000) + ".json";
    if(!safe_write_file(path, body)) {
        msg = "无法写入Flomo请求";
        return false;
    }
    std::string response = process::run_capture({
        "curl", "-sS", "-m", "45", "-X", "PUT",
        "-H", "Content-Type: application/json",
        "-H", "Authorization: Bearer " + token,
        "--data-binary", "@" + path,
        "https://flomoapp.com/api/v1/memo",
    });
    remove(path.c_str());
    if(response.find("\"code\":0") != std::string::npos) {
        msg = "已发送到Flomo";
        return true;
    }
    msg = response.empty() ? "Flomo无响应" : "Flomo发送失败";
    return false;
}

// 邮箱+密码登录换取 access_token,与 ESP32 的 login_by_email 同一套签名与请求体。
static bool flomo_generate_token(std::string &msg) {
    std::string email = g_settings.get("flomo_email", "");
    std::string pass = g_settings.get("flomo_pass", "");
    if(email.empty() || pass.empty()) {
        msg = "请先设置Flomo邮箱和密码";
        return false;
    }
    if(!process::command_exists("curl")) {
        msg = "未找到 curl";
        return false;
    }
    time_t now = time(nullptr);
    std::string ts = std::to_string((long long)now);
    std::string params = "api_key=flomo_web&app_version=4.1&email=" + email + "&password=" + pass +
                         "&platform=web&timestamp=" + ts + "&webp=1";
    std::string sign;
    if(!flomo_sign(params, sign, msg)) return false;
    std::string body = "{\"email\":\"" + json_escape(email) + "\",\"password\":\"" + json_escape(pass) +
                       "\",\"wechat_union_id\":\"\",\"wechat_oa_open_id\":\"\",\"timestamp\":\"" + ts +
                       "\",\"api_key\":\"flomo_web\",\"app_version\":\"4.1\",\"platform\":\"web\","
                       "\"webp\":\"1\",\"sign\":\"" + sign + "\"}";
    std::string path = "/tmp/pjournal-flomo-login-" + ts + ".json";
    if(!safe_write_file(path, body)) {
        msg = "无法写入Flomo请求";
        return false;
    }
    std::string response = process::run_capture({
        "curl", "-sS", "-m", "30", "-X", "POST",
        "-H", "Content-Type: application/json",
        "--data-binary", "@" + path,
        "https://flomoapp.com/api/v1/user/login_by_email",
    });
    remove(path.c_str());
    // {"code":0,"data":{"access_token":"..."}}
    std::string token;
    size_t dp = response.find("\"data\"");
    size_t tp = dp == std::string::npos ? std::string::npos : response.find("\"access_token\"", dp);
    if(tp == std::string::npos) tp = response.find("\"access_token\"");
    if(tp != std::string::npos) {
        size_t vs = response.find('"', tp + 14);
        if(vs != std::string::npos) {
            size_t ve = response.find('"', vs + 1);
            if(ve != std::string::npos) token = response.substr(vs + 1, ve - vs - 1);
        }
    }
    if(token.empty()) {
        msg = response.empty() ? "Flomo无响应" : "生成失败：邮箱或密码不正确";
        return false;
    }
    g_settings.set("flomo_token", token);
    msg = "已生成Flomo Token";
    return true;
}

static void gtd_save() {
    ensure_dir_path(gtd_dir());
    JsonValue::saveToFile(gtd_file(), g_gtd.data);
}

static bool gtd_match_view(const JsonValue &task, int view) {
    std::string status = task["status"].asString("todo");
    if(view == 0) return status == "todo";
    if(view == 1) return status == "doing";
    if(view == 2) return status == "waiting";
    if(view == 3) return !task["project"].asString().empty();
    if(view == 4) return status == "done";
    return false;
}

static JsonValue gtd_new_task(const std::string &title, const std::string &status,
                              const std::string &parent, const std::string &project) {
    JsonValue t = JsonValue::object();
    t.set("id", make_id());
    t.set("title", title);
    t.set("priority", "B");
    t.set("status", status);
    t.set("due", "");
    t.set("progress", 0);
    t.set("note", "");
    t.set("context", "");
    t.set("tags", JsonValue::array());
    t.set("project", project);
    t.set("parent", parent);
    t.set("created", today_string());
    t.set("completed", "");
    return t;
}

static void gtd_push_unique(std::vector<std::string> &dst, const std::string &v) {
    if(v.empty()) return;
    for(auto &x : dst)
        if(x == v) return;
    dst.push_back(v);
}

static void gtd_build_project_list() {
    g_gtd.projectList.clear();
    auto &projs = g_gtd.data["projects"];
    if(projs.isArray())
        for(int i = 0; i < (int)projs.size(); ++i) gtd_push_unique(g_gtd.projectList, projs[i].asString());
    auto &tasks = g_gtd.data["tasks"];
    if(tasks.isArray())
        for(int i = 0; i < (int)tasks.size(); ++i)
            gtd_push_unique(g_gtd.projectList, tasks[i]["project"].asString());
}

static void gtd_build_context_list() {
    g_gtd.contextList.clear();
    auto &arr = g_gtd.data["contexts"];
    if(arr.isArray())
        for(int i = 0; i < (int)arr.size(); ++i) gtd_push_unique(g_gtd.contextList, arr[i].asString());
    auto &tasks = g_gtd.data["tasks"];
    if(tasks.isArray())
        for(int i = 0; i < (int)tasks.size(); ++i)
            gtd_push_unique(g_gtd.contextList, tasks[i]["context"].asString());
}

static void gtd_build_tag_list() {
    g_gtd.tagList.clear();
    auto &arr = g_gtd.data["tags"];
    if(arr.isArray())
        for(int i = 0; i < (int)arr.size(); ++i) gtd_push_unique(g_gtd.tagList, arr[i].asString());
    auto &tasks = g_gtd.data["tasks"];
    if(tasks.isArray()) {
        for(int i = 0; i < (int)tasks.size(); ++i) {
            auto &tt = tasks[i]["tags"];
            if(!tt.isArray()) continue;
            for(int j = 0; j < (int)tt.size(); ++j) gtd_push_unique(g_gtd.tagList, tt[j].asString());
        }
    }
}

static void gtd_tree_walk(const std::vector<int> &scope, std::set<int> &used, int pos, int depth) {
    used.insert(pos);
    g_gtd.treeOrder.push_back(scope[pos]);
    g_gtd.treeDepth.push_back(depth);
    auto &tasks = g_gtd.data["tasks"];
    std::string pid = tasks[scope[pos]]["id"].asString();
    for(int ci = 0; ci < (int)scope.size(); ++ci) {
        if(used.count(ci)) continue;
        if(tasks[scope[ci]]["parent"].asString() != pid) continue;
        gtd_tree_walk(scope, used, ci, depth + 1);
    }
}

static void gtd_build_tree(const std::vector<int> &scope) {
    g_gtd.treeOrder.clear();
    g_gtd.treeDepth.clear();
    auto &tasks = g_gtd.data["tasks"];
    std::set<std::string> ids;
    for(int fi : scope) ids.insert(tasks[fi]["id"].asString());
    std::set<int> used;
    for(int i = 0; i < (int)scope.size(); ++i) {
        std::string pid = tasks[scope[i]]["parent"].asString();
        if(!pid.empty() && ids.count(pid)) continue;
        gtd_tree_walk(scope, used, i, 0);
    }
    // 父 id 成环或指向 scope 外的孤立节点兜底
    for(int i = 0; i < (int)scope.size(); ++i)
        if(!used.count(i)) gtd_tree_walk(scope, used, i, 0);
}

// 按 folded 把完整树压成可见列表
static void gtd_rebuild_visible() {
    g_gtd.filtered.clear();
    g_gtd.depth.clear();
    g_gtd.visPos.clear();
    int skipDepth = -1;
    for(int i = 0; i < (int)g_gtd.treeOrder.size(); ++i) {
        int d = g_gtd.treeDepth[i];
        if(skipDepth >= 0) {
            if(d > skipDepth) continue;
            skipDepth = -1;
        }
        g_gtd.filtered.push_back(g_gtd.treeOrder[i]);
        g_gtd.depth.push_back(d);
        g_gtd.visPos.push_back(i);
        if(g_gtd.folded.count(i)) skipDepth = d;
    }
}

static bool gtd_in_project_drill() {
    return g_gtd.view == 3 && g_gtd.projectDrill >= 0 &&
           g_gtd.projectDrill < (int)g_gtd.projectList.size();
}

static void gtd_clamp_sel() {
    if(g_gtd.sel >= (int)g_gtd.filtered.size()) g_gtd.sel = (int)g_gtd.filtered.size() - 1;
    if(g_gtd.sel < 0) g_gtd.sel = 0;
}

static void gtd_rebuild() {
    if(g_gtd.data.isNull() || !g_gtd.data.has("tasks") || !g_gtd.data["tasks"].isArray()) {
        g_gtd.data = JsonValue::object();
        g_gtd.data.set("tasks", JsonValue::array());
        g_gtd.data.set("projects", JsonValue::array());
        g_gtd.data.set("contexts", JsonValue::array());
        g_gtd.data.set("tags", JsonValue::array());
    }
    if(g_gtd.view == 3) gtd_build_project_list();
    bool drilled = gtd_in_project_drill();
    auto &tasks = g_gtd.data["tasks"];
    std::string proj = drilled ? g_gtd.projectList[g_gtd.projectDrill] : std::string();
    std::vector<int> scope;
    for(int i = 0; i < (int)tasks.size(); ++i) {
        auto &t = tasks[i];
        if(t.isNull() || !gtd_match_view(t, g_gtd.view)) continue;
        if(drilled && t["project"].asString() != proj) continue;
        if(!g_gtd.filterText.empty()) {
            if(t["title"].asString().find(g_gtd.filterText) == std::string::npos &&
               t["note"].asString().find(g_gtd.filterText) == std::string::npos)
                continue;
        }
        if(!g_gtd.filterContext.empty() && t["context"].asString() != g_gtd.filterContext) continue;
        if(!g_gtd.filterTags.empty()) {
            auto &tt = t["tags"];
            bool all = true;
            for(auto &ft : g_gtd.filterTags) {
                bool found = false;
                if(tt.isArray())
                    for(int j = 0; j < (int)tt.size(); ++j)
                        if(tt[j].asString() == ft) { found = true; break; }
                if(!found) { all = false; break; }
            }
            if(!all) continue;
        }
        scope.push_back(i);
    }
    if(drilled) {
        gtd_build_tree(scope);
    } else {
        g_gtd.treeOrder = scope;
        g_gtd.treeDepth.assign(scope.size(), 0);
        g_gtd.folded.clear();
    }
    gtd_rebuild_visible();
    if(!(g_gtd.view == 3 && g_gtd.projectDrill < 0)) gtd_clamp_sel();
}

static void gtd_load() {
    ensure_dir_path(gtd_dir());
    JsonValue v = JsonValue::loadFromFile(gtd_file());
    if(v.isNull() || !v.has("tasks") || !v["tasks"].isArray()) {
        g_gtd.data = JsonValue::object();
        g_gtd.data.set("tasks", JsonValue::array());
    } else {
        g_gtd.data = v;
        // 清掉历史上损坏产生的 null 项
        auto &tasks = g_gtd.data["tasks"];
        int w = 0;
        for(int i = 0; i < (int)tasks.size(); ++i)
            if(!tasks[i].isNull()) tasks.elements[w++] = tasks[i];
        tasks.elements.resize(w);
    }
    if(!g_gtd.data.has("projects") || !g_gtd.data["projects"].isArray())
        g_gtd.data.set("projects", JsonValue::array());
    if(!g_gtd.data.has("contexts") || !g_gtd.data["contexts"].isArray())
        g_gtd.data.set("contexts", JsonValue::array());
    if(!g_gtd.data.has("tags") || !g_gtd.data["tags"].isArray())
        g_gtd.data.set("tags", JsonValue::array());
    g_gtd.view = 0;
    g_gtd.sel = 0;
    g_gtd.scroll = 0;
    g_gtd.mode = GtdMode::List;
    g_gtd.projectDrill = -1;
    g_gtd.detailIdx = -1;
    g_gtd.folded.clear();
    g_gtd.multiSel.clear();
    g_gtd.multiAnchor = -1;
    g_gtd.filterText.clear();
    g_gtd.filterContext.clear();
    g_gtd.filterTags.clear();
    gtd_rebuild();
}

// ── 归档 ─────────────────────────────────────────────────────────────────

static std::string gtd_current_month() {
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m", &local);
    return buf;
}

static void gtd_auto_archive() {
    std::string cur = gtd_current_month();
    if(g_gtd.data["lastArchive"].asString() == cur) return;
    ensure_dir_path(gtd_archive_dir());
    auto &tasks = g_gtd.data["tasks"];
    std::map<std::string, std::vector<int>> byMonth;
    for(int i = 0; i < (int)tasks.size(); ++i) {
        if(tasks[i]["status"].asString("todo") != "done") continue;
        std::string completed = tasks[i]["completed"].asString();
        if(completed.size() < 7) continue;
        std::string month = completed.substr(0, 7);
        if(month >= cur) continue;
        byMonth[month].push_back(i);
    }
    bool any = false;
    std::vector<int> removeIdx;
    for(auto &kv : byMonth) {
        std::string path = gtd_archive_dir() + "/" + kv.first + ".json";
        JsonValue arc = JsonValue::loadFromFile(path);
        if(arc.isNull() || !arc.has("tasks") || !arc["tasks"].isArray()) {
            arc = JsonValue::object();
            arc.set("tasks", JsonValue::array());
            arc.set("archiveDate", kv.first);
        }
        for(int idx : kv.second) arc["tasks"].pushBack(tasks[idx]);
        JsonValue::saveToFile(path, arc);
        for(int idx : kv.second) removeIdx.push_back(idx);
        any = true;
    }
    std::sort(removeIdx.rbegin(), removeIdx.rend());
    for(int idx : removeIdx)
        if(idx >= 0 && idx < (int)tasks.elements.size())
            tasks.elements.erase(tasks.elements.begin() + idx);
    g_gtd.data.set("lastArchive", cur);
    if(any) gtd_save();
}

static void gtd_load_archive_months() {
    g_gtd.archiveMonths.clear();
    g_gtd.archiveCounts.clear();
    ensure_dir_path(gtd_archive_dir());
    DIR *d = opendir(gtd_archive_dir().c_str());
    if(!d) return;
    struct dirent *e;
    while((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if(name.size() != 12 || name.substr(7) != ".json") continue;
        if(name[4] != '-') continue;
        JsonValue arc = JsonValue::loadFromFile(gtd_archive_dir() + "/" + name);
        int n = 0;
        if(!arc.isNull() && arc["tasks"].isArray()) n = (int)arc["tasks"].size();
        g_gtd.archiveMonths.push_back(name.substr(0, 7));
        g_gtd.archiveCounts.push_back(n);
    }
    closedir(d);
    // 文件名形如 YYYY-MM,按月份降序(新的在前)
    for(size_t i = 0; i + 1 < g_gtd.archiveMonths.size(); ++i)
        for(size_t j = i + 1; j < g_gtd.archiveMonths.size(); ++j)
            if(g_gtd.archiveMonths[j] > g_gtd.archiveMonths[i]) {
                std::swap(g_gtd.archiveMonths[i], g_gtd.archiveMonths[j]);
                std::swap(g_gtd.archiveCounts[i], g_gtd.archiveCounts[j]);
            }
}

static void gtd_load_archive_month(const std::string &month) {
    g_gtd.archiveTasks.clear();
    JsonValue arc = JsonValue::loadFromFile(gtd_archive_dir() + "/" + month + ".json");
    if(arc.isNull() || !arc["tasks"].isArray()) return;
    for(int i = 0; i < (int)arc["tasks"].size(); ++i) g_gtd.archiveTasks.push_back(arc["tasks"][i]);
}

static std::string gtd_export_md() {
    std::string md = "# GTD 任务列表\n\n";
    auto &tasks = g_gtd.data["tasks"];
    if(!tasks.isArray()) return md;
    for(int i = 0; i < (int)tasks.size(); ++i) {
        auto &t = tasks[i];
        std::string status = t["status"].asString("todo");
        std::string pri = t["priority"].asString();
        const char *mark = "[ ]";
        if(status == "doing") mark = "[>]";
        else if(status == "done") mark = "[x]";
        else if(status == "waiting") mark = "[~]";
        md += "- " + std::string(mark) + " " + t["title"].asString();
        if(!pri.empty()) md += " **" + pri + "**";
        md += "\n";
    }
    return md;
}

static std::string outline_dir() {
    return home_dir() + "/.pjournal-lvgl/outline";
}

static std::string inspiration_file() {
    return outline_dir() + "/inspiration.json";
}

static void inspiration_normalize() {
    if(g_inspiration_data.isNull() || !g_inspiration_data.has("items") || !g_inspiration_data["items"].isArray()) {
        g_inspiration_data = JsonValue::object();
        g_inspiration_data.set("items", JsonValue::array());
    }
}

static void inspiration_save() {
    ensure_dir_path(outline_dir());
    JsonValue::saveToFile(inspiration_file(), g_inspiration_data);
}

static std::string ascii_lower(std::string s) {
    for(char &c : s) if(c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

static void inspiration_rebuild() {
    inspiration_normalize();
    g_inspiration_filtered.clear();
    auto &items = g_inspiration_data["items"];
    std::string q = ascii_lower(g_inspiration_query);
    for(int i = 0; i < (int)items.size(); ++i) {
        if(q.empty()) {
            g_inspiration_filtered.push_back(i);
            continue;
        }
        std::string text = ascii_lower(items[i]["content"].asString() + " " + items[i]["keywords"].asString());
        if(text.find(q) != std::string::npos) g_inspiration_filtered.push_back(i);
    }
    if(g_inspiration_sel >= (int)g_inspiration_filtered.size()) g_inspiration_sel = (int)g_inspiration_filtered.size() - 1;
    if(g_inspiration_sel < 0) g_inspiration_sel = 0;
}

static void inspiration_load() {
    ensure_dir_path(outline_dir());
    g_inspiration_data = JsonValue::loadFromFile(inspiration_file());
    inspiration_rebuild();
}

static int inspiration_selected_item_index() {
    if(g_inspiration_filtered.empty()) return -1;
    if(g_inspiration_sel < 0 || g_inspiration_sel >= (int)g_inspiration_filtered.size()) return -1;
    return g_inspiration_filtered[g_inspiration_sel];
}

static std::string utf8_preview(const std::string &s, int max_chars) {
    size_t pos = 0;
    int chars = 0;
    while(pos < s.size() && chars < max_chars) {
        unsigned char c = (unsigned char)s[pos];
        size_t n = 1;
        if((c & 0xE0) == 0xC0) n = 2;
        else if((c & 0xF0) == 0xE0) n = 3;
        else if((c & 0xF8) == 0xF0) n = 4;
        if(pos + n > s.size()) break;
        pos += n;
        ++chars;
    }
    std::string out = s.substr(0, pos);
    if(pos < s.size()) out += "...";
    return out;
}

static std::string outline_project_dir(const std::string &name) {
    return outline_dir() + "/" + name;
}

static std::string outline_project_file(const std::string &name) {
    return outline_project_dir(name) + "/project.json";
}

static bool safe_project_name(const std::string &name) {
    return !name.empty() && name.find('/') == std::string::npos && name.find("..") == std::string::npos;
}

static void outline_list_projects() {
    ensure_dir_path(outline_dir());
    g_ol.projects.clear();
    DIR *d = opendir(outline_dir().c_str());
    if(!d) return;
    dirent *de = nullptr;
    while((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if(name == "." || name == ".." || !safe_project_name(name)) continue;
        if(file_exists(outline_project_file(name))) g_ol.projects.push_back(name);
    }
    closedir(d);
    std::sort(g_ol.projects.begin(), g_ol.projects.end());
    if(g_ol.sel >= (int)g_ol.projects.size()) g_ol.sel = (int)g_ol.projects.size() - 1;
    if(g_ol.sel < 0) g_ol.sel = 0;
}

static void outline_normalize() {
    outline_normalize_data(g_ol.data);
}

static void outline_load_project(const std::string &name) {
    g_ol.project = name;
    g_ol.data = JsonValue::loadFromFile(outline_project_file(name));
    outline_normalize();
}

static void outline_save() {
    if(g_ol.project.empty()) return;
    ensure_dir_path(outline_project_dir(g_ol.project));
    JsonValue::saveToFile(outline_project_file(g_ol.project), g_ol.data);
}

static JsonValue &outline_nodes() { return g_ol.data["nodes"]; }

static bool outline_has_children(int idx) {
    return outline_has_children(outline_nodes(), idx);
}

static std::string outline_tree_prefix(int idx) {
    return outline_tree_prefix(outline_nodes(), idx);
}

// 折叠 + 标签筛选 + 文本筛选后的可见节点下标。
static void outline_rebuild_filter() {
    g_ol.filtered = outline_filter_nodes(outline_nodes(), g_ol.folded, g_ol.filterTags, g_ol.filterText);
    if(g_ol.sel >= (int)g_ol.filtered.size()) g_ol.sel = (int)g_ol.filtered.size() - 1;
    if(g_ol.sel < 0) g_ol.sel = 0;
}

static int outline_cur_node() {
    if(g_ol.sel < 0 || g_ol.sel >= (int)g_ol.filtered.size()) return -1;
    return g_ol.filtered[g_ol.sel];
}

static void outline_build_tags() {
    g_ol.tagList.clear();
    auto add = [](const std::string &t) {
        if(t.empty()) return;
        for(auto &x : g_ol.tagList)
            if(x == t) return;
        g_ol.tagList.push_back(t);
    };
    if(g_ol.data.has("tags") && g_ol.data["tags"].isArray()) {
        auto &a = g_ol.data["tags"];
        for(int i = 0; i < (int)a.size(); ++i) add(a[i].asString());
    }
    auto &n = outline_nodes();
    for(int i = 0; i < (int)n.size(); ++i) {
        if(!n[i].has("tags")) continue;
        auto &tt = n[i]["tags"];
        if(tt.isArray())
            for(int j = 0; j < (int)tt.size(); ++j) add(tt[j].asString());
    }
    std::sort(g_ol.tagList.begin(), g_ol.tagList.end());
}

static std::string outline_content_path(const std::string &file) {
    return outline_project_dir(g_ol.project) + "/" + file;
}

static std::string outline_ensure_content_file(const std::string &file) {
    ensure_dir_path(outline_project_dir(g_ol.project));
    std::string p = outline_content_path(file);
    FILE *f = fopen(p.c_str(), "rb");
    if(f) fclose(f);
    else { f = fopen(p.c_str(), "w"); if(f) fclose(f); }
    return p;
}

static std::string outline_read_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if(!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(sz <= 0) { fclose(f); return ""; }
    if(sz > 256 * 1024) sz = 256 * 1024;
    std::string c((size_t)sz, '\0');
    size_t rd = fread(&c[0], 1, (size_t)sz, f);
    fclose(f);
    c.resize(rd);
    return c;
}

static const char *k_outline_status[] = {"draft", "active", "done", "revise"};
static const char *k_outline_status_disp[] = {"草稿", "进行中", "已完成", "待修改"};
static const char *k_outline_status_sym[] = {"○", "◐", "●", "✎"};
static const int k_outline_status_count = 4;

static int outline_status_index(const std::string &s) {
    for(int i = 0; i < k_outline_status_count; ++i)
        if(s == k_outline_status[i]) return i;
    return 0;
}

static bool outline_is_bookmarked(const std::string &id) {
    auto &bm = g_ol.data["bookmarks"];
    if(!bm.isArray()) return false;
    for(int i = 0; i < (int)bm.size(); ++i)
        if(bm[i]["id"].asString() == id) return true;
    return false;
}

static std::string outline_export_md() {
    if(g_ol.project.empty()) return "# 大纲\n";
    std::string md = "# " + g_ol.project + "\n\n";
    auto &n = outline_nodes();
    for(int i = 0; i < (int)n.size(); ++i) {
        int lvl = n[i]["level"].asInt(0);
        std::string prefix;
        for(int j = 0; j <= lvl && j < 6; ++j) prefix += "#";
        md += prefix + " " + n[i]["title"].asString() + "\n";
        std::string file = n[i]["file"].asString();
        if(!file.empty()) {
            std::string content = outline_read_file(outline_content_path(file));
            if(!content.empty()) md += "\n" + content + "\n\n";
        }
    }
    return md;
}

static void outline_delete_project_dir(const std::string &name) {
    std::string dir = outline_project_dir(name);
    DIR *d = opendir(dir.c_str());
    if(d) {
        dirent *de = nullptr;
        while((de = readdir(d)) != nullptr) {
            std::string fn = de->d_name;
            if(fn == "." || fn == "..") continue;
            remove((dir + "/" + fn).c_str());
        }
        closedir(d);
    }
    remove(dir.c_str());
}

static void outline_remove_file(const std::string &project, const std::string &file) {
    if(file.empty() || project.empty()) return;
    remove((outline_project_dir(project) + "/" + file).c_str());
}

static void apply_theme_values() {
    if(g_settings.theme() == "light") {
        g_theme = {lv_color_white(), lv_color_black(), lv_color_hex(0x555555), lv_color_hex(0x111111), lv_color_hex(0xf4f4f4)};
    } else {
        g_theme = {lv_color_black(), lv_color_white(), lv_color_hex(0xaaaaaa), lv_color_white(), lv_color_hex(0x111111)};
    }
}

static void base_style(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(obj, g_theme.fg, 0);
    lv_obj_set_style_border_color(obj, g_theme.fg, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
}

static lv_obj_t *label(lv_obj_t *parent, const std::string &text, int x, int y, int w = LV_SIZE_CONTENT, int h = LV_SIZE_CONTENT) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text.c_str());
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_color(obj, g_theme.fg, 0);
    // 内容比框略高时 LVGL 默认会在右/下画滚动条,这里一律不要
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, bool selected = false) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(obj, g_theme.fg, 0);
    lv_obj_set_style_bg_color(obj, selected ? g_theme.fg : g_theme.bg, 0);
    lv_obj_set_style_text_color(obj, selected ? g_theme.bg : g_theme.fg, 0);
    lv_obj_set_style_pad_all(obj, 4, 0);
    // 子控件比内容区略大是常态,不要让 LVGL 画出右/下滚动条
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// 图标字体(Nerd Font)的 PUA 字形在字宽里基本靠左,照字宽居中会让墨迹整体偏右;
// 有些字形(如键盘)的墨迹还比字宽宽,按内容自适应尺寸会被 LVGL 裁掉一截。
// 所以标签铺满整个图标框、文字居中,再按字形墨迹盒(ofs_x/box_w)补一次偏移。
static void center_ink(lv_obj_t *lbl, const char *txt) {
    int dx = 0;
    const lv_font_t *f = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
    if(f && txt && *txt) {
        uint32_t idx = 0;
        uint32_t cp = lv_text_encoded_next(txt, &idx);
        lv_font_glyph_dsc_t dsc {};
        if(lv_font_get_glyph_dsc(f, &dsc, cp, 0) && dsc.box_w)
            dx = (int)dsc.adv_w / 2 - dsc.ofs_x - dsc.box_w / 2;
    }
    lv_obj_align(lbl, LV_ALIGN_CENTER, dx, 0);
}

static void set_status(const std::string &left, const std::string &right = "") {
    if(!g_status) return;
    lv_label_set_text(g_status, left.c_str());
    // 右侧是输入法/电池这类常驻内容,单独传 right 时才覆盖,免得临时提示把它抹掉
    if(g_status_right && !right.empty()) lv_label_set_text(g_status_right, right.c_str());
}

// 状态栏高度跟着 UI 字体走,换字号也不会把字的下半截切掉
static int status_bar_h() {
    int lh = lv_font_get_line_height(g_ui_font ? g_ui_font : LV_FONT_DEFAULT);
    if(lh < 16) lh = 16;
    return lh + 8;
}
static int status_bar_y() { return 596 - status_bar_h(); }
// 正文能占到的最下边(状态栏上方再留一点空白)
static int chrome_bottom() { return status_bar_y() - 4; }

// The UI chrome keeps one fixed face so menus, status bar and calendar stay
// visually stable; the 字体 setting only swaps the editor's text font.
static const char *k_ui_font_path = "/root/.fonts/Go-LavaPropo-NF-R.ttf";

void app_ui_reload_font() {
    std::string ui_path = k_ui_font_path;
    if(!g_fonts.supports_cjk(ui_path)) ui_path = g_fonts.default_font_path();
    g_ui_font = g_fonts.load(ui_path, 27);
    g_icon_font = g_fonts.load(ui_path, 60);
    g_ime_font = g_fonts.load(ui_path, g_settings.ime_font_size());
    // 候选分页是按字形宽度算好的,字号一换就得作废重排
    g_linux_ime.set_display_width(0);

    std::string editor_path = g_settings.font_file();
    if(editor_path.empty() || !g_fonts.supports_cjk(editor_path)) editor_path = g_fonts.default_font_path();
    int esize = g_settings.font_size();
    g_editor_font = g_fonts.load(editor_path, esize);
    // 粗体/斜体各自可指一个真字体;留空就用伪粗体/伪斜体。
    std::string bold_path = g_settings.font_bold_file();
    std::string italic_path = g_settings.font_italic_file();
    g_editor_bold_font = bold_path.empty() ? nullptr : g_fonts.load(bold_path, esize);
    g_editor_italic_font = italic_path.empty() ? nullptr : g_fonts.load(italic_path, esize);
    g_editor_faux_italic = g_fonts.load(editor_path, esize, FontStyle::Italic);
    // 粗体/斜体多半是拉丁字体,不含中日韩,缺字得往下回退,否则混排里的中文变成豆腐块。
    // 斜体回退到伪斜体(正文路径 + 斜切)而不是正文字体,这样中文照样是斜的;
    // 伪粗体没有对应的字体对象(靠描两遍实现),粗体只能退回正文字体,中文不加粗。
    // 两者的下一跳都是 FontManager 挂的符号回退,整条链不断。
    if(g_editor_font) {
        if(g_editor_bold_font && g_editor_bold_font != g_editor_font)
            g_editor_bold_font->fallback = g_editor_font;
        if(g_editor_italic_font && g_editor_italic_font != g_editor_faux_italic && g_editor_faux_italic)
            g_editor_italic_font->fallback = g_editor_faux_italic;
    }

    lv_font_t *font = g_ui_font ? g_ui_font : g_editor_font;
    g_current_font = font;
    lv_style_reset(&g_style);
    lv_style_init(&g_style);
    lv_style_set_bg_color(&g_style, g_theme.bg);
    lv_style_set_bg_opa(&g_style, LV_OPA_COVER);
    lv_style_set_text_color(&g_style, g_theme.fg);
    if(font) lv_style_set_text_font(&g_style, font);
    if(g_root) lv_obj_add_style(g_root, &g_style, 0);
}

void app_ui_reload_theme() {
    apply_theme_values();
    app_ui_reload_font();
    render();
}

static void clear_root() {
    lv_obj_clean(g_root);
    g_status = nullptr;
    g_status_right = nullptr;
    g_editor = nullptr;
    g_file_panel = nullptr;
    g_setting_text = nullptr;
    g_gtd.input = nullptr;
    g_ol.input = nullptr;
    g_wifi_password = nullptr;
    g_inspiration_text = nullptr;
    g_dict_text = nullptr;
    g_ime_bar = nullptr;
    g_search_panel = nullptr;
    g_search_input = nullptr;
    g_search_replace = nullptr;
    g_search_focus_rep = false;
    g_search_matches.clear();
    g_search_cur = -1;
    g_search_last_term.clear();
    g_search_last_text.clear();
    g_polish_panel = nullptr;
    g_polish_preview = nullptr;
    g_polish_instr = nullptr;
    g_polish_instr_mode = false;
    g_md_view = nullptr;
    g_md_content = nullptr;
    g_md_caret = nullptr;
    g_focus_dim[0] = nullptr;
    g_focus_dim[1] = nullptr;
    g_md_focus_band_on = false;
    g_md_labels.clear();
    g_md_rules.clear();
    g_md_sel_rects.clear();
    g_md_box_rects.clear();
    g_md_dot_rects.clear();
    g_md_wave_rects.clear();
    g_ro_view = nullptr;
    g_ro_content = nullptr;
    g_ro_labels.clear();
    g_ro_rules.clear();
    g_viewer_line_y.clear();
    g_vt_view = nullptr;
    g_vt_caret = nullptr;
    g_editor_exit_dlg = nullptr;
    g_quit_dlg = nullptr;
    g_quit_asking = false;
    g_vt_cols.clear();
    g_vt_marks.clear();
    g_vt_cells.clear();
    g_vt_prompt_cols.clear();
    g_vt_box_rects.clear();
    g_vt_deco_rects.clear();
    g_ed_sel_anchor = -1;
    base_style(g_root);
}

// right 非空时在状态栏右侧另起一个靠右对齐的标签(如输入法状态 + 电池)
static void draw_status_bar(const std::string &left = "", bool align_right = false,
                            const std::string &right = "") {
    int y = status_bar_y();
    int h = status_bar_h();
    g_status = label(g_root, left, 8, y, 1000, h);
    lv_obj_set_style_text_align(g_status, align_right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_border_width(g_status, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(g_status, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_pad_top(g_status, 4, 0);
    g_status_right = nullptr;
    if(!right.empty()) {
        g_status_right = label(g_root, right, 8, y, 1000, h);
        lv_obj_set_style_text_align(g_status_right, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_pad_top(g_status_right, 4, 0);
    }
}

static lv_obj_t *active_textarea() {
    if(g_polish_instr) return g_polish_instr;
    if(g_search_input) return g_search_input;
    if(g_gtd.input) return g_gtd.input;
    if(g_ol.input) return g_ol.input;
    if(g_inspiration_text) return g_inspiration_text;
    if(g_dict_text) return g_dict_text;
    if(g_setting_text) return g_setting_text;
    if(g_wifi_password) return g_wifi_password;
    if(g_editor) return g_editor;
    return nullptr;
}

static const lv_font_t *active_text_font(lv_obj_t *ta) {
    if(ta && ta == g_editor && g_editor_font) return g_editor_font;
    if(g_ui_font) return g_ui_font;
    if(g_current_font) return g_current_font;
    return LV_FONT_DEFAULT;
}

static const lv_font_t *ime_font() {
    if(g_ime_font) return g_ime_font;
    if(g_ui_font) return g_ui_font;
    return LV_FONT_DEFAULT;
}

// 候选条按像素宽度分页要好 IME 自己知道每个候选用当前字体占多宽
static int ime_measure_width(const char *text) {
    if(!text) return 0;
    return (int)lv_text_get_width(text, (uint32_t)strlen(text), ime_font(), 0);
}

// 候选条高度跟着输入法字号走,字号调大也不会切掉下半截
static int ime_bar_h() {
    int lh = lv_font_get_line_height(ime_font());
    if(lh < 12) lh = 12;
    return lh + 8;
}

static void create_ime_bar() {
    if(g_ime_bar) return;
    g_ime_bar = label(g_root, "", 8, chrome_bottom() - ime_bar_h(), 656, ime_bar_h());
    lv_label_set_long_mode(g_ime_bar, LV_LABEL_LONG_CLIP);
    if(g_ime_font) lv_obj_set_style_text_font(g_ime_bar, g_ime_font, 0);
    lv_obj_set_style_bg_color(g_ime_bar, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(g_ime_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_ime_bar, g_theme.fg, 0);
    lv_obj_set_style_border_width(g_ime_bar, 1, 0);
    lv_obj_set_style_pad_all(g_ime_bar, 4, 0);
}

static std::string editor_status_text() {
    std::string mode;
    if(g_quick_slot >= 0) mode = "快捷编辑 " + std::to_string(g_quick_slot) + ".txt";
    else if(!g_file_edit_path.empty()) {
        size_t slash = g_file_edit_path.find_last_of('/');
        mode = "文件 " + (slash == std::string::npos ? g_file_edit_path : g_file_edit_path.substr(slash + 1));
    }
    else if(!g_ol.editPath.empty()) mode = "大纲文件 " + g_ol.project;
    else if(g_edit_file.empty()) mode = g_prompt.empty() ? "自由写作" : "提示写作";
    else mode = "编辑 " + g_edit_file;
    return mode;
}

static void file_panel_reload(const std::string &prefer = "") {
    auto &p = g_file_panel_state;
    p.entries = file_edit_entries();
    if(p.entries.empty()) {
        file_edit_ensure_default();
        p.entries = file_edit_entries();
    }
    std::string wanted = prefer;
    if(wanted.empty() && !g_file_edit_path.empty()) {
        size_t slash = g_file_edit_path.find_last_of('/');
        wanted = slash == std::string::npos ? g_file_edit_path : g_file_edit_path.substr(slash + 1);
    }
    for(int i = 0; i < (int)p.entries.size(); ++i)
        if(p.entries[i] == wanted) { p.sel = i; break; }
    if(p.sel < 0) p.sel = 0;
    if(p.sel >= (int)p.entries.size()) p.sel = (int)p.entries.size() - 1;
    if(p.sel < 0) p.sel = 0;
}

// 文件管理面板的宽度。候选条定位(update_ime_bar)也按它推算,所以放这儿共用一个值。
static const int kFilePanelW = 420;

static void draw_file_panel() {
    if(!g_file_panel_state.active) {
        if(g_file_panel) { lv_obj_delete(g_file_panel); g_file_panel = nullptr; }
        return;
    }
    if(g_file_panel) { lv_obj_delete(g_file_panel); g_file_panel = nullptr; }
    auto &p = g_file_panel_state;
    const int panel_w = kFilePanelW;
    g_file_panel = box(g_root, 0, 0, panel_w, 600, false);
    lv_obj_set_style_bg_color(g_file_panel, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(g_file_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_file_panel, 2, 0);
    label(g_file_panel, "文件", 8, 8, panel_w - 16, 34);
    int visible = 10;
    if(p.sel < p.scroll) p.scroll = p.sel;
    if(p.sel >= p.scroll + visible) p.scroll = p.sel - visible + 1;
    if(p.scroll < 0) p.scroll = 0;
    for(int i = 0; i < visible && p.scroll + i < (int)p.entries.size(); ++i) {
        int idx = p.scroll + i;
        lv_obj_t *r = box(g_file_panel, 8, 54 + i * 42, panel_w - 16, 38, idx == p.sel);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_t *l = label(r, p.entries[idx], 6, 5, panel_w - 28, 28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        if(idx == p.sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
    if(p.prompt) {
        std::string title = p.rename ? "改名: " : "新建: ";
        lv_obj_t *l = label(g_file_panel, title + p.input, 8, 500, panel_w - 16, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    } else if(!p.message.empty()) {
        label(g_file_panel, p.message, 8, 500, panel_w - 16, 30);
    }
    // 组字时候选条占着提示行那一格,这一行就撤掉;a新建/d删除那排键位提示在打字途中
    // 只会抢地方,也没有用
    if(!p.prompt) {
        lv_obj_t *hint = label(g_file_panel,
                               p.confirmDelete ? "Enter确认删除 Esc取消" : "a新建 d删除 r改名 Enter编辑",
                               8, 548, panel_w - 16, 28);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
    }
    lv_obj_move_foreground(g_file_panel);
    // 面板是不透明的整高方块,候选条就摆在它里面(左下提示行下面)。不把候选条再抬一次
    // 的话,面板盖在候选条上,LVGL 认定候选条被完全遮住就不画它了,组字时看不到候选。
    if(g_ime_bar && !lv_obj_has_flag(g_ime_bar, LV_OBJ_FLAG_HIDDEN)) lv_obj_move_foreground(g_ime_bar);
}

static std::string editor_ime_text() {
    if(!g_linux_ime.active()) return "EN";
    if(g_linux_ime.english()) return "[英]";
    return std::string("[中]") + (g_linux_ime.fullwidth() ? "●" : "◐") + (g_linux_ime.trad() ? "繁" : "简");
}

// 状态栏右侧:时间 + 输入法状态 + WiFi + 电池,顺序与主面板一致
static std::string editor_right_text() {
    return clock_label() + " " + editor_ime_text() + " " + wifi_status_label() + " " +
           battery_percent_label();
}

static void refresh_ime_status() {
    if(g_screen != Screen::Editor || !g_status_right) return;
    lv_label_set_text(g_status_right, editor_right_text().c_str());
}

// 横排:把候选条真正可用的像素宽度交给 IME,由它按字形宽度分页——一页塞不下的
// 候选整项挪到下一页,编号从 1 重新计。这里只把当前页原样画出来,不再自己截断,
// 否则会出现"页里藏着 7/8/9 却看不见、翻页又从别处开始"的错位。
static void ime_bar_layout_horizontal(int x, int y, int bar_w) {
    const lv_font_t *f = ime_font();
    std::string head = " " + g_linux_ime.composition() + "  ";
    int head_w = f ? (int)lv_text_get_width(head.c_str(), (uint32_t)head.size(), f, 0) : 0;
    // 页码在 buildPage 之后才定,这里按两位数页码预留,免得页数从 9 变 10 时撑破行宽
    int tail_w = f ? (int)lv_text_get_width(" 88/88", 6, f, 0) : 0;
    // v 模式的命中候选带方括号,宽度也留出来,免得它被挤到下一页去高亮
    int br_w = (f && g_linux_ime.highlight_index() >= 0)
                   ? (int)lv_text_get_width("[]", 2, f, 0) : 0;
    int budget = bar_w - 8 - head_w - tail_w - br_w;  // 8 = create_ime_bar 的左右内边距
    if(budget < 80) budget = 80;
    if(budget > bar_w - 8) budget = bar_w - 8;
    g_linux_ime.set_display_width(budget);

    std::string tail = " " + std::to_string(g_linux_ime.current_page()) + "/" +
                       std::to_string(g_linux_ime.total_pages());
    // 前缀和 buildPage 量宽的 " 编号." + 候选 完全一致,分页与渲染才不会错位
    std::string s = head;
    const auto &c = g_linux_ime.candidates();
    int hi = g_linux_ime.highlight_index();
    for(size_t i = 0; i < c.size(); ++i) {
        std::string part = std::to_string((int)i + 1) + "." + c[i];
        s += ((int)i == hi) ? " [" + part + "]" : " " + part;
    }
    s += tail;

    lv_obj_set_style_text_line_space(g_ime_bar, 0, 0);
    lv_label_set_text(g_ime_bar, s.c_str());
    lv_obj_set_pos(g_ime_bar, x, y);
    lv_obj_set_size(g_ime_bar, bar_w, ime_bar_h());
}

// 竖排:正文是一列列竖着的字,候选条排在光标那一列旁边竖着铺开,一行一个候选。
// 分页固定 9 个,框体高度也就按满页 9 行定死,候选不够一页也不缩;整块在竖直方向
// 固定居中,不跟着光标上下跑——否则每敲一个字框就跳一下,反而看不清候选。
static void ime_bar_layout_vertical() {
    const lv_font_t *f = ime_font();
    int lh = f ? lv_font_get_line_height(f) : 24;
    int row_h = lh + 2;
    static constexpr int kRows = 9;

    lv_obj_update_layout(g_vt_view);
    lv_area_t va {};
    lv_obj_get_coords(g_vt_view, &va);
    bool caret_on = g_vt_caret && !lv_obj_has_flag(g_vt_caret, LV_OBJ_FLAG_HIDDEN);
    lv_area_t cc {};
    if(caret_on) lv_obj_get_coords(g_vt_caret, &cc);

    g_linux_ime.set_display_width(0);  // 竖排按行数分页,不走像素宽度那条路
    g_linux_ime.set_page_size(kRows);

    const auto &c = g_linux_ime.candidates();
    int hi = g_linux_ime.highlight_index();
    std::string head = " " + g_linux_ime.composition() + "  " +
                       std::to_string(g_linux_ime.current_page()) + "/" +
                       std::to_string(g_linux_ime.total_pages());
    int wmax = f ? (int)lv_text_get_width(head.c_str(), (uint32_t)head.size(), f, 0) : 0;
    std::string s = head;
    for(size_t i = 0; i < c.size(); ++i) {
        std::string part = std::to_string((int)i + 1) + "." + c[i];
        if((int)i == hi) part = "[" + part + "]";
        s += "\n" + part;
        int pw = f ? (int)lv_text_get_width(part.c_str(), (uint32_t)part.size(), f, 0) : 0;
        if(pw > wmax) wmax = pw;
    }

    int w = wmax + 14;
    if(w < 120) w = 120;
    if(w > 320) w = 320;
    int h = (kRows + 1) * row_h + 8;  // 编码行 + 满页 9 个候选
    int x = caret_on ? cc.x1 - w - 6 : va.x1 + 6;  // 左右仍贴着光标那一列
    if(x < 8) x = (caret_on ? cc.x2 + 6 : va.x1 + 6);
    if(x + w > 1016) x = 1016 - w;
    if(x < 8) x = 8;
    int y = va.y1 + (va.y2 - va.y1 + 1 - h) / 2;
    if(y + h > chrome_bottom()) y = chrome_bottom() - h;
    if(y < 8) y = 8;

    lv_obj_set_style_text_line_space(g_ime_bar, row_h - lh, 0);
    lv_label_set_text(g_ime_bar, s.c_str());
    lv_obj_set_pos(g_ime_bar, x, y);
    lv_obj_set_size(g_ime_bar, w, h);
}

static void update_ime_bar() {
    create_ime_bar();
    if(!g_ime_bar) return;
    // 输入法状态交给状态栏右侧显示,这里只在组字时弹候选条
    if(!g_linux_ime.active() || !g_linux_ime.composing()) {
        lv_obj_add_flag(g_ime_bar, LV_OBJ_FLAG_HIDDEN);
        refresh_ime_status();
        return;
    }
    lv_obj_clear_flag(g_ime_bar, LV_OBJ_FLAG_HIDDEN);

    // 文件管理面板的新建/改名:候选条摆在面板左下的提示行下面。不单独判这一支的话会
    // 走下面的 active_textarea(),拿到背后的编辑器,候选条就飘到编辑器光标那儿去了。
    // 宽度不受面板限制:面板只占屏幕三分之一,挤在里面一页摆不下几个候选。候选条本身
    // 在窗口最上层,伸到面板右边盖住编辑区没关系。
    if(g_file_panel_state.active && g_file_panel_state.prompt) {
        int y = 534;
        if(y + ime_bar_h() > 596) y = 596 - ime_bar_h();
        ime_bar_layout_horizontal(8, y, 656);
        lv_obj_move_foreground(g_ime_bar);
        refresh_ime_status();
        return;
    }

    if(editor_vertical() && g_vt_view && g_vt_caret) {
        ime_bar_layout_vertical();
        lv_obj_move_foreground(g_ime_bar);
        refresh_ime_status();
        return;
    }

    int x = 12;
    int y = 500;
    lv_obj_t *ta = active_textarea();
    if(ta) {
        lv_obj_t *ta_label = lv_textarea_get_label(ta);
        if(ta_label) {
            lv_point_t p {};
            lv_label_get_letter_pos(ta_label, lv_textarea_get_cursor_pos(ta), &p);
            x = lv_obj_get_x(ta) + lv_obj_get_x(ta_label) + p.x;
            const lv_font_t *af = active_text_font(ta);
            int fh = af ? lv_font_get_line_height(af) : g_settings.font_size();
            y = lv_obj_get_y(ta) + lv_obj_get_y(ta_label) + p.y + fh + 14;
            lv_obj_t *parent = lv_obj_get_parent(ta);
            while(parent && parent != g_root) {
                x += lv_obj_get_x(parent);
                y += lv_obj_get_y(parent);
                parent = lv_obj_get_parent(parent);
            }
        }
    }
    if(x < 8) x = 8;
    if(x > 360) x = 360;
    int bh = ime_bar_h();
    if(y + bh > chrome_bottom()) y -= 58;  // 光标太靠下就挪到上一行
    if(y < 8) y = 8;
    int bar_w = std::min(656, 1016 - x);
    if(bar_w < 320) {
        x = std::max(8, 1016 - 656);
        bar_w = std::min(656, 1016 - x);
    }

    ime_bar_layout_horizontal(x, y, bar_w);
    lv_obj_move_foreground(g_ime_bar);
    refresh_ime_status();
}

static void render_main() {
    clear_root();
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    char ym[64];
    strftime(ym, sizeof(ym), "%Y年%m月", &local);
    lv_obj_t *title = label(g_root, ym, 0, 12, 1024, 34);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *line = box(g_root, 390, 54, 244, 2);
    lv_obj_set_style_border_width(line, 0, 0);

    const char *days[7] = {"一", "二", "三", "四", "五", "六", "日"};
    int col_w = 1024 / 7;
    bool month_view = g_settings.home_view() == "month";

    if(month_view) {
        for(int i = 0; i < 7; ++i) {
            lv_obj_t *d = label(g_root, days[i], i * col_w, 70, col_w, 28);
            lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        }

        tm first = local;
        first.tm_mday = 1;
        first.tm_hour = 12;
        time_t first_t = mktime(&first);
        tm first_local {};
        localtime_r(&first_t, &first_local);
        int lead = first_local.tm_wday == 0 ? 6 : first_local.tm_wday - 1;
        tm next = first;
        next.tm_mon += 1;
        time_t next_t = mktime(&next);
        int dim = (int)((next_t - first_t) / 86400);
        for(int day = 1; day <= dim; ++day) {
            int idx = lead + day - 1;
            int row = idx / 7;
            int col = idx % 7;
            time_t day_t = first_t + (day - 1) * 86400;
            tm day_tm {};
            localtime_r(&day_t, &day_tm);
            char ds[16];
            strftime(ds, sizeof(ds), "%Y-%m-%d", &day_tm);
            bool has = g_journal.has_entry(ds);
            bool today = day == local.tm_mday;
            std::string cell = std::to_string(day);
            // 标记位始终占一个字符,否则居中时无标记的日期会比有标记的偏右
            cell += has ? "✓" : (day_t <= now ? "·" : " ");
            lv_obj_t *txt = nullptr;
            if(today) {
                int fx = col * col_w + col_w / 2 - 34;
                int fy = 104 + row * 42;
                lv_obj_t *frame = box(g_root, fx, fy, 68, 32, false);
                lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
                lv_obj_set_style_pad_all(frame, 0, 0);
                txt = label(g_root, cell, fx, fy + 2, 68, 30);
                lv_obj_move_foreground(txt);
            } else {
                txt = label(g_root, cell, col * col_w, 104 + row * 42, col_w, 32);
            }
            lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_CENTER, 0);
        }
    } else {
        // 周视图:本周七天各占一列,名称 / 日期 / 打卡标记
        int since_mon = local.tm_wday == 0 ? 6 : local.tm_wday - 1;
        time_t monday = now - (time_t)since_mon * 86400;
        for(int i = 0; i < 7; ++i) {
            time_t d = monday + i * 86400;
            tm dtm {};
            localtime_r(&d, &dtm);
            char ds[16], md[16];
            strftime(ds, sizeof(ds), "%Y-%m-%d", &dtm);
            strftime(md, sizeof(md), "%m-%d", &dtm);
            bool is_today = i == since_mon;
            bool has = g_journal.has_entry(ds);
            const char *mark = has ? "✓" : (d <= now ? "·" : "");
            int cx = i * col_w;
            if(is_today) {
                lv_obj_t *frame = box(g_root, cx + col_w / 2 - 44, 108, 88, 150, false);
                lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
                lv_obj_set_style_pad_all(frame, 0, 0);
            }
            lv_obj_t *dn = label(g_root, days[i], cx, 116, col_w, 40);
            lv_obj_t *dd = label(g_root, md, cx, 166, col_w, 30);
            lv_obj_t *mk = label(g_root, mark, cx, 206, col_w, 36);
            for(lv_obj_t *o : {dn, dd, mk}) {
                lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_move_foreground(o);
            }
            lv_obj_set_style_text_color(dd, g_theme.muted, 0);
        }
    }

    int total = g_journal.total_entries();
    int today_count = g_journal.count_today();
    lv_obj_t *stats = label(g_root, "连续:" + std::to_string(g_journal.streak()) + "天 总计:" + std::to_string(total) + "篇 今日:" + std::to_string(today_count) + "篇",
                            0, 372, 1024, 32);
    lv_obj_set_style_text_align(stats, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *sep = box(g_root, 40, 414, 944, 2);
    lv_obj_set_style_border_width(sep, 0, 0);
    int visible = total > 0 ? 7 : 6;
    if(g_main_sel >= visible) g_main_sel = visible - 1;
    int slot = 1024 / visible;
    for(int i = 0; i < visible; ++i) {
        int action_idx = (total > 0 || i < 2) ? i : i + 1;
        const Action &a = k_actions[action_idx];
        int cx = i * slot + slot / 2;
        lv_obj_t *icon = box(g_root, cx - 42, 416, 84, 76, i == g_main_sel);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        // 图标字号大,PUA 字形在字宽里靠左、墨迹还可能比字宽宽,得铺满框再按墨迹居中
        lv_obj_t *sym = label(icon, a.symbol, 0, 0, 84);
        if(g_icon_font) lv_obj_set_style_text_font(sym, g_icon_font, 0);
        lv_obj_set_style_text_align(sym, LV_TEXT_ALIGN_CENTER, 0);
        center_ink(sym, a.symbol);
        if(i == g_main_sel) lv_obj_set_style_text_color(sym, g_theme.bg, 0);
    }
    int sel_idx = (total > 0 || g_main_sel < 2) ? g_main_sel : g_main_sel + 1;
    lv_obj_t *hint = label(g_root, std::string("[") + k_actions[sel_idx].key + "] " + k_actions[sel_idx].title,
                           0, 502, 1024, 34);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    draw_status_bar(main_status_text(), true);
}

// 正文和上次保存/载入时是否一致。直接比哈希,不依赖 LVGL 的文本变更事件。
static size_t editor_text_hash() {
    const char *t = g_editor ? lv_textarea_get_text(g_editor) : nullptr;
    return std::hash<std::string>{}(t ? t : "");
}

static void editor_mark_clean() {
    g_editor_clean_hash = editor_text_hash();
}

static bool editor_dirty() {
    return g_editor && editor_text_hash() != g_editor_clean_hash;
}

static void editor_leave() {
    if(g_editor_exit_target == 1) goto_screen(Screen::Outline);
    // 快捷/文件编辑模式下 Esc 是进设置调参数。这里保留 g_quick_slot / g_file_edit_path,
    // 从设置退出时才能回到同一个模式同一个文件(见 settings_leave),所以不能提前清掉。
    else if(g_editor_exit_target == 2) goto_screen(Screen::Settings);
    else if(g_editor_exit_target == 3) {
        g_file_panel_state.active = false;
        goto_screen(Screen::Settings);
    } else goto_screen(Screen::Main);
}

static void editor_exit_target_from_state() {
    if(!g_ol.editPath.empty()) g_editor_exit_target = 1;
    else if(g_quick_slot >= 0) g_editor_exit_target = 2;
    else if(!g_file_edit_path.empty()) g_editor_exit_target = 3;
    else g_editor_exit_target = 0;
}

// 有未保存改动时按返回弹出的询问框
static void draw_editor_exit_confirm() {
    lv_obj_t *dlg = box(g_root, 312, 218, 400, 142, false);
    lv_obj_set_style_bg_color(dlg, g_theme.bg, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_t *t = label(dlg, "有未保存的修改", 0, 6, 392, 30);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *a = label(dlg, "Enter 保存并返回", 0, 40, 392, 28);
    lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *b = label(dlg, "N 不保存返回", 0, 68, 392, 28);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *c = label(dlg, "Esc 取消", 0, 96, 392, 28);
    lv_obj_set_style_text_color(c, g_theme.muted, 0);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_foreground(dlg);
    g_editor_exit_dlg = dlg;
}

// Ctrl+Q 退出 app 的确认框。三种工作模式都用它退出——尤其 file/quick 模式下根本到不了
// 主面板,没有这个键就只能去 kill 进程。
static void draw_quit_confirm() {
    lv_obj_t *dlg = box(g_root, 312, 240, 400, 114, false);
    lv_obj_set_style_bg_color(dlg, g_theme.bg, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_t *t = label(dlg, "退出 pjournal-lvgl?", 0, 6, 392, 30);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *a = label(dlg, "Enter 退出", 0, 40, 392, 28);
    lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *c = label(dlg, "Esc 取消", 0, 68, 392, 28);
    lv_obj_set_style_text_color(c, g_theme.muted, 0);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_foreground(dlg);
    g_quit_dlg = dlg;
}

static void quit_begin() {
    if(g_quit_asking) return;
    g_quit_asking = true;
    draw_quit_confirm();
}

// 退出确认框是模态的:所有按键都进这里。Enter 走人(顺手把编辑器的改动落盘,免得丢字),
// Esc/N 撤掉框子。
static void quit_confirm_key(int key) {
    if(key == '\n' || key == '\r') {
        g_quit_asking = false;
        if(g_screen == Screen::Editor && g_editor && editor_dirty()) save_editor_text();
        g_should_quit = true;
        return;
    }
    if(key == 27 || key == 'n' || key == 'N' || key == 'q' || key == 'Q' || key == 0x11) {
        g_quit_asking = false;
        if(g_quit_dlg) {
            lv_obj_delete(g_quit_dlg);
            g_quit_dlg = nullptr;
        }
    }
}

static void render_editor() {
    clear_root();
    int editor_y = 8;
    int editor_h = 552;
    // 竖排的提示词排在正文右侧的竖列里(见 editor_vt_refresh),不占顶部横条
    if(!editor_vertical() && g_quick_slot < 0 && g_file_edit_path.empty() && g_edit_file.empty() && !g_prompt.empty()) {
        lv_obj_t *prompt = label(g_root, "提示: " + g_prompt, 8, 8, 1008, 58);
        lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(prompt, g_theme.muted, 0);
        editor_y = 72;
        editor_h = 488;
    }
    g_editor = lv_textarea_create(g_root);
    lv_obj_set_pos(g_editor, 8, editor_y);
    lv_obj_set_size(g_editor, 1008, editor_h);
    base_style(g_editor);
    if(g_editor_font) lv_obj_set_style_text_font(g_editor, g_editor_font, 0);
    lv_obj_set_style_text_line_space(g_editor, k_md_line_space, 0);
    lv_obj_set_style_border_width(g_editor, 0, 0);
    lv_obj_set_style_pad_all(g_editor, 0, 0);
    lv_textarea_set_one_line(g_editor, false);
    lv_textarea_set_cursor_click_pos(g_editor, true);
    std::string initial;
    if(!g_file_edit_path.empty()) initial = read_whole_file(g_file_edit_path);
    if(!g_ol.editPath.empty()) initial = outline_read_file(g_ol.editPath);
    if(g_quick_slot >= 0) {
        ensure_dir_path(quick_dir());
        initial = read_whole_file(quick_file(g_quick_slot));
    }
    if(initial.empty() && !g_recovery_checked && g_edit_file.empty() && g_quick_slot < 0 && g_file_edit_path.empty() && g_ol.editPath.empty()) {
        g_recovery_checked = true;
        if(!g_settings.recovery_draft()) {
            g_journal.clear_recovery_draft();
        } else {
            std::string content, meta;
            if(g_journal.load_recovery_draft(content, meta) && !content.empty()) {
                std::string mode = meta_value(meta, "mode");
                if(mode.empty() || mode == "journal") {
                    initial = content;
                    std::string fn = meta_value(meta, "filename");
                    g_edit_file = journal_filename_ok(fn) ? fn : "";
                    g_prompt = meta_value(meta, "prompt");
                    g_recovery_meta = meta;
                }
            }
        }
    }
    if(initial.empty() && !g_edit_file.empty() && g_quick_slot < 0 && g_file_edit_path.empty()) initial = extract_body(g_journal.read_entry(g_edit_file));
    if(!initial.empty()) lv_textarea_set_text(g_editor, initial.c_str());
    lv_obj_add_state(g_editor, LV_STATE_FOCUSED);
    lv_group_focus_obj(g_editor);
    if(editor_vertical()) {
        lv_obj_update_layout(g_editor);
        editor_vt_create(8, editor_y, 1008, editor_h);
        editor_vt_refresh();
    } else if(g_settings.markdown_render()) {
        // 正文宽度要等 textarea 排完版才准,叠加层必须和它对齐才不会错位
        lv_obj_update_layout(g_editor);
        editor_md_create(8, editor_y, 1008, editor_h, lv_obj_get_content_width(g_editor));
        editor_md_refresh();
    } else {
        editor_sel_style_plain();
    }
    create_ime_bar();
    update_ime_bar();
    draw_status_bar(!g_recovery_meta.empty() ? ("已恢复草稿  " + editor_status_text()) : editor_status_text(),
                    false, editor_right_text());
    draw_file_panel();
    // 重新载入的正文就是"干净"的基准,之后的改动才算未保存
    editor_mark_clean();
    if(g_editor_exit_asking) draw_editor_exit_confirm();
    editor_follow_cursor();
}

static int byte_to_char_pos(const std::string &s, size_t byte_pos) {
    if(byte_pos > s.size()) byte_pos = s.size();
    return utf8_char_count(s.substr(0, byte_pos));
}

// ---------------------------------------------------------------------------
// 查找/替换:整篇文本里扫出所有匹配(字节区间),再按索引逐处跳转/替换。
// 查找框与替换框用 Tab 切换焦点。
// ---------------------------------------------------------------------------

static std::string search_field_text(lv_obj_t *f) {
    return f ? lv_textarea_get_text(f) : "";
}

static std::vector<std::pair<int,int>> search_scan(const std::string &text, const std::string &term) {
    std::vector<std::pair<int,int>> out;
    if(term.empty()) return out;
    size_t pos = 0;
    while(pos < text.size()) {
        size_t f = text.find(term, pos);
        if(f == std::string::npos) break;
        out.push_back({(int)f, (int)(f + term.size())});
        pos = f + term.size();
    }
    return out;
}

static int editor_caret_byte() {
    const char *t = g_editor ? lv_textarea_get_text(g_editor) : nullptr;
    return (int)lv_text_encoded_get_byte_id(t ? t : "", lv_textarea_get_cursor_pos(g_editor));
}

// 词或正文变了就重算;匹配表失效后 g_search_cur 归 -1,由下一步按光标重新落点。
static void search_sync(const std::string &text, const std::string &term) {
    if(term == g_search_last_term && text == g_search_last_text) return;
    g_search_last_term = term;
    g_search_last_text = text;
    g_search_matches = search_scan(text, term);
    g_search_cur = -1;
}

static void search_step(bool forward) {
    if(!g_editor) return;
    std::string term = search_field_text(g_search_input);
    if(term.empty()) { set_status("请输入查找内容"); return; }
    std::string text = lv_textarea_get_text(g_editor);
    search_sync(text, term);
    int n = (int)g_search_matches.size();
    if(n == 0) { set_status("未找到"); return; }
    int cur;
    if(g_search_cur < 0) {
        int caret = editor_caret_byte();
        cur = -1;
        if(forward) {
            for(int i = 0; i < n; ++i) if(g_search_matches[i].first >= caret) { cur = i; break; }
            if(cur < 0) cur = 0;
        } else {
            for(int i = n - 1; i >= 0; --i) if(g_search_matches[i].first < caret) { cur = i; break; }
            if(cur < 0) cur = n - 1;
        }
    } else {
        cur = forward ? (g_search_cur + 1) % n : (g_search_cur + n - 1) % n;
    }
    g_search_cur = cur;
    lv_textarea_set_cursor_pos(g_editor, byte_to_char_pos(text, g_search_matches[cur].first));
    set_status("匹配 " + std::to_string(cur + 1) + "/" + std::to_string(n));
    update_ime_bar();
}

static void search_replace_current() {
    if(!g_editor) return;
    std::string term = search_field_text(g_search_input);
    if(term.empty()) { set_status("请输入查找内容"); return; }
    std::string text = lv_textarea_get_text(g_editor);
    auto ms = search_scan(text, term);
    if(ms.empty()) { set_status("未找到"); return; }
    int caret = editor_caret_byte();
    int idx = 0;
    for(int i = 0; i < (int)ms.size(); ++i) if(ms[i].first >= caret) { idx = i; break; }
    std::string rep = search_field_text(g_search_replace);
    std::string nt = text.substr(0, ms[idx].first) + rep + text.substr(ms[idx].second);
    editor_record_undo();
    lv_textarea_set_text(g_editor, nt.c_str());
    lv_textarea_set_cursor_pos(g_editor, byte_to_char_pos(nt, ms[idx].first + (int)rep.size()));
    g_search_last_text.clear();  // 正文已变,下次重算
    set_status("已替换");
    update_ime_bar();
}

static void search_replace_all() {
    if(!g_editor) return;
    std::string term = search_field_text(g_search_input);
    if(term.empty()) { set_status("请输入查找内容"); return; }
    std::string text = lv_textarea_get_text(g_editor);
    auto ms = search_scan(text, term);
    if(ms.empty()) { set_status("未找到匹配"); return; }
    std::string rep = search_field_text(g_search_replace);
    std::string out;
    out.reserve(text.size());
    int pos = 0;
    for(auto &m : ms) {
        out.append(text, pos, m.first - pos);
        out += rep;
        pos = m.second;
    }
    out.append(text, pos, std::string::npos);
    editor_record_undo();
    lv_textarea_set_text(g_editor, out.c_str());
    g_search_last_text.clear();
    set_status("已替换 " + std::to_string((int)ms.size()) + " 处");
    update_ime_bar();
}

static void close_search_panel() {
    if(g_search_panel) lv_obj_delete(g_search_panel);
    g_search_panel = nullptr;
    g_search_input = nullptr;
    g_search_replace = nullptr;
    g_search_focus_rep = false;
    g_search_matches.clear();
    g_search_cur = -1;
    g_search_last_term.clear();
    g_search_last_text.clear();
    if(g_editor) lv_group_focus_obj(g_editor);
}

static void open_search_panel() {
    if(g_search_panel) {
        lv_group_focus_obj(g_search_focus_rep ? g_search_replace : g_search_input);
        return;
    }
    g_search_focus_rep = false;
    g_search_cur = -1;
    g_search_last_term.clear();
    g_search_last_text.clear();
    g_search_panel = box(g_root, 180, 428, 664, 132, false);
    lv_obj_set_style_bg_color(g_search_panel, g_theme.bg, 0);
    lv_obj_set_style_border_width(g_search_panel, 2, 0);
    label(g_search_panel, "查找", 8, 14, 70, 32);
    g_search_input = lv_textarea_create(g_search_panel);
    lv_obj_set_pos(g_search_input, 82, 8);
    lv_obj_set_size(g_search_input, 568, 48);
    base_style(g_search_input);
    lv_textarea_set_one_line(g_search_input, true);
    lv_textarea_set_placeholder_text(g_search_input, "输入后 Enter 下一处");
    label(g_search_panel, "替换", 8, 78, 70, 32);
    g_search_replace = lv_textarea_create(g_search_panel);
    lv_obj_set_pos(g_search_replace, 82, 72);
    lv_obj_set_size(g_search_replace, 568, 48);
    base_style(g_search_replace);
    lv_textarea_set_one_line(g_search_replace, true);
    lv_textarea_set_placeholder_text(g_search_replace, "替换为");
    lv_obj_add_state(g_search_input, LV_STATE_FOCUSED);
    lv_group_focus_obj(g_search_input);
    create_ime_bar();
    lv_obj_move_foreground(g_search_panel);
    update_ime_bar();
    set_status("Tab 切字段 · Ctrl+R 替换 · Ctrl+A 全部替换 · Esc 关闭");
}

static void render_browser() {
    clear_root();
    g_entries = g_journal.list_entries();
    if(g_browser_sel >= (int)g_entries.size()) g_browser_sel = (int)g_entries.size() - 1;
    if(g_browser_sel < 0) g_browser_sel = 0;
    label(g_root, "过往日记", 8, 8, 1008, 32);
    int y = 52;
    int row_h = 52;
    int start = std::max(0, g_browser_sel - 8);
    for(int i = 0; i < 9 && start + i < (int)g_entries.size(); ++i) {
        const auto &e = g_entries[start + i];
        lv_obj_t *r = box(g_root, 8, y + i * row_h, 1008, row_h - 4, start + i == g_browser_sel);
        lv_obj_t *t = label(r, e.date + "  " + e.title + "  " + e.preview, 4, 8, 990, 32);
        if(start + i == g_browser_sel) lv_obj_set_style_text_color(t, g_theme.bg, 0);
    }
    draw_status_bar("过往日记  " + std::to_string(g_entries.size()) + "篇");
}

static void render_viewer() {
    clear_root();
    label(g_root, g_view_file, 8, 8, 1008, 32);
    md_readonly_create(8, 48, 1008, 516);
    md_render_readonly(g_journal.read_entry(g_view_file), 1008, k_md_line_space, &g_viewer_line_y);
    if(g_viewer_scroll >= (int)g_viewer_line_y.size()) g_viewer_scroll = (int)g_viewer_line_y.size() - 1;
    if(g_viewer_scroll < 0) g_viewer_scroll = 0;
    if(!g_viewer_line_y.empty())
        lv_obj_scroll_to_y(g_ro_view, g_viewer_line_y[g_viewer_scroll], LV_ANIM_OFF);
    draw_status_bar("阅读");
}

static void draw_confirm_overlay(const std::string &title, const std::string &action) {
    lv_obj_t *dlg = box(g_root, 312, 218, 400, 142, false);
    lv_obj_set_style_bg_color(dlg, g_theme.bg, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_t *t = label(dlg, title, 0, 24, 392, 34);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *a = label(dlg, action, 0, 64, 392, 30);
    lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *c = label(dlg, "Esc/q取消", 0, 96, 392, 28);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_foreground(dlg);
}

static void render_history() {
    clear_root();
    if(g_history_versions.empty() || !g_history_preview) {
        g_history_versions = g_journal.list_history_versions(g_history_file);
    }
    if(g_history_sel >= (int)g_history_versions.size()) g_history_sel = (int)g_history_versions.size() - 1;
    if(g_history_sel < 0) g_history_sel = 0;

    if(g_history_preview && !g_history_versions.empty()) {
        const auto &v = g_history_versions[g_history_sel];
        label(g_root, "历史 " + history_filename_label(v.filename), 8, 8, 1008, 32);
        md_readonly_create(8, 48, 1008, 516);
        md_render_readonly(g_journal.read_history_version(g_history_file, v.filename), 1008,
                           k_md_line_space, &g_viewer_line_y);
        int idx = g_history_preview_scroll;
        if(idx >= (int)g_viewer_line_y.size()) idx = (int)g_viewer_line_y.size() - 1;
        if(idx < 0) idx = 0;
        if(!g_viewer_line_y.empty())
            lv_obj_scroll_to_y(g_ro_view, g_viewer_line_y[idx], LV_ANIM_OFF);
        draw_status_bar("历史预览");
    } else {
        label(g_root, "历史版本  " + g_history_file, 8, 8, 1008, 32);
        int total = (int)g_history_versions.size();
        int visible = 9;
        if(g_history_sel < g_history_scroll) g_history_scroll = g_history_sel;
        if(g_history_sel >= g_history_scroll + visible) g_history_scroll = g_history_sel - visible + 1;
        if(g_history_scroll < 0) g_history_scroll = 0;
        if(total == 0) {
            lv_obj_t *empty = label(g_root, "暂无历史版本。保存同一篇日记的修改后会自动生成历史。", 0, 250, 1024, 40);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        }
        for(int i = 0; i < visible && g_history_scroll + i < total; ++i) {
            int idx = g_history_scroll + i;
            const auto &v = g_history_versions[idx];
            lv_obj_t *r = box(g_root, 8, 54 + i * 54, 1008, 48, idx == g_history_sel);
            std::string row = history_filename_label(v.filename) + "  " + std::to_string((unsigned)v.size) + "B  " + time_label(v.mtime);
            lv_obj_t *l = label(r, row, 6, 8, 988, 30);
            if(idx == g_history_sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
        }
        draw_status_bar("历史版本");
    }

    if(g_history_confirm_restore) draw_confirm_overlay("恢复此历史版本？", "Enter恢复");
    if(g_history_confirm_delete) draw_confirm_overlay("删除此历史版本？", "Enter删除");
}

static const char *gtd_status_mark(const std::string &s) {
    if(s == "done") return "[x]";
    if(s == "doing") return "[>]";
    if(s == "waiting") return "[~]";
    return "[ ]";
}

static bool gtd_task_selected(int taskIdx) {
    if(g_gtd.multiSel.empty()) return false;
    return g_gtd.multiSel.count(g_gtd.data["tasks"][taskIdx]["id"].asString()) > 0;
}

static bool gtd_has_children(int treePos) {
    return treePos + 1 < (int)g_gtd.treeDepth.size() &&
           g_gtd.treeDepth[treePos + 1] > g_gtd.treeDepth[treePos];
}

static std::string gtd_row_text(int taskIdx, int depth, int treePos, bool drilled) {
    auto &task = g_gtd.data["tasks"][taskIdx];
    std::string row((size_t)(depth > 6 ? 6 : depth) * 2, ' ');
    if(gtd_task_selected(taskIdx)) {
        row += "[*] ";
    } else if(drilled) {
        if(gtd_has_children(treePos))
            row += g_gtd.folded.count(treePos) ? "[+] " : "[-] ";
        else
            row += "    ";
    }
    row += gtd_status_mark(task["status"].asString("todo"));
    row += " ";
    std::string pri = task["priority"].asString("B");
    if(!pri.empty()) row += pri + " ";
    row += task["title"].asString();
    std::string project = task["project"].asString();
    if(!project.empty()) row += "  @" + project;
    std::string ctx = task["context"].asString();
    if(!ctx.empty()) row += "  @" + ctx;
    auto &tags = task["tags"];
    if(tags.isArray())
        for(int i = 0; i < (int)tags.size(); ++i) row += " #" + tags[i].asString();
    std::string due = task["due"].asString();
    if(!due.empty()) row += "  截止 " + due;
    int prog = task["progress"].asInt(0);
    if(prog > 0) row += "  " + std::to_string(prog) + "%";
    return row;
}

static int gtd_visible_rows() {
    switch(g_gtd.mode) {
        case GtdMode::Add:
        case GtdMode::Filter:
        case GtdMode::Rename:
        case GtdMode::AddProject:
        case GtdMode::RenameProject:
        case GtdMode::AddContext:
        case GtdMode::AddTag:
        case GtdMode::RenameContext:
        case GtdMode::RenameTag: return 7;
        default: return 9;
    }
}

static void gtd_center_panel(int w, int h, int &x, int &y) {
    x = (1024 - w) / 2;
    y = (560 - h) / 2;
    if(y < 8) y = 8;
}

static lv_obj_t *gtd_dialog(const std::string &title, int w, int h) {
    int x = 0, y = 0;
    gtd_center_panel(w, h, x, y);
    lv_obj_t *dlg = box(g_root, x, y, w, h, false);
    lv_obj_set_style_bg_color(dlg, g_theme.bg, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_t *t = label(dlg, title, 0, 10, w - 16, 32);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_foreground(dlg);
    return dlg;
}

// 底部文本输入行 + 输入法候选条
static void gtd_text_input(int y, int h, const std::string &hint, const std::string &initial, bool multiline = false) {
    g_gtd.input = lv_textarea_create(g_root);
    lv_obj_set_pos(g_gtd.input, 8, y);
    lv_obj_set_size(g_gtd.input, 1008, h);
    base_style(g_gtd.input);
    if(!multiline) lv_textarea_set_one_line(g_gtd.input, true);
    if(!hint.empty()) lv_textarea_set_placeholder_text(g_gtd.input, hint.c_str());
    if(!initial.empty()) lv_textarea_set_text(g_gtd.input, initial.c_str());
    lv_obj_add_state(g_gtd.input, LV_STATE_FOCUSED);
    lv_group_focus_obj(g_gtd.input);
    create_ime_bar();
    lv_obj_move_foreground(g_ime_bar);
    update_ime_bar();
}

// 文本输入模式的公共按键处理。返回 true 表示已消费。
static bool gtd_input_key(int key) {
    if(!g_gtd.input) return false;
    if(key == KEY_IME_TOGGLE) {
        g_linux_ime.toggle();
        update_ime_bar();
        return true;
    }
    std::string ime_out;
    if(g_linux_ime.handle_key(key, ime_out)) {
        if(!ime_out.empty()) lv_textarea_add_text(g_gtd.input, ime_out.c_str());
        update_ime_bar();
        return true;
    }
    bool multiline = g_gtd.mode == GtdMode::Note;
    if(key == KEY_LEFT) { lv_textarea_cursor_left(g_gtd.input); return true; }
    if(key == KEY_RIGHT) { lv_textarea_cursor_right(g_gtd.input); return true; }
    if(key == KEY_UP) { if(multiline) lv_textarea_cursor_up(g_gtd.input); return true; }
    if(key == KEY_DOWN) { if(multiline) lv_textarea_cursor_down(g_gtd.input); return true; }
    if(key == KEY_HOME) { lv_textarea_set_cursor_pos(g_gtd.input, 0); return true; }
    if(key == KEY_END) { lv_textarea_set_cursor_pos(g_gtd.input, LV_TEXTAREA_CURSOR_LAST); return true; }
    if(key == 8) { lv_textarea_delete_char(g_gtd.input); return true; }
    if(key == 127) { lv_textarea_delete_char_forward(g_gtd.input); return true; }
    if(key == '\n' || key == '\r') {
        if(multiline) { lv_textarea_add_char(g_gtd.input, '\n'); return true; }
        return false;
    }
    if(key >= 32 && key < 127) {
        lv_textarea_add_char(g_gtd.input, (uint32_t)key);
        return true;
    }
    return false;
}

static void render_gtd_tabs() {
    for(int i = 0; i < 5; ++i) {
        lv_obj_t *tab = box(g_root, 8 + i * 202, 8, 196, 38, i == g_gtd.view);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_t *t = label(tab, std::string(i == g_gtd.view ? "● " : "") + k_gtd_views[i], 4, 6, 184, 26);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        if(i == g_gtd.view) lv_obj_set_style_text_color(t, g_theme.bg, 0);
    }
}

static void render_gtd_project_list() {
    label(g_root, "项目", 8, 8, 1008, 32);
    if(g_gtd.projectList.empty()) {
        lv_obj_t *empty = label(g_root, "暂无项目。按 n 新建。", 0, 244, 1024, 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    if(g_gtd.sel >= (int)g_gtd.projectList.size()) g_gtd.sel = (int)g_gtd.projectList.size() - 1;
    if(g_gtd.sel < 0) g_gtd.sel = 0;
    int visible = gtd_visible_rows();
    if(g_gtd.sel < g_gtd.scroll) g_gtd.scroll = g_gtd.sel;
    if(g_gtd.sel >= g_gtd.scroll + visible) g_gtd.scroll = g_gtd.sel - visible + 1;
    if(g_gtd.scroll < 0) g_gtd.scroll = 0;
    auto &tasks = g_gtd.data["tasks"];
    for(int i = 0; i < visible && g_gtd.scroll + i < (int)g_gtd.projectList.size(); ++i) {
        int idx = g_gtd.scroll + i;
        const std::string &name = g_gtd.projectList[idx];
        int total = 0, done = 0;
        for(int t = 0; t < (int)tasks.size(); ++t) {
            if(tasks[t]["project"].asString() != name) continue;
            total++;
            if(tasks[t]["status"].asString("todo") == "done") done++;
        }
        std::string row = name + "   " + std::to_string(done) + "/" + std::to_string(total);
        lv_obj_t *r = box(g_root, 8, 58 + i * 50, 1008, 46, idx == g_gtd.sel);
        lv_obj_t *l = label(r, row, 6, 7, 988, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        if(idx == g_gtd.sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
}

static void render_gtd_list() {
    render_gtd_tabs();
    int visible = gtd_visible_rows();
    if(g_gtd.sel < g_gtd.scroll) g_gtd.scroll = g_gtd.sel;
    if(g_gtd.sel >= g_gtd.scroll + visible) g_gtd.scroll = g_gtd.sel - visible + 1;
    if(g_gtd.scroll < 0) g_gtd.scroll = 0;
    bool drilled = gtd_in_project_drill();
    if(g_gtd.filtered.empty()) {
        lv_obj_t *empty = label(g_root, "暂无任务。按 a 添加。", 0, 244, 1024, 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
    for(int i = 0; i < visible && g_gtd.scroll + i < (int)g_gtd.filtered.size(); ++i) {
        int display_idx = g_gtd.scroll + i;
        int task_idx = g_gtd.filtered[display_idx];
        int depth = display_idx < (int)g_gtd.depth.size() ? g_gtd.depth[display_idx] : 0;
        int treePos = display_idx < (int)g_gtd.visPos.size() ? g_gtd.visPos[display_idx] : 0;
        std::string row = gtd_row_text(task_idx, depth, treePos, drilled);
        lv_obj_t *r = box(g_root, 8, 58 + i * 50, 1008, 46, display_idx == g_gtd.sel);
        lv_obj_t *l = label(r, row, 6, 7, 988, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        if(display_idx == g_gtd.sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
}

static void render_gtd_detail() {
    auto &tasks = g_gtd.data["tasks"];
    if(g_gtd.detailIdx < 0 || g_gtd.detailIdx >= (int)tasks.size()) {
        g_gtd.mode = GtdMode::List;
        render_gtd_list();
        return;
    }
    auto &task = tasks[g_gtd.detailIdx];
    std::string head = std::string(gtd_status_mark(task["status"].asString("todo"))) + " " +
                       task["priority"].asString("B") + "  " + task["title"].asString();
    label(g_root, "详情  " + head, 8, 6, 1008, 34);
    for(int i = 0; i < k_gtd_field_count; ++i) {
        const GtdField &f = k_gtd_fields[i];
        std::string value;
        if(f.type == 'p') {
            std::string p = task["priority"].asString("B");
            value = p;
            for(int k = 0; k < 3; ++k)
                if(std::string(k_gtd_priority[k]) == p) value = k_gtd_priority_disp[k];
        } else if(f.type == 't') {
            std::string s = task["status"].asString("todo");
            value = s;
            for(int k = 0; k < 4; ++k)
                if(std::string(k_gtd_status[k]) == s) value = k_gtd_status_disp[k];
        } else if(f.type == 'c') {
            std::string c = task["context"].asString();
            value = c.empty() ? "(无)" : "@" + c;
        } else if(f.type == 'g') {
            auto &tt = task["tags"];
            if(tt.isArray())
                for(int j = 0; j < (int)tt.size(); ++j)
                    value += (value.empty() ? "" : " ") + std::string("#") + tt[j].asString();
            if(value.empty()) value = "(无)";
        } else if(f.type == 'n') {
            value = std::to_string(task["progress"].asInt(0)) + "%";
        } else if(f.type == 'm') {
            std::string note = task["note"].asString();
            size_t nl = note.find('\n');
            value = nl == std::string::npos ? note : note.substr(0, nl) + " …";
            if(value.empty()) value = "(空)";
        } else {
            value = task[f.key].asString();
            if(value.empty()) value = "(无)";
        }
        lv_obj_t *r = box(g_root, 8, 50 + i * 54, 1008, 50, i == g_gtd.detailField);
        lv_obj_t *a = label(r, f.label, 6, 9, 180, 30);
        lv_obj_t *b = label(r, value, 196, 9, 800, 30);
        lv_label_set_long_mode(b, LV_LABEL_LONG_CLIP);
        if(i == g_gtd.detailField) {
            lv_obj_set_style_text_color(a, g_theme.bg, 0);
            lv_obj_set_style_text_color(b, g_theme.bg, 0);
        }
    }
}

static void render_gtd_picker() {
    int n = (int)g_gtd.pickerOpts.size();
    int h = std::min(430, 70 + n * 42);
    int w = 520;
    lv_obj_t *dlg = gtd_dialog(k_gtd_fields[g_gtd.pickerField].label, w, h);
    int visible = (h - 60) / 42;
    if(g_gtd.pickerSel < g_gtd.pickerScroll) g_gtd.pickerScroll = g_gtd.pickerSel;
    if(g_gtd.pickerSel >= g_gtd.pickerScroll + visible) g_gtd.pickerScroll = g_gtd.pickerSel - visible + 1;
    if(g_gtd.pickerScroll < 0) g_gtd.pickerScroll = 0;
    for(int i = 0; i < visible && g_gtd.pickerScroll + i < n; ++i) {
        int idx = g_gtd.pickerScroll + i;
        bool on = idx == g_gtd.pickerSel;
        bool ticked = g_gtd.pickerToggled.count(idx) > 0;
        lv_obj_t *r = box(dlg, 8, 48 + i * 42, w - 32, 38, on);
        std::string text = g_gtd.pickerOpts[idx].second;
        if(k_gtd_fields[g_gtd.pickerField].type == 'g') text = std::string(ticked ? "[x] " : "[ ] ") + text;
        lv_obj_t *l = label(r, text, 6, 5, w - 52, 28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        if(on) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
}

static int gtd_days_in_month(int year, int month) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if(month < 1 || month > 12) return 30;
    if(month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return d[month - 1];
}

static int gtd_weekday(int year, int month, int day) {
    tm t {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    mktime(&t);
    return t.tm_wday;
}

static void render_gtd_calendar() {
    int w = 620, h = 430;
    lv_obj_t *dlg = gtd_dialog(std::to_string(g_gtd.calYear) + "年" + std::to_string(g_gtd.calMonth) + "月", w, h);
    const char *wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    for(int i = 0; i < 7; ++i) label(dlg, wd[i], 16 + i * 84, 50, 76, 26);
    int days = gtd_days_in_month(g_gtd.calYear, g_gtd.calMonth);
    int first = gtd_weekday(g_gtd.calYear, g_gtd.calMonth, 1);
    for(int d = 1; d <= days; ++d) {
        int cell = first + d - 1;
        int row = cell / 7, col = cell % 7;
        bool on = d == g_gtd.calSelDay;
        lv_obj_t *r = box(dlg, 16 + col * 84, 84 + row * 52, 76, 46, on);
        lv_obj_t *l = label(r, std::to_string(d), 0, 8, 76, 28);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if(on) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
    label(dlg, "方向键选日期 Enter确定 c清除 Esc取消 ←→换月", 8, h - 42, w - 32, 28);
}

static void render_gtd_context_mgr(bool is_ctx) {
    const auto &list = is_ctx ? g_gtd.contextList : g_gtd.tagList;
    int &sel = is_ctx ? g_gtd.ctxSel : g_gtd.tagSel;
    std::string title = is_ctx ? "情境管理" : "标签管理";
    label(g_root, title + "  " + std::to_string(list.size()) + "项", 8, 8, 1008, 32);
    if(list.empty()) {
        lv_obj_t *empty = label(g_root, is_ctx ? "暂无情境。按 a 添加。" : "暂无标签。按 a 添加。", 0, 244, 1024, 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    if(sel >= (int)list.size()) sel = (int)list.size() - 1;
    if(sel < 0) sel = 0;
    int visible = gtd_visible_rows();
    if(sel < g_gtd.scroll) g_gtd.scroll = sel;
    if(sel >= g_gtd.scroll + visible) g_gtd.scroll = sel - visible + 1;
    if(g_gtd.scroll < 0) g_gtd.scroll = 0;
    auto &tasks = g_gtd.data["tasks"];
    for(int i = 0; i < visible && g_gtd.scroll + i < (int)list.size(); ++i) {
        int idx = g_gtd.scroll + i;
        int used = 0;
        for(int t = 0; t < (int)tasks.size(); ++t) {
            if(is_ctx) {
                if(tasks[t]["context"].asString() == list[idx]) used++;
            } else {
                auto &tt = tasks[t]["tags"];
                if(!tt.isArray()) continue;
                for(int j = 0; j < (int)tt.size(); ++j)
                    if(tt[j].asString() == list[idx]) { used++; break; }
            }
        }
        std::string row = (is_ctx ? "@" : "#") + list[idx] + "   " + std::to_string(used) + "项";
        lv_obj_t *r = box(g_root, 8, 58 + i * 50, 1008, 46, idx == sel);
        lv_obj_t *l = label(r, row, 6, 7, 988, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        if(idx == sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
}

static void render_gtd_summary() {
    auto &tasks = g_gtd.data["tasks"];
    int total = (int)tasks.size();
    int byStatus[4] = {0, 0, 0, 0};
    int byPri[3] = {0, 0, 0};
    int overdue = 0;
    std::string today = today_string();
    for(int i = 0; i < (int)tasks.size(); ++i) {
        auto &t = tasks[i];
        std::string s = t["status"].asString("todo");
        for(int k = 0; k < 4; ++k)
            if(s == k_gtd_status[k]) byStatus[k]++;
        std::string p = t["priority"].asString("B");
        for(int k = 0; k < 3; ++k)
            if(p == k_gtd_priority[k]) byPri[k]++;
        std::string due = t["due"].asString();
        if(!due.empty() && s != "done" && due < today) overdue++;
    }
    int w = 560, h = 420;
    lv_obj_t *dlg = gtd_dialog("任务摘要", w, h);
    std::vector<std::string> lines = {
        "总计        " + std::to_string(total),
        "待办        " + std::to_string(byStatus[0]),
        "进行中      " + std::to_string(byStatus[1]),
        "已完成      " + std::to_string(byStatus[2]),
        "等待中      " + std::to_string(byStatus[3]),
        "",
        "优先级 A    " + std::to_string(byPri[0]),
        "优先级 B    " + std::to_string(byPri[1]),
        "优先级 C    " + std::to_string(byPri[2]),
        "",
        "已逾期      " + std::to_string(overdue),
        "项目数      " + std::to_string((int)g_gtd.projectList.size()),
    };
    for(int i = 0; i < (int)lines.size(); ++i) label(dlg, lines[i], 24, 52 + i * 28, w - 48, 26);
}

static void render_gtd_archive() {
    if(g_gtd.archiveBrowsing) {
        label(g_root, "归档 " + g_gtd.archiveViewMonth + "  " +
                          std::to_string((int)g_gtd.archiveTasks.size()) + "项", 8, 8, 1008, 32);
        int visible = 9;
        if(g_gtd.archiveViewSel >= (int)g_gtd.archiveTasks.size())
            g_gtd.archiveViewSel = (int)g_gtd.archiveTasks.size() - 1;
        if(g_gtd.archiveViewSel < 0) g_gtd.archiveViewSel = 0;
        int scroll = std::max(0, g_gtd.archiveViewSel - visible + 1);
        for(int i = 0; i < visible && scroll + i < (int)g_gtd.archiveTasks.size(); ++i) {
            auto &t = g_gtd.archiveTasks[scroll + i];
            std::string row = std::string(gtd_status_mark(t["status"].asString("done"))) + " " +
                              t["priority"].asString("B") + "  " + t["title"].asString();
            std::string completed = t["completed"].asString();
            if(!completed.empty()) row += "  " + completed;
            lv_obj_t *r = box(g_root, 8, 58 + i * 50, 1008, 46, scroll + i == g_gtd.archiveViewSel);
            lv_obj_t *l = label(r, row, 6, 7, 988, 30);
            lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
            if(scroll + i == g_gtd.archiveViewSel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
        }
        return;
    }
    label(g_root, "归档管理  " + std::to_string((int)g_gtd.archiveMonths.size()) + "个月", 8, 8, 1008, 32);
    if(g_gtd.archiveMonths.empty()) {
        lv_obj_t *empty = label(g_root, "暂无归档。完成的任务会在次月自动归档。", 0, 244, 1024, 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    if(g_gtd.archiveSel >= (int)g_gtd.archiveMonths.size()) g_gtd.archiveSel = (int)g_gtd.archiveMonths.size() - 1;
    if(g_gtd.archiveSel < 0) g_gtd.archiveSel = 0;
    for(int i = 0; i < 9 && i < (int)g_gtd.archiveMonths.size(); ++i) {
        std::string row = g_gtd.archiveMonths[i] + "   " +
                          std::to_string(i < (int)g_gtd.archiveCounts.size() ? g_gtd.archiveCounts[i] : 0) + "项";
        lv_obj_t *r = box(g_root, 8, 58 + i * 50, 1008, 46, i == g_gtd.archiveSel);
        lv_obj_t *l = label(r, row, 6, 7, 988, 30);
        if(i == g_gtd.archiveSel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
}

static void render_gtd_help() {
    lv_obj_t *dlg = box(g_root, 200, 30, 624, 510, false);
    lv_obj_set_style_bg_color(dlg, g_theme.bg, 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_move_foreground(dlg);
    label(dlg, "GTD 快捷键", 0, 10, 608, 32);
    int n = (int)(sizeof(k_gtd_help) / sizeof(k_gtd_help[0]));
    int visible = 14;
    if(g_gtd.helpScroll > n - visible) g_gtd.helpScroll = std::max(0, n - visible);
    if(g_gtd.helpScroll < 0) g_gtd.helpScroll = 0;
    for(int i = 0; i < visible && g_gtd.helpScroll + i < n; ++i)
        label(dlg, k_gtd_help[g_gtd.helpScroll + i], 16, 48 + i * 32, 588, 30);
}

// 弹窗类模式下,先画好背后那一层界面
static void render_gtd_underlay(GtdMode prev) {
    switch(prev) {
        case GtdMode::Detail: render_gtd_detail(); break;
        case GtdMode::Archive: render_gtd_archive(); break;
        case GtdMode::ContextMgr:
        case GtdMode::TagMgr: render_gtd_context_mgr(prev == GtdMode::ContextMgr); break;
        case GtdMode::AddProject:
        case GtdMode::RenameProject: render_gtd_project_list(); break;
        default:
            if(g_gtd.view == 3 && g_gtd.projectDrill < 0) render_gtd_project_list();
            else render_gtd_list();
            break;
    }
}

static void render_gtd() {
    clear_root();
    gtd_rebuild();
    if(g_gtd.mode == GtdMode::Picker) {
        render_gtd_underlay(GtdMode::Detail);
        render_gtd_picker();
        draw_status_bar("GTD 选择  空格切换 Enter确定 Esc取消");
        return;
    }
    if(g_gtd.mode == GtdMode::Calendar) {
        render_gtd_underlay(GtdMode::Detail);
        render_gtd_calendar();
        draw_status_bar("GTD 日期选择");
        return;
    }
    if(g_gtd.mode == GtdMode::Summary) {
        render_gtd_underlay(g_gtd.summaryPrev);
        render_gtd_summary();
        draw_status_bar("GTD 摘要  ↑↓滚动 Esc关闭");
        return;
    }
    if(g_gtd.mode == GtdMode::Help) {
        render_gtd_underlay(g_gtd.helpPrev);
        render_gtd_help();
        draw_status_bar("GTD 帮助  ↑↓滚动 Esc关闭");
        return;
    }
    if(g_gtd.mode == GtdMode::Detail || g_gtd.mode == GtdMode::Note) {
        render_gtd_detail();
        if(g_gtd.mode == GtdMode::Note) {
            std::string note = g_gtd.detailIdx >= 0 &&
                                       g_gtd.detailIdx < (int)g_gtd.data["tasks"].size()
                                   ? g_gtd.data["tasks"][g_gtd.detailIdx]["note"].asString()
                                   : "";
            gtd_text_input(48, 480, "备注内容,可多行", note, true);
            draw_status_bar("GTD 备注  Ctrl+Enter保存 Esc取消");
        } else {
            draw_status_bar("GTD 详情  ↑↓字段 Enter编辑 空格切换 Esc返回 ?帮助");
        }
        return;
    }
    if(g_gtd.mode == GtdMode::ContextMgr || g_gtd.mode == GtdMode::TagMgr) {
        render_gtd_context_mgr(g_gtd.mode == GtdMode::ContextMgr);
        draw_status_bar(g_gtd.mode == GtdMode::ContextMgr ? "GTD 情境  a添加 r重命名 d删除 Esc返回"
                                                          : "GTD 标签  a添加 r重命名 d删除 Esc返回");
        return;
    }
    if(g_gtd.mode == GtdMode::Archive) {
        render_gtd_archive();
        draw_status_bar(g_gtd.archiveBrowsing ? "GTD 归档浏览  ↑↓选择 Esc返回"
                                              : "GTD 归档  ↑↓选择 Enter浏览 d删除 Esc返回");
        return;
    }
    if(g_gtd.mode == GtdMode::Add || g_gtd.mode == GtdMode::AddProject ||
       g_gtd.mode == GtdMode::AddContext || g_gtd.mode == GtdMode::AddTag) {
        render_gtd_underlay(g_gtd.mode);
        std::string hint;
        if(g_gtd.mode == GtdMode::AddProject) hint = "新项目名称";
        else if(g_gtd.mode == GtdMode::AddContext) hint = "新情境名称(不含@)";
        else if(g_gtd.mode == GtdMode::AddTag) hint = "新标签名称(不含#)";
        else if(!g_gtd.pendingParent.empty()) hint = "子任务标题";
        else hint = "新任务标题";
        label(g_root, hint + (g_gtd.pendingProject.empty() ? "" : "  (@" + g_gtd.pendingProject + ")"),
              8, 440, 1008, 26);
        gtd_text_input(470, 58, hint, "", false);
        draw_status_bar("GTD 输入  Enter确认 Esc取消 Ctrl+Space输入法");
        return;
    }
    if(g_gtd.mode == GtdMode::Filter) {
        render_gtd_underlay(GtdMode::List);
        std::string cur = g_gtd.filterText;
        if(!g_gtd.filterContext.empty()) cur += " @" + g_gtd.filterContext;
        for(auto &t : g_gtd.filterTags) cur += " #" + t;
        label(g_root, "筛选: " + (cur.empty() ? "(无)" : cur), 8, 440, 1008, 26);
        gtd_text_input(470, 58, "关键词(匹配标题/备注),Enter确认", g_gtd.filterText, false);
        draw_status_bar("GTD 筛选  Enter确认 Esc清除并返回");
        return;
    }
    if(g_gtd.mode == GtdMode::Rename || g_gtd.mode == GtdMode::RenameProject ||
       g_gtd.mode == GtdMode::RenameContext || g_gtd.mode == GtdMode::RenameTag) {
        render_gtd_underlay(g_gtd.mode);
        label(g_root, "重命名: " + g_gtd.renameTarget, 8, 440, 1008, 26);
        gtd_text_input(470, 58, "新名称", g_gtd.renameTarget, false);
        draw_status_bar("GTD 重命名  Enter确认 Esc取消");
        return;
    }
    if(g_gtd.mode == GtdMode::Confirm) {
        render_gtd_underlay(g_gtd.confirmReturn);
        draw_confirm_overlay(g_gtd.confirmTitle, g_gtd.confirmAction);
        draw_status_bar("GTD 确认  Enter执行 Esc取消");
        return;
    }

    if(g_gtd.view == 3 && g_gtd.projectDrill < 0) {
        render_gtd_project_list();
        draw_status_bar("GTD 项目  " + std::to_string((int)g_gtd.projectList.size()) +
                        "个  Enter打开 n新建 r重命名 d删除 Esc返回");
        return;
    }
    render_gtd_list();
    if(!g_gtd.notice.empty()) {
        draw_status_bar(g_gtd.notice);
        g_gtd.notice.clear();
        return;
    }
    std::string left = std::string("GTD ") + k_gtd_views[g_gtd.view];
    if(gtd_in_project_drill()) left += " @" + g_gtd.projectList[g_gtd.projectDrill];
    left += "  " + std::to_string((int)g_gtd.filtered.size()) + "项";
    if(!g_gtd.multiSel.empty()) left += "  已选" + std::to_string((int)g_gtd.multiSel.size());
    if(!g_gtd.filterText.empty() || !g_gtd.filterContext.empty() || !g_gtd.filterTags.empty())
        left += "  [筛选中]";
    draw_status_bar(left);
}

// ── 大纲写作 ─────────────────────────────────────────────────────────────
static const int k_ol_row_y = 58;
static const int k_ol_row_h = 46;
static const int k_ol_row_pitch = 50;

static void outline_scroll_into(int &sel, int &scroll, int total, int visible) {
    if(sel < 0) sel = 0;
    if(total > 0 && sel >= total) sel = total - 1;
    if(sel < scroll) scroll = sel;
    if(sel >= scroll + visible) scroll = sel - visible + 1;
    if(scroll > total - visible) scroll = total - visible;
    if(scroll < 0) scroll = 0;
}

static void outline_row_colors(lv_obj_t *obj, bool selected) {
    lv_obj_set_style_text_color(obj, selected ? g_theme.bg : g_theme.fg, 0);
}

static lv_obj_t *outline_hint(const std::string &text) {
    lv_obj_t *e = label(g_root, text, 0, 260, 1024, 40);
    lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(e, g_theme.muted, 0);
    return e;
}

static lv_obj_t *outline_panel(int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(g_root);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, g_theme.fg, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_move_foreground(o);
    return o;
}

static void render_outline_projects() {
    label(g_root, "大纲项目", 8, 8, 1008, 32);
    int total = (int)g_ol.projects.size();
    outline_scroll_into(g_ol.sel, g_ol.scroll, total, 9);
    if(total == 0) outline_hint("暂无项目 — 按 n 新建");
    for(int i = 0; i < 9 && g_ol.scroll + i < total; ++i) {
        int pi = g_ol.scroll + i;
        bool sel = (pi == g_ol.sel);
        lv_obj_t *r = box(g_root, 8, k_ol_row_y + i * k_ol_row_pitch, 1008, k_ol_row_h, sel);
        lv_obj_t *l = label(r, LV_SYMBOL_DIRECTORY "  " + g_ol.projects[pi], 6, 7, 988, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        outline_row_colors(l, sel);
    }
    draw_status_bar("n:新建 Enter:打开 d:删除 ?:帮助 Esc:返回");
}

// 行尾指示:书签 / 状态 / 备注 / 关联文件
static std::string outline_row_right(JsonValue &node) {
    std::string right;
    if(outline_is_bookmarked(node["id"].asString())) right += "★ ";
    right += k_outline_status_sym[outline_status_index(node["status"].asString("draft"))];
    if(!node["note"].asString().empty()) right += " [M]";
    if(!node["file"].asString().empty()) right += " " LV_SYMBOL_FILE;
    return right;
}

static void render_outline_browse(bool overlay) {
    label(g_root, g_ol.project.empty() ? "大纲" : g_ol.project, 8, 8, 1008, 32);
    outline_rebuild_filter();
    auto &n = outline_nodes();
    int total = (int)g_ol.filtered.size();
    outline_scroll_into(g_ol.sel, g_ol.scroll, total, 9);
    if(total == 0) outline_hint((int)n.size() == 0 ? "空项目 — 按 a 添加标题" : "无匹配标题");
    for(int i = 0; i < 9 && g_ol.scroll + i < total; ++i) {
        int fi = g_ol.scroll + i;
        int ni = g_ol.filtered[fi];
        auto &node = n[ni];
        bool sel = (fi == g_ol.sel);
        std::string prefix = g_ol.filterText.empty()
            ? outline_tree_prefix(ni)
            : std::string((size_t)node["level"].asInt(0) * 2, ' ');
        if(outline_has_children(ni)) prefix += g_ol.folded.count(ni) ? "▸ " : "▾ ";
        lv_obj_t *r = box(g_root, 8, k_ol_row_y + i * k_ol_row_pitch, 1008, k_ol_row_h, sel);
        lv_obj_t *l = label(r, prefix + node["title"].asString(), 6, 7, 738, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_t *rr = label(r, outline_row_right(node), 752, 7, 238, 30);
        lv_obj_set_style_text_align(rr, LV_TEXT_ALIGN_RIGHT, 0);
        outline_row_colors(l, sel);
        outline_row_colors(rr, sel);
    }
    if(overlay) return;
    std::string st;
    if(!g_ol.filterText.empty()) st = "筛选: " + g_ol.filterText + "   Esc 清除";
    else if(!g_ol.filterTags.empty()) {
        st = "标签:";
        for(auto &t : g_ol.filterTags) st += " #" + t;
        st += "   Esc 清除";
    } else st = "?:帮助 a:标题 i:子项 Ent:详情 f:文件 /:筛选 t:标签 b:书签";
    draw_status_bar(st);
}

static std::string outline_field_value(JsonValue &node, int field) {
    switch(field) {
    case 1:
        return k_outline_status_disp[outline_status_index(node["status"].asString("draft"))];
    case 2: {
        std::string s = node["keywords"].asString();
        return s.empty() ? "(空)" : s;
    }
    case 3: {
        std::string s = node["note"].asString();
        if(s.empty()) return "(空)";
        size_t nl = s.find('\n');
        if(nl != std::string::npos) s = s.substr(0, nl) + "…";
        return s;
    }
    case 4: {
        auto &tt = node["tags"];
        if(!tt.isArray() || tt.size() == 0) return "(无)";
        std::string v = "#" + tt[0].asString();
        for(int j = 1; j < (int)tt.size(); ++j) v += " #" + tt[j].asString();
        return v;
    }
    default: {
        std::string s = node["title"].asString();
        return s.empty() ? "(空)" : s;
    }
    }
}

static void render_outline_detail(bool overlay) {
    auto &n = outline_nodes();
    auto &node = n[g_ol.detailIdx];
    label(g_root, node["title"].asString(), 8, 8, 1008, 32);
    static const char *k_fields[] = {"标题", "状态", "关键词", "备注", "标签"};
    for(int i = 0; i < 5; ++i) {
        bool sel = (i == g_ol.detailField);
        lv_obj_t *r = box(g_root, 8, k_ol_row_y + i * k_ol_row_pitch, 1008, k_ol_row_h, sel);
        lv_obj_t *l = label(r, std::string(k_fields[i]) + ": " + outline_field_value(node, i), 6, 7, 988, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        outline_row_colors(l, sel);
    }
    std::string file = node["file"].asString();
    if(!file.empty()) {
        lv_obj_t *l = label(g_root, "文件: " + file, 14, k_ol_row_y + 5 * k_ol_row_pitch + 8, 988, 30);
        lv_obj_set_style_text_color(l, g_theme.muted, 0);
    }
    if(overlay) return;
    draw_status_bar("Enter:编辑 ↑↓:字段 s:摘要 f:关联文件 ?:帮助 Esc:返回");
}

static void render_outline_input(const std::string &title, bool multiline) {
    label(g_root, title, 8, 8, 1008, 32);
    g_ol.input = lv_textarea_create(g_root);
    lv_obj_set_pos(g_ol.input, 8, multiline ? 96 : 240);
    lv_obj_set_size(g_ol.input, 1008, multiline ? 424 : 58);
    base_style(g_ol.input);
    lv_textarea_set_one_line(g_ol.input, !multiline);
    lv_textarea_set_text(g_ol.input, g_ol.editBuf.c_str());
    lv_textarea_set_cursor_pos(g_ol.input, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_state(g_ol.input, LV_STATE_FOCUSED);
    lv_group_focus_obj(g_ol.input);
    create_ime_bar();
    update_ime_bar();
    draw_status_bar(multiline ? "Enter换行 Esc保存返回" : "Enter确定 Esc取消");
}

static void render_outline_summary() {
    render_outline_browse(true);
    auto &n = outline_nodes();
    if(g_ol.summaryIdx < 0 || g_ol.summaryIdx >= (int)n.size()) return;
    auto &node = n[g_ol.summaryIdx];
    lv_obj_t *p = outline_panel(92, 100, 840, 380);
    lv_obj_t *t = label(p, "摘要", 14, 10, 812, 32);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *sl = label(p, "", 14, 46, 812, 2);
    lv_obj_set_style_bg_color(sl, g_theme.fg, 0);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, 0);

    auto &tt = node["tags"];
    std::string tags;
    if(tt.isArray())
        for(int j = 0; j < (int)tt.size(); ++j) {
            if(j) tags += " ";
            tags += "#" + tt[j].asString();
        }
    std::string kw = node["keywords"].asString();
    std::string note = node["note"].asString();
    for(char &c : note)
        if(c == '\n') c = ' ';
    std::string body;
    body += "状态: " + outline_field_value(node, 1) + "\n";
    body += "关键词: " + (kw.empty() ? "(无)" : kw) + "\n";
    body += "标签: " + (tags.empty() ? "(无)" : tags) + "\n";
    body += "备注: " + (note.empty() ? "(无)" : note);
    lv_obj_t *l = label(p, body, 14, 58 - g_ol.summaryScroll * 30, 812, 312);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    draw_status_bar("↑↓滚动 Esc返回");
}

static const char *k_outline_help[] = {
    "── 项目列表 ──",
    "↑↓ 选择",
    "Enter 打开项目",
    "n 新建项目",
    "d 删除项目",
    "Esc 返回",
    "",
    "── 大纲列表 ──",
    "↑↓ 选择标题",
    "a 添加标题",
    "i 添加子标题",
    "r 重命名标题",
    "Enter 详情面板",
    "f 关联文件并编辑",
    "s 摘要",
    "d 删除标题",
    "c 清除文件关联",
    "Tab 切换项目",
    "/ 筛选标题",
    "t 标签管理",
    "b 书签管理",
    "m 加/去书签",
    "z 折叠  Z 全部折叠",
    "j/k 上/下移动",
    "h/l 提升/降低层级",
    "Ctrl+E 导出 Markdown",
    "Esc 返回项目列表",
    "",
    "── 详情面板 ──",
    "↑↓ 选择字段",
    "Enter 编辑字段",
    "s 摘要  f 关联文件",
    "Esc 返回",
    "",
    "── 通用 ──",
    "? 显示帮助",
};

static void render_outline_help() {
    if(g_ol.helpPrev == OutlineMode::Projects) render_outline_projects();
    else if(g_ol.helpPrev == OutlineMode::Detail) render_outline_detail(true);
    else render_outline_browse(true);
    const int count = (int)(sizeof(k_outline_help) / sizeof(k_outline_help[0]));
    const int visible = 12;
    lv_obj_t *p = outline_panel(212, 90, 600, 420);
    lv_obj_t *t = label(p, "快捷键帮助", 14, 10, 572, 32);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    if(g_ol.helpScroll > count - visible) g_ol.helpScroll = count - visible;
    if(g_ol.helpScroll < 0) g_ol.helpScroll = 0;
    for(int i = 0; i < visible && g_ol.helpScroll + i < count; ++i) {
        const char *line = k_outline_help[g_ol.helpScroll + i];
        lv_obj_t *l = label(p, line, 20, 52 + i * 30, 560, 28);
        if((unsigned char)line[0] == 0xE2) lv_obj_set_style_text_color(l, g_theme.muted, 0);
    }
    draw_status_bar("↑↓滚动 Esc返回");
}

static void render_outline_bookmarks() {
    label(g_root, "书签管理", 8, 8, 1008, 32);
    auto &bm = g_ol.data["bookmarks"];
    int total = bm.isArray() ? (int)bm.size() : 0;
    outline_scroll_into(g_ol.bmSel, g_ol.scroll, total, 9);
    if(total == 0) outline_hint("暂无书签 — 在大纲中按 m 添加");
    for(int i = 0; i < 9 && g_ol.scroll + i < total; ++i) {
        int bi = g_ol.scroll + i;
        bool sel = (bi == g_ol.bmSel);
        lv_obj_t *r = box(g_root, 8, k_ol_row_y + i * k_ol_row_pitch, 1008, k_ol_row_h, sel);
        lv_obj_t *l = label(r, bm[bi]["title"].asString(), 6, 7, 988, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        outline_row_colors(l, sel);
    }
    draw_status_bar("Enter:跳转 d:删除 Esc:返回  共 " + std::to_string(total) + " 项");
}

static void render_outline_tagmgr() {
    label(g_root, "标签管理", 8, 8, 1008, 32);
    int total = (int)g_ol.tagList.size();
    outline_scroll_into(g_ol.tagSel, g_ol.scroll, total, 9);
    if(total == 0) outline_hint("暂无标签 — 按 a 添加");
    for(int i = 0; i < 9 && g_ol.scroll + i < total; ++i) {
        int ti = g_ol.scroll + i;
        bool sel = (ti == g_ol.tagSel);
        lv_obj_t *r = box(g_root, 8, k_ol_row_y + i * k_ol_row_pitch, 1008, k_ol_row_h, sel);
        lv_obj_t *l = label(r, "#" + g_ol.tagList[ti], 6, 7, 900, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        outline_row_colors(l, sel);
        bool active = false;
        for(auto &ft : g_ol.filterTags)
            if(ft == g_ol.tagList[ti]) { active = true; break; }
        if(active) {
            lv_obj_t *m = label(r, "●", 928, 7, 60, 30);
            lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_RIGHT, 0);
            outline_row_colors(m, sel);
        }
    }
    draw_status_bar("a:添加 d:删除 r:重命名 Enter:筛选 Esc:返回");
}

static void render_outline_confirm() {
    lv_obj_t *p = outline_panel(212, 218, 600, 164);
    lv_obj_t *m = label(p, g_ol.confirmMsg, 14, 30, 572, 60);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_t *h = label(p, "Enter 确认    Esc 取消", 14, 104, 572, 34);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    draw_status_bar("");
}

static void render_outline_picker() {
    render_outline_detail(true);
    int total = (int)g_ol.pickerVal.size();
    if(total == 0) return;
    const int maxVis = 6;
    int vis = total > maxVis ? maxVis : total;
    int boxH = vis * 40 + 20;
    lv_obj_t *p = outline_panel((1024 - 520) / 2, (600 - boxH) / 2, 520, boxH);
    int scroll = 0;
    if(total > maxVis) {
        scroll = g_ol.pickerSel - maxVis / 2;
        if(scroll < 0) scroll = 0;
        if(scroll + maxVis > total) scroll = total - maxVis;
    }
    bool isTags = (g_ol.pickerField == 4);
    for(int i = 0; i < vis; ++i) {
        int oi = scroll + i;
        bool sel = (oi == g_ol.pickerSel);
        std::string disp = isTags ? (g_ol.pickerToggled.count(oi) ? "[x] " : "[ ] ") : "";
        disp += g_ol.pickerDisp[oi];
        lv_obj_t *r = box(p, 8, 10 + i * 40, 504, 36, sel);
        lv_obj_t *l = label(r, disp, 6, 5, 488, 26);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        outline_row_colors(l, sel);
    }
}

static void render_outline() {
    clear_root();
    outline_normalize();
    if((g_ol.mode == OutlineMode::Detail || g_ol.mode == OutlineMode::Picker) &&
       (g_ol.detailIdx < 0 || g_ol.detailIdx >= (int)outline_nodes().size()))
        g_ol.mode = OutlineMode::Browse;
    switch(g_ol.mode) {
    case OutlineMode::Projects: render_outline_projects(); break;
    case OutlineMode::Browse: render_outline_browse(false); break;
    case OutlineMode::Detail: render_outline_detail(false); break;
    case OutlineMode::AddProject: render_outline_input("新建项目", false); break;
    case OutlineMode::AddHeading: render_outline_input("添加标题", false); break;
    case OutlineMode::AddSub: render_outline_input("添加子标题", false); break;
    case OutlineMode::EditText:
        render_outline_input(g_ol.editingTitle ? "编辑标题" : (g_ol.editingKeyword ? "编辑关键词" : "编辑备注"), false);
        break;
    case OutlineMode::Filter:
        render_outline_browse(true);
        create_ime_bar();
        update_ime_bar();
        draw_status_bar(g_ol.filterText.empty() ? "输入关键词筛选标题"
                                                : ("筛选: " + g_ol.filterText + "   Esc 清除"));
        break;
    case OutlineMode::Summary: render_outline_summary(); break;
    case OutlineMode::Help: render_outline_help(); break;
    case OutlineMode::BookmarkMgr: render_outline_bookmarks(); break;
    case OutlineMode::TagMgr: render_outline_tagmgr(); break;
    case OutlineMode::AddTag: render_outline_input("添加标签", false); break;
    case OutlineMode::RenameTag: render_outline_input("重命名标签", false); break;
    case OutlineMode::Confirm: render_outline_confirm(); break;
    case OutlineMode::Picker: render_outline_picker(); break;
    }
}

struct SetItem {
    std::string key;  // 设置键;文本字段直接对应 g_settings 的键
    std::string label;
    std::string value;
};

// 枚举型设置的候选值 (存值, 显示名)。空表示非选项字段(文本编辑或跳转)。
static std::vector<std::pair<std::string, std::string>> setting_options(const std::string &k) {
    if(k == "theme") return {{"dark", "黑底白字"}, {"light", "白底黑字"}};
    if(k == "app_mode") return {{"journal", "个人日记"}, {"quick", "快捷编辑"}, {"file", "文件编辑"}};
    if(k == "home_view") return {{"week", "周视图"}, {"month", "月视图"}};
    if(k == "input_mode") return {{"builtin", "内置"}, {"fcitx5_rime", "fcitx5+rime"}};
    if(k == "editor_orientation") return {{"horizontal", "横排"}, {"vertical", "竖排"}};
    if(k == "editor_mode")
        return {{"normal", "正常"},
                {"typewriter", "打字机"},
                {"focus", "聚焦"},
                {"typewriter_focus", "打字机聚焦"}};
    if(k == "vertical_ref_line_style")
        return {{"solid", "实线"}, {"dash", "虚线"}, {"dot", "点状虚线"}};
    if(k == "md_render" || k == "first_line_indent" || k == "version_history" ||
       k == "auto_save" || k == "recovery_draft" || k == "vertical_ref_line")
        return {{"1", "开"}, {"0", "关"}};
    if(k == "font_size" || k == "ime_font_size") {
        std::vector<std::pair<std::string, std::string>> o;
        for(int n : {16, 18, 20, 22, 24, 28, 32, 36, 48}) o.push_back({std::to_string(n), std::to_string(n)});
        return o;
    }
    if(k == "font_file") {
        std::vector<std::pair<std::string, std::string>> o;
        for(const auto &f : g_fonts.fonts()) o.push_back({f.path, f.path});
        if(o.empty()) o.push_back({g_fonts.default_font_path(), g_fonts.default_font_path()});
        return o;
    }
    if(k == "font_bold_file" || k == "font_italic_file") {
        // 空值代表不指定真字体,用描两遍/斜切凑出效果
        std::vector<std::pair<std::string, std::string>> o{
            {"", k == "font_bold_file" ? "(伪粗体)" : "(伪斜体)"}};
        for(const auto &f : g_fonts.fonts()) o.push_back({f.path, f.path});
        return o;
    }
    return {};
}

static std::string setting_current_value(const std::string &k) {
    if(k == "theme") return g_settings.theme();
    if(k == "app_mode") return g_settings.app_mode();
    if(k == "home_view") return g_settings.home_view();
    if(k == "font_size") return std::to_string(g_settings.font_size());
    if(k == "ime_font_size") return std::to_string(g_settings.ime_font_size());
    if(k == "font_file") return g_settings.font_file().empty() ? g_fonts.default_font_path() : g_settings.font_file();
    if(k == "font_bold_file") return g_settings.font_bold_file();
    if(k == "font_italic_file") return g_settings.font_italic_file();
    if(k == "input_mode") return g_settings.input_mode();
    if(k == "editor_orientation") return editor_vertical() ? "vertical" : "horizontal";
    if(k == "editor_mode") return g_settings.editor_mode();
    if(k == "vertical_ref_line_style") return g_settings.vertical_reference_line_style();
    if(k == "md_render") return g_settings.markdown_render() ? "1" : "0";
    if(k == "first_line_indent") return g_settings.first_line_indent() ? "1" : "0";
    if(k == "version_history") return g_settings.version_history() ? "1" : "0";
    if(k == "auto_save") return g_settings.auto_save() ? "1" : "0";
    if(k == "recovery_draft") return g_settings.recovery_draft() ? "1" : "0";
    if(k == "vertical_ref_line") return g_settings.vertical_reference_line() ? "1" : "0";
    return "";
}

static void setting_apply(const std::string &k, const std::string &v) {
    g_settings.set(k, v);
    if(k == "font_file" || k == "font_bold_file" || k == "font_italic_file") app_ui_reload_font();
    app_ui_reload_theme();
}

static bool g_setting_pick_active = false;
static int g_setting_pick_sel = 0;
static std::string g_setting_pick_key, g_setting_pick_label;
static std::vector<std::pair<std::string, std::string>> g_setting_pick_opts;

static void open_setting_pick(const SetItem &it) {
    std::vector<std::pair<std::string, std::string>> opts = setting_options(it.key);
    if(opts.empty()) return;
    g_setting_pick_active = true;
    g_setting_pick_key = it.key;
    g_setting_pick_label = it.label;
    g_setting_pick_opts = std::move(opts);
    g_setting_pick_sel = 0;
    std::string cur = setting_current_value(it.key);
    for(size_t i = 0; i < g_setting_pick_opts.size(); ++i)
        if(g_setting_pick_opts[i].first == cur) { g_setting_pick_sel = (int)i; break; }
    render();
}

static std::string vertical_style_label() {
    std::string s = g_settings.vertical_reference_line_style();
    if(s == "dash") return "虚线";
    if(s == "dot") return "点状虚线";
    return "实线";
}

static std::vector<SetItem> settings_items() {
    std::string input = g_settings.input_mode() == "fcitx5_rime" ? "fcitx5+rime" : "内置";
    std::string appModeLabel = "个人日记";
    if(g_settings.app_mode() == "quick") appModeLabel = "快捷编辑";
    else if(g_settings.app_mode() == "file") appModeLabel = "文件编辑";
    IME &ime = IME::getInstance();
    ime.ensureUserDictLoaded();
    std::vector<SetItem> v = {
        {"theme", "主题", g_settings.theme() == "light" ? "白底黑字" : "黑底白字"},
        {"app_mode", "工作模式", appModeLabel},
        {"home_view", "主页视图", g_settings.home_view() == "month" ? "月视图" : "周视图"},
        {"font_size", "字号", std::to_string(g_settings.font_size())},
        {"ime_font_size", "输入法字号", std::to_string(g_settings.ime_font_size())},
        {"font_file", "正文字体", g_settings.font_file().empty() ? g_fonts.default_font_path() : g_settings.font_file()},
        {"font_bold_file", "粗体字体",
         g_settings.font_bold_file().empty() ? "(伪粗体)" : g_settings.font_bold_file()},
        {"font_italic_file", "斜体字体",
         g_settings.font_italic_file().empty() ? "(伪斜体)" : g_settings.font_italic_file()},
        {"journal_dir", "保存位置", g_settings.journal_dir()},
        {"webdav_url", "WebDAV URL", g_settings.get("webdav_url", "")},
        {"webdav_user", "WebDAV 用户", g_settings.get("webdav_user", "")},
        {"webdav_pass", "WebDAV 密码", g_settings.get("webdav_pass", "").empty() ? "" : "******"},
        {"input_mode", "输入法", input},
        {"wifi", "WiFi管理", g_wifi.status().connected ? ("已连接 " + g_wifi.status().ssid) : "未连接"},
        {"md_render", "Markdown渲染", g_settings.markdown_render() ? "开" : "关"},
        {"first_line_indent", "首行缩进", g_settings.first_line_indent() ? "开" : "关"},
        {"version_history", "历史版本", g_settings.version_history() ? "开" : "关"},
        {"auto_save", "自动保存", g_settings.auto_save() ? "开" : "关"},
        {"recovery_draft", "恢复草稿", g_settings.recovery_draft() ? "开" : "关"},
        {"deepseek_key", "Deepseek Key", g_settings.get("deepseek_key", "").empty() ? "" : "******"},
        {"polish_prompt", "润色提示词", g_settings.get("polish_prompt", "").empty() ? "(未设置)" : "(已设置)"},
        {"flomo_email", "Flomo 邮箱", g_settings.get("flomo_email", "")},
        {"flomo_pass", "Flomo 密码", g_settings.get("flomo_pass", "").empty() ? "" : "******"},
        {"flomo_token", "Flomo Token", g_settings.get("flomo_token", "").empty() ? "未生成" : "已生成"},
        {"personal_exp", "个人经历", g_settings.get("personal_exp", "").empty() ? "(未设置)" : "(已设置)"},
        {"personal_hob", "个人爱好", g_settings.get("personal_hob", "").empty() ? "(未设置)" : "(已设置)"},
        {"editor_orientation", "文字方向", editor_vertical() ? "竖排" : "横排"},
    };
    // 竖排专属两项只在竖排时出现,与 ESP32 版一致
    if(editor_vertical()) {
        v.push_back({"vertical_ref_line", "竖排参考线", g_settings.vertical_reference_line() ? "开" : "关"});
        if(g_settings.vertical_reference_line())
            v.push_back({"vertical_ref_line_style", "参考线样式", vertical_style_label()});
    }
    v.push_back({"editor_mode", "编辑模式", editor_mode_label()});
    v.push_back({"dict", "词库管理",
                 "固定 " + std::to_string(ime.userDictSize(IME::FIXED_DICT)) + " / 动态 " +
                     std::to_string(ime.userDictSize(IME::DYNAMIC_DICT))});
    v.push_back({"file_mgr", "文件管理",
                 file_manager_server_running() ? ("运行中 :" + std::to_string(file_manager_server_get_port()))
                                               : "已停止"});
    v.push_back({"file_mgr_token", "文件管理密码", g_settings.get("file_mgr_token", "").empty() ? "" : "******"});
    return v;
}

static void render_setting_pick();

static void render_settings() {
    clear_root();
    label(g_root, "设置", 8, 8, 1008, 32);
    auto items = settings_items();
    if(g_settings_sel >= (int)items.size()) g_settings_sel = (int)items.size() - 1;
    if(g_settings_sel < 0) g_settings_sel = 0;
    int visible = 8;
    if(g_settings_sel < g_settings_scroll) g_settings_scroll = g_settings_sel;
    if(g_settings_sel >= g_settings_scroll + visible) g_settings_scroll = g_settings_sel - visible + 1;
    if(g_settings_scroll < 0) g_settings_scroll = 0;
    for(int i = 0; i < visible && g_settings_scroll + i < (int)items.size(); ++i) {
        int idx = g_settings_scroll + i;
        lv_obj_t *r = box(g_root, 8, 54 + i * 64, 1008, 56, idx == g_settings_sel);
        lv_obj_t *l = label(r, items[idx].label + ": " + items[idx].value, 6, 10, 988, 34);
        if(idx == g_settings_sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
    draw_status_bar("设置  " + std::to_string(g_settings_sel + 1) + "/" + std::to_string((int)items.size()) +
                    "   Enter 选择");
    if(g_setting_pick_active) render_setting_pick();
}

static void render_setting_pick() {
    int total = (int)g_setting_pick_opts.size();
    if(total == 0) return;
    const int maxVis = 8;
    int vis = total > maxVis ? maxVis : total;
    const int boxW = 640;
    int boxH = vis * 44 + 56;
    lv_obj_t *p = box(g_root, (1024 - boxW) / 2, (600 - boxH) / 2, boxW, boxH, false);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_t *t = label(p, g_setting_pick_label, 6, 8, boxW - 12, 30);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    int scroll = 0;
    if(total > maxVis) {
        scroll = g_setting_pick_sel - maxVis / 2;
        if(scroll < 0) scroll = 0;
        if(scroll + maxVis > total) scroll = total - maxVis;
    }
    for(int i = 0; i < vis; ++i) {
        int oi = scroll + i;
        bool sel = oi == g_setting_pick_sel;
        lv_obj_t *r = box(p, 10, 46 + i * 44, boxW - 20, 40, sel);
        lv_obj_t *l = label(r, g_setting_pick_opts[oi].second, 8, 5, boxW - 36, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        if(sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
    lv_obj_move_foreground(p);
}

static void render_setting_text() {
    clear_root();
    label(g_root, g_setting_title, 8, 8, 1008, 32);
    g_setting_text = lv_textarea_create(g_root);
    lv_obj_set_pos(g_setting_text, 8, 72);
    bool multiline = g_setting_key == "polish_prompt" || g_setting_key == "personal_exp" || g_setting_key == "personal_hob";
    lv_obj_set_size(g_setting_text, 1008, multiline ? 300 : 58);
    base_style(g_setting_text);
    lv_textarea_set_one_line(g_setting_text, !multiline);
    lv_textarea_set_cursor_click_pos(g_setting_text, true);
    std::string value = g_setting_key == "journal_dir" ? g_settings.journal_dir() : g_settings.get(g_setting_key, "");
    lv_textarea_set_text(g_setting_text, value.c_str());
    lv_obj_add_state(g_setting_text, LV_STATE_FOCUSED);
    lv_group_focus_obj(g_setting_text);
    create_ime_bar();
    update_ime_bar();
    std::string hint = g_setting_key == "journal_dir" ? "输入完整路径。保存位置会立即用于新的日记列表、保存和历史版本。"
                                                       : "输入内容后按 Enter/Ctrl+S 保存，Esc 取消。";
    label(g_root, hint, 8, multiline ? 392 : 156, 1008, 34);
    draw_status_bar("Enter/Ctrl+S保存  Esc取消");
}

static void render_wifi() {
    clear_root();
    WifiStatus st = g_wifi.status();
    label(g_root, std::string("WiFi管理  ") + (g_wifi_saved_mode ? "已保存" : "扫描") + "  " +
                      (st.connected ? ("已连接 " + st.ssid + " " + st.ip) : st.state),
          8, 8, 1008, 32);
    lv_obj_t *scan_tab = box(g_root, 8, 44, 170, 34, !g_wifi_saved_mode);
    lv_obj_t *scan_txt = label(scan_tab, "扫描网络", 0, 4, 162, 24);
    lv_obj_set_style_text_align(scan_txt, LV_TEXT_ALIGN_CENTER, 0);
    if(!g_wifi_saved_mode) lv_obj_set_style_text_color(scan_txt, g_theme.bg, 0);
    lv_obj_t *saved_tab = box(g_root, 186, 44, 170, 34, g_wifi_saved_mode);
    lv_obj_t *saved_txt = label(saved_tab, "已保存", 0, 4, 162, 24);
    lv_obj_set_style_text_align(saved_txt, LV_TEXT_ALIGN_CENTER, 0);
    if(g_wifi_saved_mode) lv_obj_set_style_text_color(saved_txt, g_theme.bg, 0);

    if(g_wifi_saved_mode) {
        if(g_wifi_saved_entries.empty()) g_wifi_saved_entries = g_wifi.list_networks();
        if(g_wifi_saved_sel >= (int)g_wifi_saved_entries.size()) g_wifi_saved_sel = (int)g_wifi_saved_entries.size() - 1;
        if(g_wifi_saved_sel < 0) g_wifi_saved_sel = 0;
        int start = std::max(0, g_wifi_saved_sel - 6);
        for(int i = 0; i < 7 && start + i < (int)g_wifi_saved_entries.size(); ++i) {
            int idx = start + i;
            const auto &n = g_wifi_saved_entries[idx];
            lv_obj_t *r = box(g_root, 8, 88 + i * 50, 1008, 46, idx == g_wifi_saved_sel);
            lv_obj_t *l = label(r, n.id + "  " + n.ssid + "  " + n.flags, 6, 7, 988, 30);
            if(idx == g_wifi_saved_sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
        }
    } else {
        if(g_wifi_entries.empty()) g_wifi_entries = g_wifi.scan();
        if(g_wifi_sel >= (int)g_wifi_entries.size()) g_wifi_sel = (int)g_wifi_entries.size() - 1;
        if(g_wifi_sel < 0) g_wifi_sel = 0;
        int start = std::max(0, g_wifi_sel - 6);
        for(int i = 0; i < 7 && start + i < (int)g_wifi_entries.size(); ++i) {
            int idx = start + i;
            const auto &n = g_wifi_entries[idx];
            lv_obj_t *r = box(g_root, 8, 88 + i * 50, 1008, 46, idx == g_wifi_sel);
            lv_obj_t *l = label(r, n.ssid + "  " + n.signal + "dBm  " + n.flags, 6, 7, 988, 30);
            if(idx == g_wifi_sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
        }
    }
    g_wifi_password = lv_textarea_create(g_root);
    lv_obj_set_pos(g_wifi_password, 8, 452);
    lv_obj_set_size(g_wifi_password, 1008, 54);
    base_style(g_wifi_password);
    lv_textarea_set_one_line(g_wifi_password, true);
    lv_textarea_set_password_mode(g_wifi_password, true);
    lv_textarea_set_placeholder_text(g_wifi_password, g_wifi_saved_mode ? "已保存网络: Enter选择 d删除 r刷新 x断开 c重连" : "扫描网络: 输入密码 Enter连接 Tab切换 r刷新");
    draw_status_bar(st.connected ? ("WiFi  " + st.ssid) : "WiFi");
}

static std::string dict_trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if(start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static const char *dict_kind_label(int kind) {
    return kind == IME::FIXED_DICT ? "固定词库" : "动态词库";
}

static void dict_reload() {
    IME &ime = IME::getInstance();
    ime.ensureUserDictLoaded();
    g_dict_entries = ime.userDictEntries((IME::UserDictKind)g_dict_kind);
    std::string q = dict_trim(g_dict_query);
    g_dict_filtered.clear();
    for(int i = 0; i < (int)g_dict_entries.size(); ++i) {
        if(q.empty() || g_dict_entries[i].code.find(q) != std::string::npos ||
           g_dict_entries[i].word.find(q) != std::string::npos)
            g_dict_filtered.push_back(i);
    }
    if(g_dict_sel >= (int)g_dict_filtered.size()) g_dict_sel = (int)g_dict_filtered.size() - 1;
    if(g_dict_sel < 0) g_dict_sel = 0;
}

static void render_dict() {
    clear_root();
    if(g_dict_mode == DictMode::Choose) {
        label(g_root, "词库管理", 8, 8, 1008, 32);
        lv_obj_t *r0 = box(g_root, 8, 72, 1008, 56, g_dict_sel == 0);
        lv_obj_t *l0 = label(r0, "固定词库", 6, 10, 988, 34);
        if(g_dict_sel == 0) lv_obj_set_style_text_color(l0, g_theme.bg, 0);
        lv_obj_t *r1 = box(g_root, 8, 136, 1008, 56, g_dict_sel == 1);
        lv_obj_t *l1 = label(r1, "动态词库", 6, 10, 988, 34);
        if(g_dict_sel == 1) lv_obj_set_style_text_color(l1, g_theme.bg, 0);
        label(g_root, "固定词库长期保留；动态词库由输入法自动积累，超限后淘汰旧词。", 8, 220, 1008, 68);
        draw_status_bar("↑↓选择  Enter进入  Esc返回");
        return;
    }

    if(g_dict_mode == DictMode::Add) {
        label(g_root, std::string("添加") + dict_kind_label(g_dict_kind), 8, 8, 1008, 32);
        g_dict_text = lv_textarea_create(g_root);
        lv_obj_set_pos(g_dict_text, 8, 72);
        lv_obj_set_size(g_dict_text, 1008, 58);
        base_style(g_dict_text);
        lv_textarea_set_one_line(g_dict_text, true);
        lv_textarea_set_cursor_click_pos(g_dict_text, true);
        lv_obj_add_state(g_dict_text, LV_STATE_FOCUSED);
        lv_group_focus_obj(g_dict_text);
        create_ime_bar();
        update_ime_bar();
        label(g_root, "格式: 编码 空格 候选词，例如 aaa 好", 8, 156, 1008, 34);
        draw_status_bar("Enter确定  Esc取消");
        return;
    }

    if(g_dict_mode == DictMode::Search) {
        label(g_root, "检索词条", 8, 8, 1008, 32);
        g_dict_text = lv_textarea_create(g_root);
        lv_obj_set_pos(g_dict_text, 8, 72);
        lv_obj_set_size(g_dict_text, 1008, 58);
        base_style(g_dict_text);
        lv_textarea_set_one_line(g_dict_text, true);
        lv_textarea_set_cursor_click_pos(g_dict_text, true);
        lv_textarea_set_text(g_dict_text, g_dict_query.c_str());
        lv_obj_add_state(g_dict_text, LV_STATE_FOCUSED);
        lv_group_focus_obj(g_dict_text);
        create_ime_bar();
        update_ime_bar();
        label(g_root, "输入编码或候选词，Enter 应用过滤；清空后显示全部。", 8, 156, 1008, 34);
        draw_status_bar("Enter搜索  Esc取消");
        return;
    }

    std::string title = std::string(dict_kind_label(g_dict_kind)) + " " + std::to_string(g_dict_entries.size()) + "/" +
                        (g_dict_kind == IME::FIXED_DICT ? "500" : "1000");
    if(!g_dict_query.empty()) title += "  检索: " + g_dict_query;
    label(g_root, title, 8, 8, 1008, 32);
    label(g_root, "编码", 18, 44, 180, 28);
    label(g_root, "候选词", 232, 44, 620, 28);
    label(g_root, "频次", 872, 44, 120, 28);

    int visible = 9;
    if(g_dict_sel < g_dict_scroll) g_dict_scroll = g_dict_sel;
    if(g_dict_sel >= g_dict_scroll + visible) g_dict_scroll = g_dict_sel - visible + 1;
    if(g_dict_scroll < 0) g_dict_scroll = 0;
    if(g_dict_filtered.empty()) {
        lv_obj_t *empty = label(g_root, g_dict_entries.empty() ? "暂无词条。按 a 添加。" : "无匹配词条。",
                                0, 252, 1024, 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
    for(int i = 0; i < visible && g_dict_scroll + i < (int)g_dict_filtered.size(); ++i) {
        int view = g_dict_scroll + i;
        int idx = g_dict_filtered[view];
        const IME::UserEntryView &e = g_dict_entries[idx];
        bool selected = view == g_dict_sel;
        lv_obj_t *r = box(g_root, 8, 78 + i * 50, 1008, 46, selected);
        lv_obj_t *lc = label(r, (g_dict_selected.count(idx) ? "* " : "  ") + e.code, 6, 7, 208, 30);
        lv_obj_t *lw = label(r, e.word, 220, 7, 620, 30);
        lv_obj_t *ln = label(r, std::to_string(e.count), 860, 7, 130, 30);
        if(selected) {
            lv_obj_set_style_text_color(lc, g_theme.bg, 0);
            lv_obj_set_style_text_color(lw, g_theme.bg, 0);
            lv_obj_set_style_text_color(ln, g_theme.bg, 0);
        }
    }
    draw_status_bar("a添加 d删除 /检索 已选" + std::to_string(g_dict_selected.size()) +
                    "  Space多选  Tab切换  q返回");
}

static void render_filemgr() {
    clear_root();
    label(g_root, "文件管理", 8, 8, 1008, 32);
    if(!file_manager_server_running()) {
        lv_obj_t *m = label(g_root, "服务未启动。端口 8080 可能已被占用。", 0, 252, 1024, 40);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        draw_status_bar("q返回");
        return;
    }
    WifiStatus st = g_wifi.status();
    label(g_root, "目录: " + g_settings.journal_dir(), 8, 72, 1008, 34);
    label(g_root, "WiFi: " + (st.connected ? st.ssid : std::string("未连接")), 8, 116, 1008, 34);
    std::string ip = st.ip.empty() ? std::string("设备IP") : st.ip;
    std::string url = "地址: http://" + ip + ":" + std::to_string(file_manager_server_get_port());
    label(g_root, url, 8, 160, 1008, 34);
    label(g_root, "在手机或电脑浏览器打开上面的地址，即可浏览、上传、下载日记文件。", 8, 220, 1008, 68);
    if(!g_settings.get("file_mgr_token", "").empty())
        label(g_root, "已启用访问密码: 网页需勾选密码并填入相同内容。", 8, 292, 1008, 34);
    draw_status_bar("q返回并停止服务");
}

static void render_inspiration() {
    clear_root();
    inspiration_rebuild();
    if(g_inspiration_mode == InspirationMode::EditContent) {
        std::string title = g_inspiration_edit_idx >= 0 ? "编辑灵感" : "添加灵感";
        label(g_root, title, 8, 8, 1008, 32);
        g_inspiration_text = lv_textarea_create(g_root);
        lv_obj_set_pos(g_inspiration_text, 8, 54);
        lv_obj_set_size(g_inspiration_text, 1008, 476);
        base_style(g_inspiration_text);
        lv_textarea_set_one_line(g_inspiration_text, false);
        lv_textarea_set_cursor_click_pos(g_inspiration_text, true);
        if(g_inspiration_edit_idx >= 0) {
            lv_textarea_set_text(g_inspiration_text, g_inspiration_data["items"][g_inspiration_edit_idx]["content"].asString().c_str());
        }
        lv_obj_add_state(g_inspiration_text, LV_STATE_FOCUSED);
        lv_group_focus_obj(g_inspiration_text);
        create_ime_bar();
        update_ime_bar();
        draw_status_bar("Ctrl+S保存  Esc取消  Enter换行");
        return;
    }

    if(g_inspiration_mode == InspirationMode::EditKeywords || g_inspiration_mode == InspirationMode::Search) {
        bool keyword = g_inspiration_mode == InspirationMode::EditKeywords;
        label(g_root, keyword ? "编辑关键词" : "检索灵感", 8, 8, 1008, 32);
        g_inspiration_text = lv_textarea_create(g_root);
        lv_obj_set_pos(g_inspiration_text, 8, 72);
        lv_obj_set_size(g_inspiration_text, 1008, 58);
        base_style(g_inspiration_text);
        lv_textarea_set_one_line(g_inspiration_text, true);
        lv_textarea_set_cursor_click_pos(g_inspiration_text, true);
        if(keyword && g_inspiration_edit_idx >= 0) {
            lv_textarea_set_text(g_inspiration_text, g_inspiration_data["items"][g_inspiration_edit_idx]["keywords"].asString().c_str());
        } else if(!keyword) {
            lv_textarea_set_text(g_inspiration_text, g_inspiration_query.c_str());
        }
        lv_obj_add_state(g_inspiration_text, LV_STATE_FOCUSED);
        lv_group_focus_obj(g_inspiration_text);
        create_ime_bar();
        update_ime_bar();
        label(g_root, keyword ? "多个关键词可用空格分隔。" : "输入内容或关键词，Enter 应用过滤；清空后显示全部。", 8, 156, 1008, 34);
        draw_status_bar("Enter保存/搜索  Esc取消");
        return;
    }

    label(g_root, "灵感面板", 8, 8, 1008, 32);
    std::string sub = g_inspiration_query.empty() ? "全部" : ("检索: " + g_inspiration_query);
    label(g_root, sub, 8, 40, 1008, 28);
    int visible = 9;
    if(g_inspiration_sel < g_inspiration_scroll) g_inspiration_scroll = g_inspiration_sel;
    if(g_inspiration_sel >= g_inspiration_scroll + visible) g_inspiration_scroll = g_inspiration_sel - visible + 1;
    if(g_inspiration_scroll < 0) g_inspiration_scroll = 0;
    auto &items = g_inspiration_data["items"];
    if(g_inspiration_filtered.empty()) {
        lv_obj_t *empty = label(g_root, "暂无灵感。按 a 添加，/ 检索。", 0, 252, 1024, 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
    for(int i = 0; i < visible && g_inspiration_scroll + i < (int)g_inspiration_filtered.size(); ++i) {
        int display_idx = g_inspiration_scroll + i;
        int item_idx = g_inspiration_filtered[display_idx];
        auto &item = items[item_idx];
        std::string content = utf8_preview(item["content"].asString(), 48);
        if(content.empty()) content = "(空内容)";
        std::string kw = item["keywords"].asString();
        std::string row = content;
        if(!kw.empty()) row += "  [" + kw + "]";
        lv_obj_t *r = box(g_root, 8, 78 + i * 50, 1008, 46, display_idx == g_inspiration_sel);
        lv_obj_t *l = label(r, row, 6, 7, 988, 30);
        if(display_idx == g_inspiration_sel) lv_obj_set_style_text_color(l, g_theme.bg, 0);
    }
    draw_status_bar("灵感  a添加 Enter编辑 k关键词 /检索 c复制 d删除 q返回");
}

static void render_sync() {
    clear_root();
    label(g_root, "WebDAV 同步", 8, 8, 1008, 34);
    label(g_root, "远端: " + g_settings.get("webdav_url", ""), 8, 62, 1008, 34);
    label(g_root, "本地: " + g_settings.journal_dir(), 8, 106, 1008, 34);
    std::string msg = g_sync_message.empty() ? "按 Enter 开始同步。请先在设置中配置 WebDAV URL、用户和密码。" : g_sync_message;
    lv_obj_t *m = label(g_root, msg, 8, 180, 1008, 160);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    draw_status_bar(g_sync_done ? "同步完成" : "WebDAV");
}

static void render_help() {
    clear_root();
    static const char *help[] = {
        "主界面: p提示写作 f自由写作 v查看 w同步WebDAV t任务 o大纲 s设置 Ctrl+Q退出",
        "通用: hjkl/方向键移动 Enter确认 Esc/q返回 Ctrl+I灵感",
        "编辑: Ctrl+S保存 Ctrl+Q退出 Ctrl+A全选 Ctrl+C复制 Ctrl+X剪切 Ctrl+V粘贴",
        "编辑: Ctrl+Z撤销 Ctrl+R重做 Ctrl+Y历史 Ctrl+/查找替换 Ctrl+?帮助",
        "查找: Enter下一处 Ctrl+Enter上一处 Tab切查找/替换 Ctrl+R替换 Ctrl+A全部替换 Esc关闭",
        "编辑: Ctrl+P生成提示(仅日记模式) Ctrl+O润色(有选区只润色选区) Ctrl+F发送Flomo",
        "润色: Shift+方向键/Ctrl+A选文本 Enter应用 R改指令重润色 Esc取消",
        "编辑: Ctrl+T折叠/展开光标所在的标题(Markdown渲染开启时)",
        "快捷编辑: F1-F10切换文件 Ctrl+n/p下/上一个 Esc保存并回设置",
        "文件编辑: Ctrl+E文件面板 a新建 d删除 r改名 Enter编辑",
        "输入法: Ctrl+Space开关 候选数字直选 空格首选 方向键/无变换/变换/标点翻页",
        "浏览: Enter查看 e编辑 h历史 d删除 Ctrl+F发送Flomo",
        "GTD: 1-5/←→切换视图 a加任务 i子任务 Enter详情 空格切状态 d删除 r重命名",
        "GTD: j/k上移下移 h/l提降层级 z/Z折叠 /筛选 c/t情境标签 s摘要 A归档 E导出 n新项目 ?帮助",
        "大纲: 项目列表a新建 Enter打开；项目内a同级 i子级 Enter状态 d删除 ,/.调整层级",
        "WiFi: Tab扫描/已保存 r刷新 Enter连接/选择 d删保存 x断开 c重连",
        "灵感: Ctrl+I进入 a添加 Enter编辑 k关键词 /检索 c复制 d删除 q返回",
        "词库: 设置→词库管理 a添加 d删除 /检索 Space多选 Tab切换词库",
        "设置: 主题/字号/字体/保存位置/WebDAV/输入法/WiFi/Markdown",
        "Linux版: 蓝牙/语音识别按要求移除，无声卡故打字音移除；TTF/OTF从/root/.fonts加载。",
        "UTF-8: LVGL文本和文件均为UTF-8。",
    };
    label(g_root, "快捷键帮助", 8, 8, 1008, 32);
    for(size_t i = 0; i < sizeof(help) / sizeof(help[0]); ++i) label(g_root, help[i], 16, 50 + i * 26, 992, 25);
    draw_status_bar("帮助");
}

static void render() {
    switch(g_screen) {
    case Screen::Main: render_main(); break;
    case Screen::Editor: render_editor(); break;
    case Screen::Browser: render_browser(); break;
    case Screen::Viewer: render_viewer(); break;
    case Screen::History: render_history(); break;
    case Screen::Gtd: render_gtd(); break;
    case Screen::Outline: render_outline(); break;
    case Screen::Sync: render_sync(); break;
    case Screen::SettingText: render_setting_text(); break;
    case Screen::Settings: render_settings(); break;
    case Screen::Wifi: render_wifi(); break;
    case Screen::Inspiration: render_inspiration(); break;
    case Screen::Dict: render_dict(); break;
    case Screen::FileMgr: render_filemgr(); break;
    case Screen::Help: render_help(); break;
    }
    // 退出框不属于任何一屏的渲染函数,clear_root 会把它抹掉,所以在这里补画一次。
    if(g_quit_asking) draw_quit_confirm();
}

static void goto_screen(Screen s) {
    // 只有从「设置外面」进来才更新返回目标;设置自己的子界面退回来时保持不动
    if(s == Screen::Settings && g_screen != Screen::SettingText && g_screen != Screen::Wifi &&
       g_screen != Screen::Dict && g_screen != Screen::FileMgr)
        g_settings_return = g_screen;
    g_prev_screen = g_screen;
    // 离开编辑器时把「大纲正文文件」的路径收回来(进入时保留,供编辑器读取)。
    bool back_from_outline_file = false;
    if(g_screen == Screen::Editor && s != Screen::Editor) {
        back_from_outline_file = !g_ol.editPath.empty();
        g_ol.editPath.clear();
    }
    g_screen = s;
    if(s != Screen::Settings) g_setting_pick_active = false;
    if(s == Screen::Editor) {
        g_recovery_checked = false;
        g_recovery_meta.clear();
        g_last_recovery_hash = 0;
        g_next_recovery_ms = 0;
        g_undo_stack.clear();
        g_redo_stack.clear();
        g_md_folded.clear();
    }
    if(s == Screen::Gtd) {
        gtd_load();
        gtd_auto_archive();
        gtd_rebuild();
    }
    if(s == Screen::Outline) {
        outline_list_projects();
        if(!back_from_outline_file) g_ol.project.clear();
        g_ol.mode = g_ol.project.empty() ? OutlineMode::Projects : OutlineMode::Browse;
        g_ol.filterText.clear();
        g_ol.filterTags.clear();
        g_ol.folded.clear();
        g_ol.editBuf.clear();
        g_ol.insertAfter = -1;
        if(g_ol.project.empty()) {
            g_ol.data = JsonValue();
            outline_normalize();
            g_ol.filtered.clear();
            g_ol.sel = 0;
        } else {
            outline_load_project(g_ol.project);
            outline_rebuild_filter();
        }
        g_ol.scroll = 0;
    }
    if(s == Screen::Sync) {
        g_sync_message.clear();
        g_sync_done = false;
    }
    if(s == Screen::Inspiration) {
        g_inspiration_mode = InspirationMode::List;
        inspiration_load();
    }
    if(s == Screen::Dict) {
        g_dict_mode = DictMode::Choose;
        g_dict_kind = IME::FIXED_DICT;
        g_dict_sel = 0;
        g_dict_scroll = 0;
        g_dict_selected.clear();
        g_dict_query.clear();
        dict_reload();
    }
    if(s == Screen::FileMgr) file_manager_server_start(8080);
    render();
}

static void save_editor_text() {
    if(!g_editor) return;
    std::string body = lv_textarea_get_text(g_editor);
    if(body.empty()) {
        if(!g_file_edit_path.empty()) {
            bool ok = safe_write_file(g_file_edit_path, "");
            if(ok) {
                editor_mark_clean();
                file_edit_set_last(g_file_edit_path.substr(g_file_edit_path.find_last_of('/') + 1));
            }
            set_status(ok ? "已保存文件" : "保存文件失败");
            return;
        }
        if(!g_ol.editPath.empty()) {
            bool ok = safe_write_file(g_ol.editPath, "");
            if(ok) editor_mark_clean();
            set_status(ok ? "已保存" : "保存失败");
            return;
        }
        if(g_quick_slot < 0) g_journal.clear_recovery_draft();
        set_status("内容为空，未保存");
        return;
    }
    if(!utf8_valid(body)) {
        set_status("UTF-8无效，未保存");
        return;
    }
    if(g_quick_slot >= 0) {
        bool ok = ensure_dir_path(quick_dir()) && safe_write_file(quick_file(g_quick_slot), body);
        if(ok) editor_mark_clean();
        set_status(ok ? "已保存快捷文件" : "保存快捷文件失败");
        return;
    }
    if(!g_file_edit_path.empty()) {
        bool ok = safe_write_file(g_file_edit_path, body);
        if(ok) {
            editor_mark_clean();
            file_edit_set_last(g_file_edit_path.substr(g_file_edit_path.find_last_of('/') + 1));
        }
        set_status(ok ? "已保存文件" : "保存文件失败");
        return;
    }
    if(!g_ol.editPath.empty()) {
        bool ok = safe_write_file(g_ol.editPath, body);
        if(ok) editor_mark_clean();
        set_status(ok ? "已保存" : "保存失败");
        return;
    }
    std::string content = make_journal_text(g_prompt, body);
    bool ok = g_edit_file.empty() ? g_journal.save_entry(content) : g_journal.save_entry_raw(g_edit_file, content, true);
    if(ok) {
        g_journal.clear_recovery_draft();
        g_last_recovery_hash = 0;
        g_recovery_meta.clear();
        editor_mark_clean();
    }
    set_status(ok ? "已保存" : "保存失败");
}

static bool save_editor_text_for_history() {
    if(!g_editor) return false;
    std::string body = lv_textarea_get_text(g_editor);
    if(body.empty()) return false;
    if(!utf8_valid(body)) return false;
    if(g_quick_slot >= 0) return ensure_dir_path(quick_dir()) && safe_write_file(quick_file(g_quick_slot), body);
    if(!g_file_edit_path.empty()) { safe_write_file(g_file_edit_path, body); return false; }
    if(!g_ol.editPath.empty()) { safe_write_file(g_ol.editPath, body); return false; }
    std::string content = make_journal_text(g_prompt, body);
    if(!g_edit_file.empty()) {
        bool ok = g_journal.save_entry_raw(g_edit_file, content, true);
        if(ok) g_journal.clear_recovery_draft();
        return ok;
    }
    if(!g_journal.save_entry(content)) return false;
    g_journal.clear_recovery_draft();
    g_entries = g_journal.list_entries();
    if(g_entries.empty()) return false;
    g_edit_file = g_entries.front().filename;
    return true;
}

static void open_history(const std::string &filename, Screen return_to) {
    g_history_file = filename;
    g_history_return = return_to;
    g_history_versions = g_journal.list_history_versions(filename);
    g_history_sel = 0;
    g_history_scroll = 0;
    g_history_preview_scroll = 0;
    g_history_preview = false;
    g_history_confirm_restore = false;
    g_history_confirm_delete = false;
    goto_screen(Screen::History);
}

static void open_setting_text(const std::string &key, const std::string &title) {
    g_setting_key = key;
    g_setting_title = title;
    goto_screen(Screen::SettingText);
}

static void open_quick_editor(int slot) {
    if(slot < 0) slot = 0;
    if(slot > 9) slot = 9;
    if(g_screen == Screen::Editor && g_quick_slot >= 0) save_editor_text();
    g_quick_slot = slot;
    g_file_edit_path.clear();
    g_prompt.clear();
    g_edit_file.clear();
    goto_screen(Screen::Editor);
}

static void open_file_editor(const std::string &name) {
    if(g_screen == Screen::Editor && !g_file_edit_path.empty()) save_editor_text();
    file_edit_ensure_default();
    std::string clean = file_edit_clean_name(name.empty() ? file_edit_last_name() : name);
    g_file_edit_path = file_edit_path_for(clean);
    if(!path_is_file(g_file_edit_path)) safe_write_file(g_file_edit_path, "");
    file_edit_set_last(clean);
    g_quick_slot = -1;
    g_prompt.clear();
    g_edit_file.clear();
    g_ol.editPath.clear();
    goto_screen(Screen::Editor);
}

static void file_panel_begin_prompt(bool rename) {
    auto &p = g_file_panel_state;
    p.prompt = true;
    p.rename = rename;
    p.confirmDelete = false;
    p.message.clear();
    if(rename && !p.entries.empty()) p.input = p.entries[p.sel];
    else p.input = "untitled.txt";
    draw_file_panel();
}

static bool handle_file_panel_key(int key) {
    auto &p = g_file_panel_state;
    if(!p.active) return false;
    if(p.confirmDelete) {
        if((key == '\n' || key == '\r' || key == 'y' || key == 'Y') && !p.entries.empty()) {
            std::string name = p.entries[p.sel];
            std::string path = file_edit_path_for(name);
            p.message = remove(path.c_str()) == 0 ? "已删除" : "删除失败";
            file_panel_reload();
            p.confirmDelete = false;
        } else if(key == 27 || key == 'n' || key == 'N' || key == 'q' || key == 'Q') {
            p.confirmDelete = false;
            p.message = "已取消";
        }
        draw_file_panel();
        return true;
    }
    if(p.prompt) {
        // 文件名也能用中文输入法:面板的输入是一个裸 string(不是 textarea),自己接。
        // 输入法没开时 handle_key 直接返回 false,下面的 ASCII 分支照旧。
        if(key == KEY_IME_TOGGLE) {
            g_linux_ime.toggle();
            draw_file_panel();
            update_ime_bar();
            return true;
        }
        std::string ime_out;
        if(g_linux_ime.handle_key(key, ime_out)) {
            if(!ime_out.empty()) p.input += ime_out;
            draw_file_panel();
            update_ime_bar();
            return true;
        }
        if(key == 27) {
            p.prompt = false;
        } else if(key == '\n' || key == '\r') {
            std::string clean = file_edit_clean_name(p.input);
            bool ok = false;
            if(p.rename && !p.entries.empty()) {
                std::string old = p.entries[p.sel];
                std::string old_path = file_edit_path_for(old);
                std::string new_path = file_edit_path_for(clean);
                if(old != clean && !path_is_file(new_path)) ok = rename(old_path.c_str(), new_path.c_str()) == 0;
                if(ok && g_file_edit_path == old_path) {
                    g_file_edit_path = new_path;
                    file_edit_set_last(clean);
                }
            } else {
                std::string path = file_edit_path_for(clean);
                if(!path_is_file(path)) ok = safe_write_file(path, "");
            }
            p.prompt = false;
            p.message = ok ? "完成" : "失败";
            file_panel_reload(clean);
        } else if(key == 8 || key == 127) {
            utf8_pop_back(p.input);
        } else if(key >= 32 && key < 127) {
            p.input.push_back((char)key);
        }
        draw_file_panel();
        update_ime_bar();
        return true;
    }
    if(key == 0x05 || key == 27 || key == 'q' || key == 'Q') {
        p.active = false;
        draw_file_panel();
        update_ime_bar();
        return true;
    }
    if(key == KEY_UP || key == 'k') {
        if(p.sel > 0) p.sel--;
    } else if(key == KEY_DOWN || key == 'j') {
        if(p.sel < (int)p.entries.size() - 1) p.sel++;
    } else if(key == 'a' || key == 'A') {
        file_panel_begin_prompt(false);
        return true;
    } else if((key == 'r' || key == 'R') && !p.entries.empty()) {
        file_panel_begin_prompt(true);
        return true;
    } else if((key == 'd' || key == 'D') && !p.entries.empty()) {
        std::string name = p.entries[p.sel];
        std::string path = file_edit_path_for(name);
        if(path == g_file_edit_path) {
            p.message = "当前文件不可删";
        } else {
            p.confirmDelete = true;
            p.message = "删除 " + name + "?";
        }
    } else if((key == '\n' || key == '\r') && !p.entries.empty()) {
        std::string name = p.entries[p.sel];
        save_editor_text();
        p.active = false;
        open_file_editor(name);
        return true;
    }
    draw_file_panel();
    return true;
}

static void insert_utf8(const std::string &s) {
    if(g_screen == Screen::Editor && g_editor && utf8_valid(s)) lv_textarea_add_text(g_editor, s.c_str());
    if(g_screen == Screen::SettingText && g_setting_text && utf8_valid(s)) lv_textarea_add_text(g_setting_text, s.c_str());
    if(g_screen == Screen::Gtd && g_gtd.input && utf8_valid(s)) lv_textarea_add_text(g_gtd.input, s.c_str());
    if(g_screen == Screen::Outline && g_ol.input && utf8_valid(s)) lv_textarea_add_text(g_ol.input, s.c_str());
    if(g_screen == Screen::Inspiration && g_inspiration_text && utf8_valid(s)) lv_textarea_add_text(g_inspiration_text, s.c_str());
    if(g_screen == Screen::Wifi && g_wifi_password && utf8_valid(s)) lv_textarea_add_text(g_wifi_password, s.c_str());
}

static void handle_main(int key) {
    int visible = g_journal.total_entries() > 0 ? 7 : 6;
    if(key == KEY_LEFT || key == KEY_UP || key == 'h' || key == 'k') g_main_sel = (g_main_sel + visible - 1) % visible;
    else if(key == KEY_RIGHT || key == KEY_DOWN || key == 'l' || key == 'j') g_main_sel = (g_main_sel + 1) % visible;
    else {
        char ch = key >= 'A' && key <= 'Z' ? key + 32 : key;
        for(int i = 0; i < visible; ++i) {
            int idx = (g_journal.total_entries() > 0 || i < 2) ? i : i + 1;
            if(k_actions[idx].key == ch) {
                g_main_sel = i;
                key = '\n';
                break;
            }
        }
    }
    if(key == '\n' || key == '\r') {
        int idx = (g_journal.total_entries() > 0 || g_main_sel < 2) ? g_main_sel : g_main_sel + 1;
        const Action &a = k_actions[idx];
        if(a.key == 'p') { g_quick_slot = -1; g_file_edit_path.clear(); g_prompt = random_builtin_prompt(); g_edit_file.clear(); goto_screen(Screen::Editor); return; }
        if(a.key == 'f') { g_quick_slot = -1; g_file_edit_path.clear(); g_prompt.clear(); g_edit_file.clear(); goto_screen(Screen::Editor); return; }
        if(a.key == 'v') { g_browser_sel = 0; goto_screen(Screen::Browser); return; }
        goto_screen(a.screen);
        return;
    }
    render();
}

static void handle_browser(int key) {
    if(key == 'q' || key == 27) { goto_screen(Screen::Main); return; }
    if(key == KEY_DOWN || key == 'j') g_browser_sel++;
    if(key == KEY_UP || key == 'k') g_browser_sel--;
    if(g_browser_sel < 0) g_browser_sel = 0;
    if(g_browser_sel >= (int)g_entries.size()) g_browser_sel = (int)g_entries.size() - 1;
    if(g_entries.empty()) { goto_screen(Screen::Main); return; }
    if(key == '\n' || key == '\r') { g_view_file = g_entries[g_browser_sel].filename; g_viewer_scroll = 0; goto_screen(Screen::Viewer); return; }
    if(key == 'e' || key == 'E') { g_quick_slot = -1; g_file_edit_path.clear(); g_edit_file = g_entries[g_browser_sel].filename; g_prompt.clear(); goto_screen(Screen::Editor); return; }
    if(key == 'h' || key == 'H') { open_history(g_entries[g_browser_sel].filename, Screen::Browser); return; }
    if(key == 'd' || key == 'D') { g_journal.delete_entry(g_entries[g_browser_sel].filename); }
    render();
}

static void handle_history(int key) {
    if(g_history_confirm_restore) {
        if((key == '\n' || key == '\r' || key == 'y' || key == 'Y') && !g_history_versions.empty()) {
            std::string hist = g_history_versions[g_history_sel].filename;
            bool ok = g_journal.restore_history_version(g_history_file, hist);
            g_history_confirm_restore = false;
            if(ok) {
                if(g_history_return == Screen::Editor) {
                    g_quick_slot = -1;
                    g_file_edit_path.clear();
                    g_edit_file = g_history_file;
                    g_prompt.clear();
                }
                goto_screen(g_history_return);
            } else {
                set_status("恢复失败");
            }
            return;
        }
        if(key == 'q' || key == 'n' || key == 'N' || key == 27) {
            g_history_confirm_restore = false;
            render();
            return;
        }
        render();
        return;
    }
    if(g_history_confirm_delete) {
        if((key == '\n' || key == '\r' || key == 'y' || key == 'Y') && !g_history_versions.empty()) {
            bool ok = g_journal.delete_history_version(g_history_file, g_history_versions[g_history_sel].filename);
            g_history_versions = g_journal.list_history_versions(g_history_file);
            if(g_history_sel >= (int)g_history_versions.size()) g_history_sel = (int)g_history_versions.size() - 1;
            if(g_history_sel < 0) g_history_sel = 0;
            g_history_preview = false;
            g_history_confirm_delete = false;
            render();
            set_status(ok ? "已删除历史版本" : "删除失败");
            return;
        }
        if(key == 'q' || key == 'n' || key == 'N' || key == 27) {
            g_history_confirm_delete = false;
            render();
            return;
        }
        render();
        return;
    }

    if(g_history_preview) {
        if(key == 'q' || key == 'Q' || key == 27) {
            g_history_preview = false;
            g_history_preview_scroll = 0;
            render();
            return;
        }
        if(key == KEY_DOWN || key == 'j' || key == KEY_PAGE_DOWN) g_history_preview_scroll += key == KEY_PAGE_DOWN ? 10 : 1;
        if(key == KEY_UP || key == 'k' || key == KEY_PAGE_UP) g_history_preview_scroll -= key == KEY_PAGE_UP ? 10 : 1;
        if(g_history_preview_scroll < 0) g_history_preview_scroll = 0;
        if(key == 'r' || key == 'R') g_history_confirm_restore = true;
        if(key == 'd' || key == 'D') g_history_confirm_delete = true;
        render();
        return;
    }

    int total = (int)g_history_versions.size();
    if(key == 'q' || key == 'Q' || key == 27) { goto_screen(g_history_return); return; }
    if((key == KEY_DOWN || key == 'j') && g_history_sel < total - 1) g_history_sel++;
    if((key == KEY_UP || key == 'k') && g_history_sel > 0) g_history_sel--;
    if((key == '\n' || key == '\r') && total > 0) {
        g_history_preview = true;
        g_history_preview_scroll = 0;
    }
    if((key == 'r' || key == 'R') && total > 0) g_history_confirm_restore = true;
    if((key == 'd' || key == 'D') && total > 0) g_history_confirm_delete = true;
    render();
}

// 设置界面的退出。回到进来时的那个界面:快捷/文件编辑模式下按 Esc 是进来调参数的,
// 退出要回到原来那个编辑模式和同一个文件,而不是被丢回个人日记——设备常把工作模式设
// 成 file、当专用编辑器用,一按 Esc 就跳去个人日记很突兀。工作模式若刚在设置里改过,
// 以新模式为准。
static void settings_leave() {
    if(g_settings_return != Screen::Editor) { goto_screen(g_settings_return); return; }
    const std::string am = g_settings.app_mode();
    if(am == "journal") {
        g_quick_slot = -1;
        g_file_edit_path.clear();
        goto_screen(Screen::Main);
        return;
    }
    if(am == "quick" && g_quick_slot < 0) { open_quick_editor(0); return; }
    if(am == "file" && g_file_edit_path.empty()) { open_file_editor(file_edit_last_name()); return; }
    goto_screen(Screen::Editor);
}

static void handle_settings(int key) {
    auto items = settings_items();
    if(g_setting_pick_active) {
        int total = (int)g_setting_pick_opts.size();
        if(key == 27 || key == 'q') {
            g_setting_pick_active = false;
            render();
            return;
        }
        if(key == KEY_DOWN || key == 'j') g_setting_pick_sel++;
        if(key == KEY_UP || key == 'k') g_setting_pick_sel--;
        if(g_setting_pick_sel < 0) g_setting_pick_sel = 0;
        if(g_setting_pick_sel >= total) g_setting_pick_sel = total - 1;
        if((key == '\n' || key == '\r' || key == KEY_RIGHT) && total > 0) {
            setting_apply(g_setting_pick_key, g_setting_pick_opts[g_setting_pick_sel].first);
            g_setting_pick_active = false;
        }
        render();
        return;
    }
    if(key == 'q' || key == 27) { settings_leave(); return; }
    if(key == KEY_DOWN || key == 'j') g_settings_sel++;
    if(key == KEY_UP || key == 'k') g_settings_sel--;
    if(g_settings_sel < 0) g_settings_sel = 0;
    if(g_settings_sel >= (int)items.size()) g_settings_sel = (int)items.size() - 1;
    if(key == '\n' || key == '\r' || key == KEY_RIGHT || key == KEY_LEFT) {
        const SetItem &it = items[g_settings_sel];
        const std::string &k = it.key;
        if(k == "wifi") {
            g_wifi_entries.clear();
            g_wifi_saved_entries.clear();
            goto_screen(Screen::Wifi);
            return;
        }
        if(k == "dict") { goto_screen(Screen::Dict); return; }
        if(k == "file_mgr") { goto_screen(Screen::FileMgr); return; }
        if(k == "journal_dir") { open_setting_text("journal_dir", "保存位置"); return; }
        if(k == "webdav_url") { open_setting_text("webdav_url", "WebDAV URL"); return; }
        if(k == "webdav_user") { open_setting_text("webdav_user", "WebDAV 用户"); return; }
        if(k == "webdav_pass") { open_setting_text("webdav_pass", "WebDAV 密码"); return; }
        if(k == "deepseek_key") { open_setting_text("deepseek_key", "Deepseek Key"); return; }
        if(k == "polish_prompt") { open_setting_text("polish_prompt", "润色提示词"); return; }
        if(k == "flomo_email") { open_setting_text("flomo_email", "Flomo 邮箱"); return; }
        if(k == "flomo_pass") { open_setting_text("flomo_pass", "Flomo 密码"); return; }
        if(k == "flomo_token") {
            std::string m;
            set_status("正在生成Flomo Token...");
            lv_timer_handler();
            flomo_generate_token(m);
            // render() 会重建状态栏,提示必须在它之后再写
            render();
            set_status(m);
            return;
        }
        if(k == "personal_exp") { open_setting_text("personal_exp", "个人经历"); return; }
        if(k == "personal_hob") { open_setting_text("personal_hob", "个人爱好"); return; }
        if(k == "file_mgr_token") { open_setting_text("file_mgr_token", "文件管理密码"); return; }
        open_setting_pick(it);
        return;
    }
    render();
}

static void handle_setting_text(int key) {
    if(key == KEY_IME_TOGGLE) {
        g_linux_ime.toggle();
        update_ime_bar();
        return;
    }
    std::string ime_out;
    if(g_linux_ime.handle_key(key, ime_out)) {
        if(!ime_out.empty() && g_setting_text) lv_textarea_add_text(g_setting_text, ime_out.c_str());
        update_ime_bar();
        return;
    }
    if(key == 27) {
        goto_screen(Screen::Settings);
        return;
    }
    if(key == '\n' || key == '\r' || key == 0x13) {
        std::string value = g_setting_text ? lv_textarea_get_text(g_setting_text) : "";
        if(g_setting_key == "journal_dir") {
            if(value.empty() || value[0] != '/') {
                set_status("请输入绝对路径");
                return;
            }
            if(!ensure_dir_path(value)) {
                set_status("无法创建目录");
                return;
            }
        }
        if(!utf8_valid(value)) {
            set_status("UTF-8无效，未保存");
            return;
        }
        g_settings.set(g_setting_key, value);
        g_journal.begin();
        g_entries.clear();
        goto_screen(Screen::Settings);
        return;
    }
    if(key == KEY_LEFT) lv_textarea_cursor_left(g_setting_text);
    else if(key == KEY_RIGHT) lv_textarea_cursor_right(g_setting_text);
    else if(key == KEY_HOME) lv_textarea_set_cursor_pos(g_setting_text, 0);
    else if(key == KEY_END) lv_textarea_set_cursor_pos(g_setting_text, LV_TEXTAREA_CURSOR_LAST);
    else if(key == 8 || key == 127) lv_textarea_delete_char(g_setting_text);
    else if(key >= 32 && key < 127) lv_textarea_add_char(g_setting_text, (uint32_t)key);
}

static void gtd_cycle_status(JsonValue &task) {
    std::string s = task["status"].asString("todo");
    if(s == "todo") s = "doing";
    else if(s == "doing") s = "done";
    else if(s == "done") s = "waiting";
    else s = "todo";
    task.set("status", s);
    task.set("completed", s == "done" ? today_string() : "");
}

static void gtd_remember_project(const std::string &name) {
    if(name.empty()) return;
    auto &projs = g_gtd.data["projects"];
    for(int i = 0; i < (int)projs.size(); ++i)
        if(projs[i].asString() == name) return;
    projs.pushBack(JsonValue(name));
}

static void gtd_remember_named(const char *arrayKey, const std::string &name) {
    if(name.empty()) return;
    auto &arr = g_gtd.data[arrayKey];
    for(int i = 0; i < (int)arr.size(); ++i)
        if(arr[i].asString() == name) return;
    arr.pushBack(JsonValue(name));
}

static void gtd_open_picker(int fieldIdx) {
    if(g_gtd.detailIdx < 0 || g_gtd.detailIdx >= (int)g_gtd.data["tasks"].size()) return;
    auto &task = g_gtd.data["tasks"][g_gtd.detailIdx];
    const GtdField &f = k_gtd_fields[fieldIdx];
    g_gtd.pickerOpts.clear();
    g_gtd.pickerToggled.clear();
    g_gtd.pickerField = fieldIdx;
    g_gtd.pickerSel = 0;
    g_gtd.pickerScroll = 0;
    if(f.type == 'p') {
        for(int i = 0; i < 3; ++i) g_gtd.pickerOpts.push_back({k_gtd_priority[i], k_gtd_priority_disp[i]});
        std::string cur = task["priority"].asString("B");
        for(int i = 0; i < 3; ++i)
            if(std::string(k_gtd_priority[i]) == cur) g_gtd.pickerSel = i;
    } else if(f.type == 't') {
        for(int i = 0; i < 4; ++i) g_gtd.pickerOpts.push_back({k_gtd_status[i], k_gtd_status_disp[i]});
        std::string cur = task["status"].asString("todo");
        for(int i = 0; i < 4; ++i)
            if(std::string(k_gtd_status[i]) == cur) g_gtd.pickerSel = i;
    } else if(f.type == 'j') {
        gtd_build_project_list();
        g_gtd.pickerOpts.push_back({"", "(无)"});
        for(auto &p : g_gtd.projectList) g_gtd.pickerOpts.push_back({p, p});
        std::string cur = task["project"].asString();
        for(int i = 0; i < (int)g_gtd.pickerOpts.size(); ++i)
            if(g_gtd.pickerOpts[i].first == cur) { g_gtd.pickerSel = i; break; }
    } else if(f.type == 'c') {
        gtd_build_context_list();
        g_gtd.pickerOpts.push_back({"", "(无)"});
        for(auto &c : g_gtd.contextList) g_gtd.pickerOpts.push_back({c, "@" + c});
        std::string cur = task["context"].asString();
        for(int i = 0; i < (int)g_gtd.pickerOpts.size(); ++i)
            if(g_gtd.pickerOpts[i].first == cur) { g_gtd.pickerSel = i; break; }
    } else if(f.type == 'g') {
        gtd_build_tag_list();
        for(auto &t : g_gtd.tagList) g_gtd.pickerOpts.push_back({t, "#" + t});
        auto &tt = task["tags"];
        if(tt.isArray())
            for(int i = 0; i < (int)g_gtd.pickerOpts.size(); ++i)
                for(int j = 0; j < (int)tt.size(); ++j)
                    if(g_gtd.pickerOpts[i].first == tt[j].asString()) {
                        g_gtd.pickerToggled.insert(i);
                        break;
                    }
    }
    g_gtd.mode = GtdMode::Picker;
}

static void gtd_picker_commit() {
    if(g_gtd.detailIdx < 0 || g_gtd.pickerField < 0 || g_gtd.pickerField >= k_gtd_field_count) return;
    auto &task = g_gtd.data["tasks"][g_gtd.detailIdx];
    const GtdField &f = k_gtd_fields[g_gtd.pickerField];
    if(f.type == 'g') {
        JsonValue arr = JsonValue::array();
        for(int i : g_gtd.pickerToggled)
            if(i >= 0 && i < (int)g_gtd.pickerOpts.size()) arr.pushBack(JsonValue(g_gtd.pickerOpts[i].first));
        task.set("tags", arr);
        gtd_save();
        return;
    }
    if(g_gtd.pickerSel < 0 || g_gtd.pickerSel >= (int)g_gtd.pickerOpts.size()) return;
    std::string val = g_gtd.pickerOpts[g_gtd.pickerSel].first;
    if(f.type == 'p') {
        task.set("priority", val);
    } else if(f.type == 't') {
        task.set("status", val);
        task.set("completed", val == "done" ? today_string() : "");
    } else if(f.type == 'j') {
        task.set("project", val);
        gtd_remember_project(val);
    } else if(f.type == 'c') {
        task.set("context", val);
        gtd_remember_named("contexts", val);
    }
    gtd_save();
}

static void gtd_open_calendar() {
    std::string due = g_gtd.detailIdx >= 0 ? g_gtd.data["tasks"][g_gtd.detailIdx]["due"].asString() : "";
    time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    g_gtd.calYear = local.tm_year + 1900;
    g_gtd.calMonth = local.tm_mon + 1;
    g_gtd.calSelDay = local.tm_mday;
    if(due.size() >= 10) {
        g_gtd.calYear = atoi(due.substr(0, 4).c_str());
        g_gtd.calMonth = atoi(due.substr(5, 2).c_str());
        g_gtd.calSelDay = atoi(due.substr(8, 2).c_str());
        if(g_gtd.calMonth < 1 || g_gtd.calMonth > 12) g_gtd.calMonth = 1;
    }
    int days = gtd_days_in_month(g_gtd.calYear, g_gtd.calMonth);
    if(g_gtd.calSelDay < 1 || g_gtd.calSelDay > days) g_gtd.calSelDay = 1;
    g_gtd.mode = GtdMode::Calendar;
}

static void gtd_calendar_commit(bool clear) {
    if(g_gtd.detailIdx < 0) return;
    auto &task = g_gtd.data["tasks"][g_gtd.detailIdx];
    if(clear) {
        task.set("due", "");
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", g_gtd.calYear, g_gtd.calMonth, g_gtd.calSelDay);
        task.set("due", std::string(buf));
    }
    gtd_save();
}

static void gtd_delete_task(int taskIdx) {
    auto &tasks = g_gtd.data["tasks"];
    if(taskIdx < 0 || taskIdx >= (int)tasks.size()) return;
    tasks.elements.erase(tasks.elements.begin() + taskIdx);
}

static void gtd_delete_selected() {
    auto &tasks = g_gtd.data["tasks"];
    std::vector<int> idxs;
    for(int i = 0; i < (int)tasks.size(); ++i)
        if(g_gtd.multiSel.count(tasks[i]["id"].asString())) idxs.push_back(i);
    std::sort(idxs.rbegin(), idxs.rend());
    for(int i : idxs)
        if(i < (int)tasks.elements.size()) tasks.elements.erase(tasks.elements.begin() + i);
    g_gtd.multiSel.clear();
    g_gtd.multiAnchor = -1;
}

static void gtd_exec_confirm() {
    switch(g_gtd.confirm) {
        case GtdConfirm::Task:
            gtd_delete_task(g_gtd.detailIdx);
            g_gtd.detailIdx = -1;
            break;
        case GtdConfirm::Tasks: gtd_delete_selected(); break;
        case GtdConfirm::Project:
            if(g_gtd.projectDrill >= 0 && g_gtd.projectDrill < (int)g_gtd.projectList.size()) {
                std::string name = g_gtd.projectList[g_gtd.projectDrill];
                auto &projs = g_gtd.data["projects"];
                for(int i = (int)projs.size() - 1; i >= 0; --i)
                    if(projs[i].asString() == name) projs.elements.erase(projs.elements.begin() + i);
                auto &tasks = g_gtd.data["tasks"];
                for(int i = 0; i < (int)tasks.size(); ++i)
                    if(tasks[i]["project"].asString() == name) tasks[i].set("project", "");
                g_gtd.projectDrill = -1;
            }
            break;
        case GtdConfirm::Context:
            if(!g_gtd.renameTarget.empty()) {
                auto &arr = g_gtd.data["contexts"];
                for(int i = (int)arr.size() - 1; i >= 0; --i)
                    if(arr[i].asString() == g_gtd.renameTarget) arr.elements.erase(arr.elements.begin() + i);
                auto &tasks = g_gtd.data["tasks"];
                for(int i = 0; i < (int)tasks.size(); ++i)
                    if(tasks[i]["context"].asString() == g_gtd.renameTarget) tasks[i].set("context", "");
            }
            break;
        case GtdConfirm::Tag:
            if(!g_gtd.renameTarget.empty()) {
                auto &arr = g_gtd.data["tags"];
                for(int i = (int)arr.size() - 1; i >= 0; --i)
                    if(arr[i].asString() == g_gtd.renameTarget) arr.elements.erase(arr.elements.begin() + i);
                auto &tasks = g_gtd.data["tasks"];
                for(int i = 0; i < (int)tasks.size(); ++i) {
                    auto &tt = tasks[i]["tags"];
                    if(!tt.isArray()) continue;
                    JsonValue kept = JsonValue::array();
                    for(int j = 0; j < (int)tt.size(); ++j)
                        if(tt[j].asString() != g_gtd.renameTarget) kept.pushBack(tt[j]);
                    tasks[i].set("tags", kept);
                }
            }
            break;
        case GtdConfirm::ArchiveMonth:
            if(g_gtd.archiveSel >= 0 && g_gtd.archiveSel < (int)g_gtd.archiveMonths.size()) {
                std::string path = gtd_archive_dir() + "/" + g_gtd.archiveMonths[g_gtd.archiveSel] + ".json";
                remove(path.c_str());
                g_gtd.archiveMonths.erase(g_gtd.archiveMonths.begin() + g_gtd.archiveSel);
                if(g_gtd.archiveSel >= (int)g_gtd.archiveMonths.size())
                    g_gtd.archiveSel = (int)g_gtd.archiveMonths.size() - 1;
                if(g_gtd.archiveSel < 0) g_gtd.archiveSel = 0;
            }
            break;
        default: break;
    }
    g_gtd.confirm = GtdConfirm::None;
    gtd_save();
    gtd_rebuild();
}

static void gtd_open_detail() {
    if(g_gtd.sel < 0 || g_gtd.sel >= (int)g_gtd.filtered.size()) return;
    g_gtd.detailIdx = g_gtd.filtered[g_gtd.sel];
    g_gtd.detailField = 0;
    g_gtd.mode = GtdMode::Detail;
}

static void gtd_begin_confirm(const std::string &title, const std::string &action, GtdConfirm kind,
                              GtdMode ret = GtdMode::List) {
    g_gtd.confirmTitle = title;
    g_gtd.confirmAction = action;
    g_gtd.confirm = kind;
    g_gtd.confirmReturn = ret;
    g_gtd.mode = GtdMode::Confirm;
}

static void gtd_cycle_view(int dir) {
    g_gtd.view = (g_gtd.view + dir + 5) % 5;
    g_gtd.sel = 0;
    g_gtd.scroll = 0;
    g_gtd.projectDrill = -1;
    g_gtd.folded.clear();
    g_gtd.multiSel.clear();
    g_gtd.multiAnchor = -1;
    gtd_rebuild();
}

static void handle_gtd(int key) {
    // ── 文本输入类模式 ────────────────────────────────────────────────
    if(g_gtd.mode == GtdMode::Add || g_gtd.mode == GtdMode::AddProject ||
       g_gtd.mode == GtdMode::AddContext || g_gtd.mode == GtdMode::AddTag) {
        if(key == KEY_CTRL_ENTER) key = '\n';
        if(gtd_input_key(key)) return;
        if(key == 27) {
            g_gtd.mode = GtdMode::List;
            g_gtd.pendingParent.clear();
            g_gtd.pendingProject.clear();
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            std::string text = g_gtd.input ? lv_textarea_get_text(g_gtd.input) : "";
            if(!text.empty() && utf8_valid(text)) {
                if(g_gtd.mode == GtdMode::AddProject) {
                    gtd_remember_project(text);
                } else if(g_gtd.mode == GtdMode::AddContext) {
                    gtd_remember_named("contexts", text);
                } else if(g_gtd.mode == GtdMode::AddTag) {
                    gtd_remember_named("tags", text);
                } else {
                    std::string status = g_gtd.view == 1 ? "doing" : (g_gtd.view == 2 ? "waiting" : "todo");
                    JsonValue t = gtd_new_task(text, status, g_gtd.pendingParent, g_gtd.pendingProject);
                    auto &tasks = g_gtd.data["tasks"];
                    if(g_gtd.insertAfter >= 0 && g_gtd.insertAfter < (int)tasks.size())
                        tasks.elements.insert(tasks.elements.begin() + g_gtd.insertAfter + 1, t);
                    else
                        tasks.pushBack(t);
                    if(!g_gtd.pendingProject.empty()) gtd_remember_project(g_gtd.pendingProject);
                }
                gtd_save();
            }
            g_gtd.mode = GtdMode::List;
            g_gtd.pendingParent.clear();
            g_gtd.pendingProject.clear();
            g_gtd.insertAfter = -1;
            gtd_rebuild();
            render();
            return;
        }
        return;
    }

    if(g_gtd.mode == GtdMode::Filter) {
        if(gtd_input_key(key)) return;
        if(key == 27) {
            g_gtd.filterText.clear();
            g_gtd.mode = GtdMode::List;
            gtd_rebuild();
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            g_gtd.filterText = g_gtd.input ? lv_textarea_get_text(g_gtd.input) : "";
            g_gtd.mode = GtdMode::List;
            g_gtd.sel = 0;
            g_gtd.scroll = 0;
            gtd_rebuild();
            render();
            return;
        }
        return;
    }

    if(g_gtd.mode == GtdMode::Rename || g_gtd.mode == GtdMode::RenameProject ||
       g_gtd.mode == GtdMode::RenameContext || g_gtd.mode == GtdMode::RenameTag) {
        if(gtd_input_key(key)) return;
        GtdMode back = g_gtd.mode == GtdMode::RenameProject ? GtdMode::List
                       : (g_gtd.mode == GtdMode::RenameContext ? GtdMode::ContextMgr
                                                               : GtdMode::TagMgr);
        if(key == 27) {
            g_gtd.mode = back;
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            std::string text = g_gtd.input ? lv_textarea_get_text(g_gtd.input) : "";
            if(!text.empty() && utf8_valid(text)) {
                if(g_gtd.mode == GtdMode::Rename) {
                    if(g_gtd.detailIdx >= 0 && g_gtd.detailIdx < (int)g_gtd.data["tasks"].size())
                        g_gtd.data["tasks"][g_gtd.detailIdx].set("title", text);
                } else if(g_gtd.mode == GtdMode::RenameProject) {
                    if(g_gtd.projectDrill >= 0 && g_gtd.projectDrill < (int)g_gtd.projectList.size()) {
                        std::string old = g_gtd.projectList[g_gtd.projectDrill];
                        auto &projs = g_gtd.data["projects"];
                        for(int i = 0; i < (int)projs.size(); ++i)
                            if(projs[i].asString() == old) projs[i] = JsonValue(text);
                        auto &tasks = g_gtd.data["tasks"];
                        for(int i = 0; i < (int)tasks.size(); ++i)
                            if(tasks[i]["project"].asString() == old) tasks[i].set("project", text);
                    }
                } else {
                    const char *arrKey = g_gtd.mode == GtdMode::RenameContext ? "contexts" : "tags";
                    const char *taskKey = g_gtd.mode == GtdMode::RenameContext ? "context" : nullptr;
                    std::string old = g_gtd.renameTarget;
                    auto &arr = g_gtd.data[arrKey];
                    for(int i = 0; i < (int)arr.size(); ++i)
                        if(arr[i].asString() == old) arr[i] = JsonValue(text);
                    auto &tasks = g_gtd.data["tasks"];
                    for(int i = 0; i < (int)tasks.size(); ++i) {
                        if(taskKey) {
                            if(tasks[i][taskKey].asString() == old) tasks[i].set(taskKey, text);
                        } else {
                            auto &tt = tasks[i]["tags"];
                            if(!tt.isArray()) continue;
                            for(int j = 0; j < (int)tt.size(); ++j)
                                if(tt[j].asString() == old) tt[j] = JsonValue(text);
                        }
                    }
                }
                gtd_save();
            }
            g_gtd.mode = back;
            if(back == GtdMode::List) gtd_rebuild();
            render();
            return;
        }
        return;
    }

    if(g_gtd.mode == GtdMode::Note) {
        if(key == KEY_CTRL_ENTER || key == 27) {
            if(key == KEY_CTRL_ENTER && g_gtd.detailIdx >= 0 &&
               g_gtd.detailIdx < (int)g_gtd.data["tasks"].size()) {
                g_gtd.data["tasks"][g_gtd.detailIdx].set(
                    "note", g_gtd.input ? lv_textarea_get_text(g_gtd.input) : "");
                gtd_save();
            }
            g_gtd.mode = GtdMode::Detail;
            render();
            return;
        }
        gtd_input_key(key);
        return;
    }

    // ── 弹窗类模式 ────────────────────────────────────────────────────
    if(g_gtd.mode == GtdMode::Picker) {
        int n = (int)g_gtd.pickerOpts.size();
        if(key == 27 || key == 'q') { g_gtd.mode = GtdMode::Detail; render(); return; }
        if(key == KEY_UP || key == 'k') { if(g_gtd.pickerSel > 0) g_gtd.pickerSel--; render(); return; }
        if(key == KEY_DOWN || key == 'j') { if(g_gtd.pickerSel < n - 1) g_gtd.pickerSel++; render(); return; }
        if(key == ' ' && k_gtd_fields[g_gtd.pickerField].type == 'g') {
            if(g_gtd.pickerToggled.count(g_gtd.pickerSel)) g_gtd.pickerToggled.erase(g_gtd.pickerSel);
            else g_gtd.pickerToggled.insert(g_gtd.pickerSel);
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            gtd_picker_commit();
            g_gtd.mode = GtdMode::Detail;
            render();
            return;
        }
        return;
    }

    if(g_gtd.mode == GtdMode::Calendar) {
        if(key == 27 || key == 'q') { g_gtd.mode = GtdMode::Detail; render(); return; }
        if(key == 'c' || key == 'C') {
            gtd_calendar_commit(true);
            g_gtd.mode = GtdMode::Detail;
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            gtd_calendar_commit(false);
            g_gtd.mode = GtdMode::Detail;
            render();
            return;
        }
        int days = gtd_days_in_month(g_gtd.calYear, g_gtd.calMonth);
        if(key == KEY_LEFT) { if(g_gtd.calSelDay > 1) g_gtd.calSelDay--; }
        else if(key == KEY_RIGHT) { if(g_gtd.calSelDay < days) g_gtd.calSelDay++; }
        else if(key == KEY_UP) { g_gtd.calSelDay -= 7; }
        else if(key == KEY_DOWN) { g_gtd.calSelDay += 7; }
        else if(key == KEY_PAGE_UP || key == 'h') {
            if(--g_gtd.calMonth < 1) { g_gtd.calMonth = 12; g_gtd.calYear--; }
        } else if(key == KEY_PAGE_DOWN || key == 'l') {
            if(++g_gtd.calMonth > 12) { g_gtd.calMonth = 1; g_gtd.calYear++; }
        }
        days = gtd_days_in_month(g_gtd.calYear, g_gtd.calMonth);
        if(g_gtd.calSelDay < 1) g_gtd.calSelDay = 1;
        if(g_gtd.calSelDay > days) g_gtd.calSelDay = days;
        render();
        return;
    }

    if(g_gtd.mode == GtdMode::Confirm) {
        if(key == '\n' || key == '\r') {
            GtdMode ret = g_gtd.confirmReturn;
            gtd_exec_confirm();
            g_gtd.mode = ret;
            render();
            return;
        }
        if(key == 27 || key == 'q') {
            g_gtd.confirm = GtdConfirm::None;
            g_gtd.mode = g_gtd.confirmReturn;
            render();
            return;
        }
        return;
    }

    if(g_gtd.mode == GtdMode::Help) {
        int n = (int)(sizeof(k_gtd_help) / sizeof(k_gtd_help[0]));
        if(key == KEY_UP || key == 'k') { if(g_gtd.helpScroll > 0) g_gtd.helpScroll--; render(); return; }
        if(key == KEY_DOWN || key == 'j') { if(g_gtd.helpScroll < n - 1) g_gtd.helpScroll++; render(); return; }
        g_gtd.mode = g_gtd.helpPrev;
        render();
        return;
    }

    if(g_gtd.mode == GtdMode::Summary) {
        if(key == KEY_UP || key == 'k') { if(g_gtd.summaryScroll > 0) g_gtd.summaryScroll--; render(); return; }
        if(key == KEY_DOWN || key == 'j') { g_gtd.summaryScroll++; render(); return; }
        g_gtd.mode = g_gtd.summaryPrev;
        render();
        return;
    }

    if(g_gtd.mode == GtdMode::Archive) {
        if(key == 27 || key == 'q') {
            if(g_gtd.archiveBrowsing) g_gtd.archiveBrowsing = false;
            else g_gtd.mode = GtdMode::List;
            render();
            return;
        }
        if(key == KEY_UP || key == 'k') {
            if(g_gtd.archiveBrowsing) { if(g_gtd.archiveViewSel > 0) g_gtd.archiveViewSel--; }
            else if(g_gtd.archiveSel > 0) g_gtd.archiveSel--;
            render();
            return;
        }
        if(key == KEY_DOWN || key == 'j') {
            if(g_gtd.archiveBrowsing) {
                if(g_gtd.archiveViewSel < (int)g_gtd.archiveTasks.size() - 1) g_gtd.archiveViewSel++;
            } else if(g_gtd.archiveSel < (int)g_gtd.archiveMonths.size() - 1) g_gtd.archiveSel++;
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            if(!g_gtd.archiveBrowsing && g_gtd.archiveSel >= 0 &&
               g_gtd.archiveSel < (int)g_gtd.archiveMonths.size()) {
                g_gtd.archiveViewMonth = g_gtd.archiveMonths[g_gtd.archiveSel];
                gtd_load_archive_month(g_gtd.archiveViewMonth);
                g_gtd.archiveBrowsing = true;
                g_gtd.archiveViewSel = 0;
            }
            render();
            return;
        }
        if((key == 'd' || key == 'D') && !g_gtd.archiveBrowsing && !g_gtd.archiveMonths.empty()) {
            gtd_begin_confirm("删除归档", g_gtd.archiveMonths[g_gtd.archiveSel] + " 的全部任务",
                              GtdConfirm::ArchiveMonth, GtdMode::Archive);
            render();
            return;
        }
        if(key == '?' || key == 'h') {
            g_gtd.helpScroll = 0;
            g_gtd.helpPrev = GtdMode::Archive;
            g_gtd.mode = GtdMode::Help;
            render();
            return;
        }
        return;
    }

    if(g_gtd.mode == GtdMode::ContextMgr || g_gtd.mode == GtdMode::TagMgr) {
        bool isCtx = g_gtd.mode == GtdMode::ContextMgr;
        auto &list = isCtx ? g_gtd.contextList : g_gtd.tagList;
        int &sel = isCtx ? g_gtd.ctxSel : g_gtd.tagSel;
        if(key == 27 || key == 'q') { g_gtd.mode = GtdMode::List; render(); return; }
        if(key == KEY_UP || key == 'k') { if(sel > 0) sel--; render(); return; }
        if(key == KEY_DOWN || key == 'j') { if(sel < (int)list.size() - 1) sel++; render(); return; }
        if((key == 'a' || key == 'A')) {
            g_gtd.mode = isCtx ? GtdMode::AddContext : GtdMode::AddTag;
            render();
            return;
        }
        if((key == 'r' || key == 'R') && sel >= 0 && sel < (int)list.size()) {
            g_gtd.renameTarget = list[sel];
            g_gtd.mode = isCtx ? GtdMode::RenameContext : GtdMode::RenameTag;
            render();
            return;
        }
        if((key == 'd' || key == 'D') && sel >= 0 && sel < (int)list.size()) {
            g_gtd.renameTarget = list[sel];
            gtd_begin_confirm(isCtx ? "删除情境" : "删除标签",
                              (isCtx ? "@" : "#") + list[sel], isCtx ? GtdConfirm::Context : GtdConfirm::Tag,
                              g_gtd.mode);
            render();
            return;
        }
        return;
    }

    // ── 详情模式 ─────────────────────────────────────────────────────
    if(g_gtd.mode == GtdMode::Detail) {
        if(g_gtd.detailIdx < 0 || g_gtd.detailIdx >= (int)g_gtd.data["tasks"].size()) {
            g_gtd.mode = GtdMode::List;
            render();
            return;
        }
        if(key == 27 || key == 'q') { g_gtd.mode = GtdMode::List; g_gtd.detailIdx = -1; render(); return; }
        if(key == KEY_UP || key == 'k') { if(g_gtd.detailField > 0) g_gtd.detailField--; render(); return; }
        if(key == KEY_DOWN || key == 'j') {
            if(g_gtd.detailField < k_gtd_field_count - 1) g_gtd.detailField++;
            render();
            return;
        }
        if(key == '?' || key == 'h') { g_gtd.helpScroll = 0; g_gtd.helpPrev = GtdMode::Detail; g_gtd.mode = GtdMode::Help; render(); return; }
        if(key == 's' || key == 'S') { g_gtd.summaryScroll = 0; g_gtd.summaryPrev = GtdMode::Detail; g_gtd.mode = GtdMode::Summary; render(); return; }
        if(key == 'd' || key == 'D') {
            gtd_begin_confirm("删除任务", g_gtd.data["tasks"][g_gtd.detailIdx]["title"].asString(),
                              GtdConfirm::Task, GtdMode::List);
            render();
            return;
        }
        if(key == ' ') {
            gtd_cycle_status(g_gtd.data["tasks"][g_gtd.detailIdx]);
            gtd_save();
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            const GtdField &f = k_gtd_fields[g_gtd.detailField];
            if(f.type == 'p' || f.type == 't' || f.type == 'j' || f.type == 'c' || f.type == 'g') {
                gtd_open_picker(g_gtd.detailField);
                render();
                return;
            }
            if(f.type == 'd') { gtd_open_calendar(); render(); return; }
            if(f.type == 's') { g_gtd.mode = GtdMode::Rename; render(); return; }
            if(f.type == 'm') { g_gtd.mode = GtdMode::Note; render(); return; }
            if(f.type == 'n') {
                auto &task = g_gtd.data["tasks"][g_gtd.detailIdx];
                int p = task["progress"].asInt(0);
                p = p >= 100 ? 0 : (p / 10 + 1) * 10;
                task.set("progress", p);
                gtd_save();
                render();
                return;
            }
            return;
        }
        return;
    }

    if(g_gtd.mode != GtdMode::List) return;

    // ── 列表模式 ─────────────────────────────────────────────────────
    bool projectList = g_gtd.view == 3 && g_gtd.projectDrill < 0;
    if(key == 27 || key == 'q') {
        if(!g_gtd.multiSel.empty()) {
            g_gtd.multiSel.clear();
            g_gtd.multiAnchor = -1;
        } else if(g_gtd.view == 3 && g_gtd.projectDrill >= 0) {
            g_gtd.projectDrill = -1;
            g_gtd.sel = 0;
            g_gtd.scroll = 0;
            g_gtd.folded.clear();
            gtd_rebuild();
        } else {
            goto_screen(Screen::Main);
            return;
        }
        render();
        return;
    }
    if(key >= '1' && key <= '5') {
        g_gtd.view = key - '1';
        g_gtd.sel = 0;
        g_gtd.scroll = 0;
        g_gtd.projectDrill = -1;
        g_gtd.folded.clear();
        g_gtd.multiSel.clear();
        g_gtd.multiAnchor = -1;
        gtd_rebuild();
        render();
        return;
    }
    if(key == KEY_LEFT) { gtd_cycle_view(-1); render(); return; }
    if(key == KEY_RIGHT) { gtd_cycle_view(1); render(); return; }
    if(key == KEY_SHIFT_DOWN || key == KEY_SHIFT_UP) {
        int n = projectList ? (int)g_gtd.projectList.size() : (int)g_gtd.filtered.size();
        if(key == KEY_SHIFT_DOWN && g_gtd.sel < n - 1) {
            if(g_gtd.multiAnchor < 0) g_gtd.multiAnchor = g_gtd.sel;
            g_gtd.sel++;
        } else if(key == KEY_SHIFT_UP && g_gtd.sel > 0) {
            if(g_gtd.multiAnchor < 0) g_gtd.multiAnchor = g_gtd.sel;
            g_gtd.sel--;
        }
        g_gtd.multiSel.clear();
        int a = std::min(g_gtd.multiAnchor, g_gtd.sel);
        int b = std::max(g_gtd.multiAnchor, g_gtd.sel);
        if(projectList) {
            for(int i = a; i <= b && i < (int)g_gtd.projectList.size(); ++i)
                g_gtd.multiSel.insert(g_gtd.projectList[i]);
        } else {
            auto &tasks = g_gtd.data["tasks"];
            for(int i = a; i <= b && i < (int)g_gtd.filtered.size(); ++i)
                g_gtd.multiSel.insert(tasks[g_gtd.filtered[i]]["id"].asString());
        }
        render();
        return;
    }
    if(key == KEY_UP || key == KEY_DOWN) {
        g_gtd.multiSel.clear();
        g_gtd.multiAnchor = -1;
        int n = projectList ? (int)g_gtd.projectList.size() : (int)g_gtd.filtered.size();
        if(key == KEY_UP && g_gtd.sel > 0) g_gtd.sel--;
        if(key == KEY_DOWN && g_gtd.sel < n - 1) g_gtd.sel++;
        render();
        return;
    }
    if(key == '\t') {
        g_gtd.view = (g_gtd.view + 1) % 5;
        g_gtd.sel = 0;
        g_gtd.scroll = 0;
        g_gtd.projectDrill = -1;
        g_gtd.folded.clear();
        gtd_rebuild();
        render();
        return;
    }
    if(key == '?') {
        g_gtd.helpScroll = 0;
        g_gtd.helpPrev = GtdMode::List;
        g_gtd.mode = GtdMode::Help;
        render();
        return;
    }
    if(key == 'z' || key == 'Z') {
        if(gtd_in_project_drill()) {
            if(key == 'Z') {
                if((int)g_gtd.folded.size() < (int)g_gtd.treeOrder.size()) {
                    for(int i = 0; i < (int)g_gtd.treeDepth.size(); ++i)
                        if(gtd_has_children(i)) g_gtd.folded.insert(i);
                } else {
                    g_gtd.folded.clear();
                }
            } else if(g_gtd.sel < (int)g_gtd.visPos.size()) {
                int pos = g_gtd.visPos[g_gtd.sel];
                if(gtd_has_children(pos)) {
                    if(g_gtd.folded.count(pos)) g_gtd.folded.erase(pos);
                    else g_gtd.folded.insert(pos);
                }
            }
            gtd_rebuild_visible();
            gtd_clamp_sel();
        }
        render();
        return;
    }
    if(key == '/') {
        if(!g_gtd.filterText.empty()) {
            g_gtd.filterText.clear();
            gtd_rebuild();
            render();
            return;
        }
        g_gtd.mode = GtdMode::Filter;
        render();
        return;
    }
    if(key == 'c' || key == 'C') {
        gtd_build_context_list();
        g_gtd.ctxSel = 0;
        g_gtd.mode = GtdMode::ContextMgr;
        render();
        return;
    }
    if(key == 't' || key == 'T') {
        gtd_build_tag_list();
        g_gtd.tagSel = 0;
        g_gtd.mode = GtdMode::TagMgr;
        render();
        return;
    }
    if(key == 'A') {
        ensure_dir_path(gtd_archive_dir());
        gtd_load_archive_months();
        g_gtd.archiveSel = 0;
        g_gtd.archiveBrowsing = false;
        g_gtd.mode = GtdMode::Archive;
        render();
        return;
    }
    if(key == 'E') {
        std::string path = g_settings.journal_dir() + "/gtd-export.md";
        ensure_dir_path(g_settings.journal_dir());
        g_gtd.notice = safe_write_file(path, gtd_export_md()) ? "已导出 " + path : "导出失败";
        render();
        return;
    }
    if((key == 's' || key == 'S') && !projectList && !g_gtd.filtered.empty()) {
        g_gtd.summaryScroll = 0;
        g_gtd.summaryPrev = GtdMode::List;
        g_gtd.mode = GtdMode::Summary;
        render();
        return;
    }
    if((key == 'n' || key == 'N') && g_gtd.view == 3 && g_gtd.projectDrill < 0) {
        g_gtd.mode = GtdMode::AddProject;
        render();
        return;
    }
    if((key == 'r' || key == 'R') && g_gtd.view == 3 && g_gtd.projectDrill < 0) {
        if(g_gtd.sel >= 0 && g_gtd.sel < (int)g_gtd.projectList.size()) {
            g_gtd.projectDrill = g_gtd.sel;
            g_gtd.renameTarget = g_gtd.projectList[g_gtd.sel];
            g_gtd.mode = GtdMode::RenameProject;
            render();
            return;
        }
    }
    if((key == 'd' || key == 'D') && g_gtd.view == 3 && g_gtd.projectDrill < 0) {
        if(g_gtd.sel >= 0 && g_gtd.sel < (int)g_gtd.projectList.size()) {
            g_gtd.projectDrill = g_gtd.sel;
            gtd_begin_confirm("删除项目", g_gtd.projectList[g_gtd.sel], GtdConfirm::Project);
            render();
            return;
        }
    }
    if(key == '\n' || key == '\r') {
        if(projectList) {
            if(g_gtd.sel >= 0 && g_gtd.sel < (int)g_gtd.projectList.size()) {
                g_gtd.projectDrill = g_gtd.sel;
                g_gtd.sel = 0;
                g_gtd.scroll = 0;
                g_gtd.folded.clear();
                gtd_rebuild();
            }
        } else {
            gtd_open_detail();
        }
        render();
        return;
    }
    if(projectList) { render(); return; }

    if(key == 'a' || key == 'A') {
        g_gtd.pendingParent.clear();
        g_gtd.insertAfter = -1;
        g_gtd.pendingProject = gtd_in_project_drill() ? g_gtd.projectList[g_gtd.projectDrill] : std::string();
        g_gtd.mode = GtdMode::Add;
        render();
        return;
    }
    if((key == 'i' || key == 'I') && !g_gtd.filtered.empty()) {
        int idx = g_gtd.filtered[g_gtd.sel];
        g_gtd.pendingParent = g_gtd.data["tasks"][idx]["id"].asString();
        g_gtd.insertAfter = idx;
        g_gtd.pendingProject = gtd_in_project_drill() ? g_gtd.projectList[g_gtd.projectDrill]
                                                      : g_gtd.data["tasks"][idx]["project"].asString();
        g_gtd.mode = GtdMode::Add;
        render();
        return;
    }
    if((key == 'r' || key == 'R') && !g_gtd.filtered.empty()) {
        g_gtd.detailIdx = g_gtd.filtered[g_gtd.sel];
        g_gtd.mode = GtdMode::Rename;
        render();
        return;
    }
    if((key == 'd' || key == 'D') && !g_gtd.filtered.empty()) {
        if(!g_gtd.multiSel.empty()) {
            gtd_begin_confirm("批量删除", std::to_string((int)g_gtd.multiSel.size()) + " 个任务", GtdConfirm::Tasks);
        } else {
            g_gtd.detailIdx = g_gtd.filtered[g_gtd.sel];
            gtd_begin_confirm("删除任务", g_gtd.data["tasks"][g_gtd.detailIdx]["title"].asString(),
                              GtdConfirm::Task);
        }
        render();
        return;
    }
    if(key == ' ' && !g_gtd.filtered.empty()) {
        gtd_cycle_status(g_gtd.data["tasks"][g_gtd.filtered[g_gtd.sel]]);
        gtd_save();
        gtd_rebuild();
        render();
        return;
    }
    if(!g_gtd.filtered.empty() && g_gtd.view != 4) {
        auto &tasks = g_gtd.data["tasks"];
        int cur = g_gtd.filtered[g_gtd.sel];
        if((key == 'j' || key == 'J') && g_gtd.sel > 0) {
            int above = g_gtd.filtered[g_gtd.sel - 1];
            std::swap(tasks.elements[cur], tasks.elements[above]);
            g_gtd.sel--;
            gtd_save();
            gtd_rebuild();
        } else if((key == 'k' || key == 'K') && g_gtd.sel < (int)g_gtd.filtered.size() - 1) {
            int below = g_gtd.filtered[g_gtd.sel + 1];
            std::swap(tasks.elements[cur], tasks.elements[below]);
            g_gtd.sel++;
            gtd_save();
            gtd_rebuild();
        } else if(key == 'h' || key == 'H') {
            tasks[cur].set("parent", "");
            gtd_save();
            gtd_rebuild();
        } else if(key == 'l' || key == 'L') {
            if(g_gtd.sel > 0) {
                int prev = g_gtd.filtered[g_gtd.sel - 1];
                tasks[cur].set("parent", tasks[prev]["id"].asString());
                gtd_save();
                gtd_rebuild();
            }
        }
    }
    render();
}

static bool outline_input_key(int key, bool multiline) {
    if(!g_ol.input) return false;
    if(key == KEY_IME_TOGGLE) {
        g_linux_ime.toggle();
        update_ime_bar();
        return true;
    }
    std::string ime_out;
    if(g_linux_ime.handle_key(key, ime_out)) {
        if(!ime_out.empty()) lv_textarea_add_text(g_ol.input, ime_out.c_str());
        update_ime_bar();
        return true;
    }
    if(key == KEY_LEFT) { lv_textarea_cursor_left(g_ol.input); return true; }
    if(key == KEY_RIGHT) { lv_textarea_cursor_right(g_ol.input); return true; }
    if(key == KEY_UP) { if(multiline) lv_textarea_cursor_up(g_ol.input); return true; }
    if(key == KEY_DOWN) { if(multiline) lv_textarea_cursor_down(g_ol.input); return true; }
    if(key == KEY_HOME) { lv_textarea_set_cursor_pos(g_ol.input, 0); return true; }
    if(key == KEY_END) { lv_textarea_set_cursor_pos(g_ol.input, LV_TEXTAREA_CURSOR_LAST); return true; }
    if(key == 8) { lv_textarea_delete_char(g_ol.input); return true; }
    if(key == 127) { lv_textarea_delete_char_forward(g_ol.input); return true; }
    if(key == '\n' || key == '\r') {
        if(multiline) { lv_textarea_add_char(g_ol.input, '\n'); return true; }
        return false;
    }
    if(key >= 32 && key < 127) { lv_textarea_add_char(g_ol.input, (uint32_t)key); return true; }
    return false;
}

static std::string outline_input_text() {
    return g_ol.input ? lv_textarea_get_text(g_ol.input) : "";
}

static void outline_open_project(const std::string &name) {
    outline_load_project(name);
    outline_rebuild_filter();
    g_ol.mode = OutlineMode::Browse;
    g_ol.sel = 0;
    g_ol.scroll = 0;
    g_ol.folded.clear();
}

static void outline_back_to_projects() {
    g_ol.mode = OutlineMode::Projects;
    g_ol.scroll = 0;
    outline_list_projects();
    g_ol.sel = 0;
    for(int i = 0; i < (int)g_ol.projects.size(); ++i)
        if(g_ol.projects[i] == g_ol.project) { g_ol.sel = i; break; }
}

// 明细化路径:把标题变成同名内容文件,交给编辑器打开。
static void outline_edit_contents(int idx) {
    auto &nodes = outline_nodes();
    if(idx < 0 || idx >= (int)nodes.size()) return;
    std::string file = nodes[idx]["file"].asString();
    if(file.empty()) {
        file = outline_safe_filename(nodes[idx]["title"].asString());
        nodes[idx].set("file", file);
        outline_save();
    }
    g_ol.editPath = outline_ensure_content_file(file);
    g_prompt.clear();
    g_quick_slot = -1;
    g_file_edit_path.clear();
    g_edit_file.clear();
    goto_screen(Screen::Editor);
}

static void outline_commit_input() {
    std::string text = outline_input_text();
    OutlineMode m = g_ol.mode;
    if(m == OutlineMode::AddProject) {
        if(!text.empty() && utf8_valid(text) && safe_project_name(text)) {
            ensure_dir_path(outline_project_dir(text));
            JsonValue data = JsonValue::object();
            data.set("nodes", JsonValue::array());
            data.set("bookmarks", JsonValue::array());
            data.set("tags", JsonValue::array());
            JsonValue::saveToFile(outline_project_file(text), data);
            g_ol.project = text;
            outline_open_project(text);
        } else {
            g_ol.mode = g_ol.project.empty() ? OutlineMode::Projects : OutlineMode::Browse;
        }
    } else if(m == OutlineMode::AddHeading || m == OutlineMode::AddSub) {
        if(!text.empty() && utf8_valid(text)) {
            auto &nodes = outline_nodes();
            int level = g_ol.pendingLevel;
            int cur = outline_cur_node();
            if(m == OutlineMode::AddSub && cur >= 0) level = nodes[cur]["level"].asInt(0) + 1;
            JsonValue node = JsonValue::object();
            node.set("id", make_id());
            node.set("title", text);
            node.set("level", level);
            node.set("file", "");
            node.set("note", "");
            node.set("keywords", "");
            node.set("status", "draft");
            node.set("tags", JsonValue::array());
            int newIdx;
            if(g_ol.insertAfter >= 0 && g_ol.insertAfter < (int)nodes.size()) {
                nodes.elements.insert(nodes.elements.begin() + g_ol.insertAfter + 1, node);
                newIdx = g_ol.insertAfter + 1;
            } else {
                nodes.pushBack(node);
                newIdx = (int)nodes.size() - 1;
            }
            outline_save();
            outline_rebuild_filter();
            for(int i = 0; i < (int)g_ol.filtered.size(); ++i)
                if(g_ol.filtered[i] == newIdx) { g_ol.sel = i; break; }
        }
        g_ol.insertAfter = -1;
        g_ol.mode = OutlineMode::Browse;
    } else if(m == OutlineMode::EditText) {
        int idx = g_ol.editIdx;
        auto &nodes = outline_nodes();
        if(idx >= 0 && idx < (int)nodes.size()) {
            if(g_ol.editingTitle) nodes[idx].set("title", text);
            else if(g_ol.editingKeyword) nodes[idx].set("keywords", text);
            else nodes[idx].set("note", text);
            outline_save();
        }
        g_ol.detailIdx = idx;
        g_ol.mode = OutlineMode::Detail;
    } else if(m == OutlineMode::AddTag || m == OutlineMode::RenameTag) {
        std::string name = text;
        if(!name.empty() && name[0] == '#') name = name.substr(1);
        if(!name.empty()) {
            if(m == OutlineMode::AddTag) {
                g_ol.data["tags"].pushBack(name);
            } else {
                auto &arr = g_ol.data["tags"];
                for(int i = 0; i < (int)arr.size(); ++i)
                    if(arr[i].asString() == g_ol.renameTag) arr.elements[i] = JsonValue(name);
                auto &nodes = outline_nodes();
                for(int i = 0; i < (int)nodes.size(); ++i) {
                    auto &tt = nodes[i]["tags"];
                    if(tt.isArray())
                        for(int j = 0; j < (int)tt.size(); ++j)
                            if(tt[j].asString() == g_ol.renameTag) tt.elements[j] = JsonValue(name);
                }
            }
            outline_save();
            outline_build_tags();
        }
        g_ol.mode = OutlineMode::TagMgr;
    }
    g_ol.editBuf.clear();
    render();
}

static void outline_open_picker(int field) {
    g_ol.pickerVal.clear();
    g_ol.pickerDisp.clear();
    g_ol.pickerField = field;
    g_ol.pickerSel = 0;
    auto &node = outline_nodes()[g_ol.detailIdx];
    if(field == 1) {
        for(int i = 0; i < k_outline_status_count; ++i) {
            g_ol.pickerVal.push_back(k_outline_status[i]);
            g_ol.pickerDisp.push_back(k_outline_status_disp[i]);
        }
        g_ol.pickerSel = outline_status_index(node["status"].asString("draft"));
    } else if(field == 4) {
        outline_build_tags();
        for(auto &t : g_ol.tagList) {
            g_ol.pickerVal.push_back(t);
            g_ol.pickerDisp.push_back("#" + t);
        }
        g_ol.pickerToggled.clear();
        auto &tt = node["tags"];
        if(tt.isArray())
            for(int i = 0; i < (int)tt.size(); ++i)
                for(int j = 0; j < (int)g_ol.pickerVal.size(); ++j)
                    if(g_ol.pickerVal[j] == tt[i].asString()) { g_ol.pickerToggled.insert(j); break; }
    }
    g_ol.mode = OutlineMode::Picker;
}

static void outline_apply_confirm() {
    auto &nodes = outline_nodes();
    if(g_ol.confirmAction == 1 && g_ol.confirmIdx >= 0 && g_ol.confirmIdx < (int)nodes.size()) {
        outline_remove_file(g_ol.project, nodes[g_ol.confirmIdx]["file"].asString());
        nodes.elements.erase(nodes.elements.begin() + g_ol.confirmIdx);
        outline_save();
        outline_rebuild_filter();
        outline_scroll_into(g_ol.sel, g_ol.scroll, (int)g_ol.filtered.size(), 9);
    } else if(g_ol.confirmAction == 2 && g_ol.confirmIdx >= 0 && g_ol.confirmIdx < (int)g_ol.projects.size()) {
        std::string name = g_ol.projects[g_ol.confirmIdx];
        outline_delete_project_dir(name);
        if(name == g_ol.project) {
            g_ol.project.clear();
            g_ol.data = JsonValue();
            outline_normalize();
        }
        outline_list_projects();
        g_ol.sel = 0;
        g_ol.scroll = 0;
    } else if(g_ol.confirmAction == 3 && g_ol.confirmIdx >= 0 && g_ol.confirmIdx < (int)nodes.size()) {
        outline_remove_file(g_ol.project, nodes[g_ol.confirmIdx]["file"].asString());
        nodes[g_ol.confirmIdx].set("file", "");
        outline_save();
    }
    g_ol.mode = (g_ol.confirmAction == 2) ? OutlineMode::Projects : OutlineMode::Browse;
}

static void handle_outline(int key) {
    // ── 单行文本输入 ─────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::AddProject || g_ol.mode == OutlineMode::AddHeading ||
       g_ol.mode == OutlineMode::AddSub || g_ol.mode == OutlineMode::EditText ||
       g_ol.mode == OutlineMode::AddTag || g_ol.mode == OutlineMode::RenameTag) {
        OutlineMode m = g_ol.mode;
        if(outline_input_key(key, false)) return;
        if(key == 27) {
            if(m == OutlineMode::AddTag || m == OutlineMode::RenameTag) g_ol.mode = OutlineMode::TagMgr;
            else if(m == OutlineMode::EditText) { g_ol.detailIdx = g_ol.editIdx; g_ol.mode = OutlineMode::Detail; }
            else g_ol.mode = g_ol.project.empty() ? OutlineMode::Projects : OutlineMode::Browse;
            g_ol.insertAfter = -1;
            render();
            return;
        }
        if(key == '\n' || key == '\r') { outline_commit_input(); return; }
        return;
    }

    // ── 筛选 ─────────────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::Filter) {
        if(key == 27) {
            if(!g_ol.filterText.empty()) { g_ol.filterText.clear(); outline_rebuild_filter(); }
            else g_ol.mode = OutlineMode::Browse;
            render();
            return;
        }
        if(key == KEY_IME_TOGGLE) {
            g_linux_ime.toggle();
            update_ime_bar();
            render();
            return;
        }
        std::string ime_out;
        if(g_linux_ime.handle_key(key, ime_out)) {
            if(!ime_out.empty()) { g_ol.filterText += ime_out; outline_rebuild_filter(); }
            update_ime_bar();
            render();
            return;
        }
        if(key == 8 || key == 127) {
            if(!g_ol.filterText.empty()) {
                size_t len = g_ol.filterText.size();
                while(len > 1 && ((unsigned char)g_ol.filterText[len - 1] & 0xC0) == 0x80) len--;
                g_ol.filterText.erase(len - 1);
                outline_rebuild_filter();
            }
            render();
            return;
        }
        if(key == '\n' || key == '\r') { g_ol.mode = OutlineMode::Browse; render(); return; }
        if(key >= 32 && key < 127) { g_ol.filterText += (char)key; outline_rebuild_filter(); render(); return; }
        render();
        return;
    }

    // ── 选择浮层(状态/标签) ─────────────────────────────────────────
    if(g_ol.mode == OutlineMode::Picker) {
        if(key == 27 || key == 'q' || key == 'Q') { g_ol.mode = OutlineMode::Detail; render(); return; }
        if(key == KEY_UP && g_ol.pickerSel > 0) g_ol.pickerSel--;
        if(key == KEY_DOWN && g_ol.pickerSel < (int)g_ol.pickerVal.size() - 1) g_ol.pickerSel++;
        if(key == '\n' || key == '\r' || key == ' ') {
            if(!g_ol.pickerVal.empty() && g_ol.detailIdx >= 0 && g_ol.detailIdx < (int)outline_nodes().size()) {
                auto &node = outline_nodes()[g_ol.detailIdx];
                if(g_ol.pickerField == 1) {
                    node.set("status", g_ol.pickerVal[g_ol.pickerSel]);
                    outline_save();
                    g_ol.mode = OutlineMode::Detail;
                } else if(g_ol.pickerField == 4) {
                    if(g_ol.pickerToggled.count(g_ol.pickerSel)) g_ol.pickerToggled.erase(g_ol.pickerSel);
                    else g_ol.pickerToggled.insert(g_ol.pickerSel);
                }
            }
        } else if(key == 'y' || key == 'Y') {
            if(g_ol.pickerField == 4 && g_ol.detailIdx >= 0 && g_ol.detailIdx < (int)outline_nodes().size()) {
                JsonValue tags = JsonValue::array();
                for(int idx : g_ol.pickerToggled)
                    if(idx >= 0 && idx < (int)g_ol.pickerVal.size()) tags.pushBack(g_ol.pickerVal[idx]);
                outline_nodes()[g_ol.detailIdx].set("tags", tags);
                outline_save();
                g_ol.mode = OutlineMode::Detail;
            }
        }
        render();
        return;
    }

    // ── 标签管理 ─────────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::TagMgr) {
        int total = (int)g_ol.tagList.size();
        if(key == 27 || key == 'q' || key == 'Q') { g_ol.mode = OutlineMode::Browse; render(); return; }
        if(key == KEY_UP || key == 'k') { if(g_ol.tagSel > 0) g_ol.tagSel--; }
        if(key == KEY_DOWN || key == 'j') { if(g_ol.tagSel < total - 1) g_ol.tagSel++; }
        if(key == 'a' || key == 'A') { g_ol.editBuf.clear(); g_ol.mode = OutlineMode::AddTag; render(); return; }
        if((key == 'd' || key == 'D') && g_ol.tagSel >= 0 && g_ol.tagSel < total) {
            std::string name = g_ol.tagList[g_ol.tagSel];
            auto &arr = g_ol.data["tags"];
            for(int i = (int)arr.size() - 1; i >= 0; --i)
                if(arr[i].asString() == name) arr.elements.erase(arr.elements.begin() + i);
            auto &nodes = outline_nodes();
            for(int i = 0; i < (int)nodes.size(); ++i) {
                auto &tt = nodes[i]["tags"];
                if(!tt.isArray()) continue;
                for(int j = (int)tt.size() - 1; j >= 0; --j)
                    if(tt[j].asString() == name) tt.elements.erase(tt.elements.begin() + j);
            }
            outline_save();
            outline_build_tags();
            if(g_ol.tagSel >= (int)g_ol.tagList.size()) g_ol.tagSel = (int)g_ol.tagList.size() - 1;
            if(g_ol.tagSel < 0) g_ol.tagSel = 0;
        } else if((key == 'r' || key == 'R') && g_ol.tagSel >= 0 && g_ol.tagSel < total) {
            g_ol.renameTag = g_ol.tagList[g_ol.tagSel];
            g_ol.editBuf = g_ol.renameTag;
            g_ol.mode = OutlineMode::RenameTag;
            render();
            return;
        } else if((key == '\n' || key == '\r') && g_ol.tagSel >= 0 && g_ol.tagSel < total) {
            std::string name = g_ol.tagList[g_ol.tagSel];
            bool found = false;
            for(int i = 0; i < (int)g_ol.filterTags.size(); ++i)
                if(g_ol.filterTags[i] == name) { g_ol.filterTags.erase(g_ol.filterTags.begin() + i); found = true; break; }
            if(!found) g_ol.filterTags.push_back(name);
            outline_rebuild_filter();
        }
        render();
        return;
    }

    // ── 摘要 ─────────────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::Summary) {
        if(key == 27 || key == 'q' || key == 'Q') { g_ol.mode = OutlineMode::Browse; render(); return; }
        if(key == KEY_UP) { if(g_ol.summaryScroll > 0) g_ol.summaryScroll--; }
        if(key == KEY_DOWN) g_ol.summaryScroll++;
        render();
        return;
    }

    // ── 帮助 ─────────────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::Help) {
        if(key == 27 || key == 'q' || key == 'Q' || key == '\n' || key == '\r') {
            g_ol.mode = g_ol.helpPrev;
            render();
            return;
        }
        if(key == KEY_UP) { if(g_ol.helpScroll > 0) g_ol.helpScroll--; }
        if(key == KEY_DOWN) g_ol.helpScroll++;
        if(key == KEY_LEFT) g_ol.helpScroll = g_ol.helpScroll > 5 ? g_ol.helpScroll - 5 : 0;
        if(key == KEY_RIGHT) g_ol.helpScroll += 5;
        render();
        return;
    }

    // ── 书签管理 ─────────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::BookmarkMgr) {
        auto &bm = g_ol.data["bookmarks"];
        int bmCount = bm.isArray() ? (int)bm.size() : 0;
        if(key == 27 || key == 'q' || key == 'Q') { g_ol.mode = OutlineMode::Browse; render(); return; }
        if(key == KEY_UP || key == 'k') { if(g_ol.bmSel > 0) g_ol.bmSel--; }
        if(key == KEY_DOWN || key == 'j') { if(g_ol.bmSel < bmCount - 1) g_ol.bmSel++; }
        if((key == '\n' || key == '\r') && g_ol.bmSel >= 0 && g_ol.bmSel < bmCount) {
            std::string bid = bm[g_ol.bmSel]["id"].asString();
            auto &nodes = outline_nodes();
            for(int i = 0; i < (int)nodes.size(); ++i) {
                if(nodes[i]["id"].asString() != bid) continue;
                g_ol.folded.clear();
                outline_rebuild_filter();
                for(int fi = 0; fi < (int)g_ol.filtered.size(); ++fi)
                    if(g_ol.filtered[fi] == i) { g_ol.sel = fi; break; }
                g_ol.mode = OutlineMode::Browse;
                break;
            }
        } else if((key == 'd' || key == 'D') && g_ol.bmSel >= 0 && g_ol.bmSel < bmCount) {
            bm.elements.erase(bm.elements.begin() + g_ol.bmSel);
            if(g_ol.bmSel >= bmCount - 1) g_ol.bmSel = bmCount - 2;
            if(g_ol.bmSel < 0) g_ol.bmSel = 0;
            outline_save();
        }
        render();
        return;
    }

    // ── 确认对话框 ───────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::Confirm) {
        if(key == '\n' || key == '\r') { outline_apply_confirm(); render(); return; }
        if(key == 27) {
            g_ol.mode = (g_ol.confirmAction == 2) ? OutlineMode::Projects : OutlineMode::Browse;
            render();
            return;
        }
        render();
        return;
    }

    // ── 节点详情面板 ─────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::Detail) {
        auto &nodes = outline_nodes();
        if(g_ol.detailIdx < 0 || g_ol.detailIdx >= (int)nodes.size()) { g_ol.mode = OutlineMode::Browse; render(); return; }
        auto &node = nodes[g_ol.detailIdx];
        if(key == 27 || key == 'q' || key == 'Q') { g_ol.mode = OutlineMode::Browse; render(); return; }
        if(key == KEY_UP && g_ol.detailField > 0) g_ol.detailField--;
        if(key == KEY_DOWN && g_ol.detailField < 4) g_ol.detailField++;
        if(key == '\n' || key == '\r') {
            if(g_ol.detailField == 0 || g_ol.detailField == 2 || g_ol.detailField == 3) {
                g_ol.editIdx = g_ol.detailIdx;
                g_ol.editingTitle = (g_ol.detailField == 0);
                g_ol.editingKeyword = (g_ol.detailField == 2);
                g_ol.editBuf = g_ol.editingTitle ? node["title"].asString()
                               : (g_ol.editingKeyword ? node["keywords"].asString() : node["note"].asString());
                g_ol.mode = OutlineMode::EditText;
            } else {
                outline_open_picker(g_ol.detailField);
            }
        } else if(key == 's' || key == 'S') {
            g_ol.summaryIdx = g_ol.detailIdx;
            g_ol.summaryScroll = 0;
            g_ol.mode = OutlineMode::Summary;
        } else if(key == 'f' || key == 'F') {
            outline_edit_contents(g_ol.detailIdx);
            return;
        } else if(key == '?') {
            g_ol.helpScroll = 0;
            g_ol.helpPrev = OutlineMode::Detail;
            g_ol.mode = OutlineMode::Help;
        }
        render();
        return;
    }

    // ── 项目列表 ─────────────────────────────────────────────────────
    if(g_ol.mode == OutlineMode::Projects) {
        int total = (int)g_ol.projects.size();
        if(key == 'q' || key == 'Q' || key == 27) { goto_screen(Screen::Main); return; }
        if(key == 'j' || key == KEY_DOWN) { if(g_ol.sel < total - 1) g_ol.sel++; }
        if(key == 'k' || key == KEY_UP) { if(g_ol.sel > 0) g_ol.sel--; }
        if(key == 'n' || key == 'N') { g_ol.editBuf.clear(); g_ol.mode = OutlineMode::AddProject; render(); return; }
        if((key == '\n' || key == '\r') && g_ol.sel >= 0 && g_ol.sel < total) {
            outline_open_project(g_ol.projects[g_ol.sel]);
            render();
            return;
        }
        if((key == 'd' || key == 'D') && g_ol.sel >= 0 && g_ol.sel < total) {
            g_ol.confirmAction = 2;
            g_ol.confirmIdx = g_ol.sel;
            g_ol.confirmMsg = "删除项目「" + g_ol.projects[g_ol.sel] + "」?";
            g_ol.mode = OutlineMode::Confirm;
        }
        if(key == '?') {
            g_ol.helpScroll = 0;
            g_ol.helpPrev = OutlineMode::Projects;
            g_ol.mode = OutlineMode::Help;
        }
        render();
        return;
    }

    // ── 大纲树 ───────────────────────────────────────────────────────
    {
        auto &nodes = outline_nodes();
        int total = (int)g_ol.filtered.size();
        if(key == 'q' || key == 'Q' || key == 27) {
            if(!g_ol.filterTags.empty()) { g_ol.filterTags.clear(); outline_rebuild_filter(); }
            else if(!g_ol.filterText.empty()) { g_ol.filterText.clear(); outline_rebuild_filter(); }
            else outline_back_to_projects();
            render();
            return;
        }
        if(key == '\t') {
            if(!g_ol.projects.empty()) {
                int cur = 0;
                for(int i = 0; i < (int)g_ol.projects.size(); ++i)
                    if(g_ol.projects[i] == g_ol.project) { cur = i; break; }
                outline_open_project(g_ol.projects[(cur + 1) % (int)g_ol.projects.size()]);
            }
            render();
            return;
        }
        if(key == KEY_UP) { if(g_ol.sel > 0) g_ol.sel--; }
        if(key == KEY_DOWN) { if(g_ol.sel < total - 1) g_ol.sel++; }

        int idx = outline_cur_node();
        bool plain = g_ol.filterText.empty() && g_ol.filterTags.empty();

        // j/k 上下移动、h/l 升降层级(仅在不筛选时)
        if(plain && idx >= 0 && idx < (int)nodes.size()) {
            auto &node = nodes[idx];
            if(key == 'j' && idx > 0) {
                std::swap(nodes[idx], nodes[idx - 1]);
                g_ol.sel--;
                outline_save();
                outline_rebuild_filter();
            } else if(key == 'k' && idx < (int)nodes.size() - 1) {
                std::swap(nodes[idx], nodes[idx + 1]);
                g_ol.sel++;
                outline_save();
                outline_rebuild_filter();
            } else if(key == 'h') {
                int lvl = node["level"].asInt(0);
                if(lvl > 0) { node.set("level", lvl - 1); outline_save(); }
            } else if(key == 'l') {
                int lvl = node["level"].asInt(0);
                if(idx > 0 && lvl <= nodes[idx - 1]["level"].asInt(0)) { node.set("level", lvl + 1); outline_save(); }
            }
        }

        if(key == 'a' || key == 'A') {
            if(idx >= 0 && idx < (int)nodes.size()) {
                g_ol.pendingLevel = nodes[idx]["level"].asInt(0);
                int insertPos = idx + 1;
                while(insertPos < (int)nodes.size() && nodes[insertPos]["level"].asInt(0) > g_ol.pendingLevel) insertPos++;
                g_ol.insertAfter = insertPos - 1;
            } else {
                g_ol.pendingLevel = 0;
                g_ol.insertAfter = -1;
            }
            g_ol.editBuf.clear();
            g_ol.mode = OutlineMode::AddHeading;
            render();
            return;
        }
        if(key == 'i' || key == 'I') {
            g_ol.insertAfter = (idx >= 0 && idx < (int)nodes.size()) ? idx : -1;
            g_ol.editBuf.clear();
            g_ol.mode = (int)nodes.size() == 0 ? OutlineMode::AddHeading : OutlineMode::AddSub;
            render();
            return;
        }
        if(key == '/' || key == KEY_SEARCH) { g_ol.mode = OutlineMode::Filter; render(); return; }
        if(key == 'r' || key == 'R') {
            if(idx >= 0 && idx < (int)nodes.size()) {
                g_ol.editIdx = idx;
                g_ol.editingTitle = true;
                g_ol.editingKeyword = false;
                g_ol.editBuf = nodes[idx]["title"].asString();
                g_ol.mode = OutlineMode::EditText;
                render();
                return;
            }
        }
        if((key == 's' || key == 'S') && idx >= 0) {
            g_ol.summaryIdx = idx;
            g_ol.summaryScroll = 0;
            g_ol.mode = OutlineMode::Summary;
            render();
            return;
        }
        if(key == 0x05) {  // Ctrl+E 导出 Markdown
            std::string md = outline_export_md();
            time_t now = time(nullptr);
            char name[64];
            strftime(name, sizeof(name), "export_%Y%m%d_%H%M%S.md", localtime(&now));
            ensure_dir_path(outline_dir());
            bool ok = safe_write_file(outline_dir() + "/" + name, md);
            set_status(ok ? std::string("已导出 ") + name : "导出失败");
            render();
            return;
        }
        if((key == 'd' || key == 'D') && idx >= 0) {
            g_ol.confirmAction = 1;
            g_ol.confirmIdx = idx;
            g_ol.confirmMsg = "删除标题「" + nodes[idx]["title"].asString() + "」?";
            g_ol.mode = OutlineMode::Confirm;
            render();
            return;
        }
        if((key == 'c' || key == 'C') && idx >= 0 && !nodes[idx]["file"].asString().empty()) {
            g_ol.confirmAction = 3;
            g_ol.confirmIdx = idx;
            g_ol.confirmMsg = "清除「" + nodes[idx]["title"].asString() + "」的文件关联?";
            g_ol.mode = OutlineMode::Confirm;
            render();
            return;
        }
        if(key == 'n' || key == 'N') { g_ol.editBuf.clear(); g_ol.mode = OutlineMode::AddProject; render(); return; }
        if(key == '\n' || key == '\r') {
            if(idx >= 0) {
                g_ol.detailIdx = idx;
                g_ol.detailField = 0;
                g_ol.mode = OutlineMode::Detail;
            }
            render();
            return;
        }
        if(key == 'f' || key == 'F') {
            if(idx >= 0) { outline_edit_contents(idx); return; }
        }
        if(key == '?') {
            g_ol.helpScroll = 0;
            g_ol.helpPrev = OutlineMode::Browse;
            g_ol.mode = OutlineMode::Help;
            render();
            return;
        }
        if(key == 'z' && idx >= 0) {
            if(g_ol.folded.count(idx)) g_ol.folded.erase(idx);
            else g_ol.folded.insert(idx);
            outline_rebuild_filter();
        }
        if(key == 'Z') {
            if(g_ol.folded.empty()) {
                for(int i = 0; i < (int)nodes.size(); ++i)
                    if(outline_has_children(i)) g_ol.folded.insert(i);
            } else {
                g_ol.folded.clear();
            }
            outline_rebuild_filter();
        }
        if((key == 'm' || key == 'M') && idx >= 0) {
            auto &bm = g_ol.data["bookmarks"];
            std::string nid = nodes[idx]["id"].asString();
            bool found = false;
            for(int i = 0; i < (int)bm.size(); ++i)
                if(bm[i]["id"].asString() == nid) { bm.elements.erase(bm.elements.begin() + i); found = true; break; }
            if(!found) {
                JsonValue item = JsonValue::object();
                item.set("id", nid);
                item.set("title", nodes[idx]["title"].asString());
                item.set("level", nodes[idx]["level"].asInt(0));
                bm.pushBack(item);
            }
            outline_save();
        }
        if(key == 't' || key == 'T') {
            outline_build_tags();
            g_ol.tagSel = 0;
            g_ol.scroll = 0;
            g_ol.mode = OutlineMode::TagMgr;
            render();
            return;
        }
        if(key == 'b' || key == 'B') {
            g_ol.bmSel = 0;
            g_ol.scroll = 0;
            g_ol.mode = OutlineMode::BookmarkMgr;
            render();
            return;
        }
        render();
    }
}

static void handle_sync(int key) {
    if(key == 'q' || key == 27) { goto_screen(Screen::Main); return; }
    if(key == '\n' || key == '\r') {
        g_sync_message = "同步中...";
        g_sync_done = false;
        render();
        lv_timer_handler();
        WebdavSyncResult r = webdav_sync_journal();
        g_sync_done = true;
        g_sync_message = std::string(r.success ? "同步成功: " : "同步失败: ") + r.message;
        render();
        return;
    }
    render();
}

static void handle_wifi(int key) {
    if(key == 'q' || key == 27) { goto_screen(Screen::Settings); return; }
    if(key == '\t') {
        g_wifi_saved_mode = !g_wifi_saved_mode;
        g_wifi_entries.clear();
        g_wifi_saved_entries.clear();
        render();
        return;
    }
    if(key == 'r' || key == 'R') {
        if(g_wifi_saved_mode) g_wifi_saved_entries = g_wifi.list_networks();
        else g_wifi_entries = g_wifi.scan();
        render();
        return;
    }
    if(key == 'x' || key == 'X') { std::string m; g_wifi.disconnect(m); render(); return; }
    if(key == 'c' || key == 'C') { std::string m; g_wifi.reconnect(m); render(); return; }
    if(g_wifi_saved_mode) {
        if(key == KEY_DOWN || key == 'j') g_wifi_saved_sel++;
        if(key == KEY_UP || key == 'k') g_wifi_saved_sel--;
        if(g_wifi_saved_sel < 0) g_wifi_saved_sel = 0;
        if(g_wifi_saved_sel >= (int)g_wifi_saved_entries.size()) g_wifi_saved_sel = (int)g_wifi_saved_entries.size() - 1;
        if((key == '\n' || key == '\r') && !g_wifi_saved_entries.empty()) {
            std::string m;
            g_wifi.select_network(g_wifi_saved_entries[g_wifi_saved_sel].id, m);
            render();
            return;
        }
        if((key == 'd' || key == 'D') && !g_wifi_saved_entries.empty()) {
            std::string m;
            g_wifi.remove_network(g_wifi_saved_entries[g_wifi_saved_sel].id, m);
            g_wifi_saved_entries = g_wifi.list_networks();
            render();
            return;
        }
        render();
        return;
    }
    if(key == KEY_DOWN || key == 'j') g_wifi_sel++;
    if(key == KEY_UP || key == 'k') g_wifi_sel--;
    if(g_wifi_sel < 0) g_wifi_sel = 0;
    if(g_wifi_sel >= (int)g_wifi_entries.size()) g_wifi_sel = (int)g_wifi_entries.size() - 1;
    if((key == '\n' || key == '\r') && !g_wifi_entries.empty()) {
        std::string m;
        g_wifi.connect_psk(g_wifi_entries[g_wifi_sel].ssid, g_wifi_password ? lv_textarea_get_text(g_wifi_password) : "", m);
        g_wifi_saved_entries.clear();
        render();
        return;
    }
    render();
}

static bool handle_textarea_key(lv_obj_t *ta, int key, bool multiline) {
    if(!ta) return false;
    if(key == KEY_LEFT) lv_textarea_cursor_left(ta);
    else if(key == KEY_RIGHT) lv_textarea_cursor_right(ta);
    else if(key == KEY_UP && multiline) lv_textarea_cursor_up(ta);
    else if(key == KEY_DOWN && multiline) lv_textarea_cursor_down(ta);
    else if(key == KEY_HOME) lv_textarea_set_cursor_pos(ta, 0);
    else if(key == KEY_END) lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    else if(key == 8 || key == 127) lv_textarea_delete_char(ta);
    else if((key == '\n' || key == '\r') && multiline) lv_textarea_add_char(ta, '\n');
    else if(key >= 32 && key < 127) lv_textarea_add_char(ta, (uint32_t)key);
    else return false;
    update_ime_bar();
    return true;
}

static void handle_inspiration(int key) {
    if(g_inspiration_mode != InspirationMode::List) {
        if(key == KEY_IME_TOGGLE) {
            g_linux_ime.toggle();
            update_ime_bar();
            return;
        }
        std::string ime_out;
        if(g_linux_ime.handle_key(key, ime_out)) {
            if(!ime_out.empty() && g_inspiration_text) lv_textarea_add_text(g_inspiration_text, ime_out.c_str());
            update_ime_bar();
            return;
        }
        if(key == 27) {
            g_inspiration_mode = InspirationMode::List;
            g_inspiration_edit_idx = -1;
            render();
            return;
        }
        bool editing_content = g_inspiration_mode == InspirationMode::EditContent;
        if(key == 0x13 || (!editing_content && (key == '\n' || key == '\r'))) {
            std::string value = g_inspiration_text ? lv_textarea_get_text(g_inspiration_text) : "";
            if(!utf8_valid(value)) {
                set_status("UTF-8无效，未保存");
                return;
            }
            if(g_inspiration_mode == InspirationMode::EditContent) {
                inspiration_normalize();
                if(g_inspiration_edit_idx >= 0 && g_inspiration_edit_idx < (int)g_inspiration_data["items"].size()) {
                    g_inspiration_data["items"][g_inspiration_edit_idx].set("content", value);
                } else if(!value.empty()) {
                    JsonValue item = JsonValue::object();
                    item.set("id", make_id());
                    item.set("content", value);
                    item.set("keywords", "");
                    g_inspiration_data["items"].pushBack(item);
                    g_inspiration_sel = (int)g_inspiration_data["items"].size() - 1;
                }
                inspiration_save();
            } else if(g_inspiration_mode == InspirationMode::EditKeywords) {
                if(g_inspiration_edit_idx >= 0 && g_inspiration_edit_idx < (int)g_inspiration_data["items"].size()) {
                    g_inspiration_data["items"][g_inspiration_edit_idx].set("keywords", value);
                    inspiration_save();
                }
            } else if(g_inspiration_mode == InspirationMode::Search) {
                g_inspiration_query = value;
                g_inspiration_sel = 0;
                g_inspiration_scroll = 0;
            }
            g_inspiration_mode = InspirationMode::List;
            g_inspiration_edit_idx = -1;
            inspiration_rebuild();
            render();
            return;
        }
        if(handle_textarea_key(g_inspiration_text, key, editing_content)) return;
        return;
    }

    int total = (int)g_inspiration_filtered.size();
    if(key == 'q' || key == 'Q' || key == 27) { goto_screen(Screen::Main); return; }
    if((key == KEY_DOWN || key == 'j') && g_inspiration_sel < total - 1) g_inspiration_sel++;
    if(key == KEY_UP && g_inspiration_sel > 0) g_inspiration_sel--;
    if(key == 'a' || key == 'A') {
        g_inspiration_mode = InspirationMode::EditContent;
        g_inspiration_edit_idx = -1;
        render();
        return;
    }
    if(key == '/' || key == KEY_SEARCH) {
        g_inspiration_mode = InspirationMode::Search;
        g_inspiration_edit_idx = -1;
        render();
        return;
    }
    int idx = inspiration_selected_item_index();
    if((key == '\n' || key == '\r') && idx >= 0) {
        g_inspiration_mode = InspirationMode::EditContent;
        g_inspiration_edit_idx = idx;
        render();
        return;
    }
    if((key == 'k' || key == 'K') && idx >= 0) {
        g_inspiration_mode = InspirationMode::EditKeywords;
        g_inspiration_edit_idx = idx;
        render();
        return;
    }
    if((key == 'c' || key == 'C') && idx >= 0) {
        g_clipboard = g_inspiration_data["items"][idx]["content"].asString();
        set_status("已复制到内部剪贴板");
        return;
    }
    if((key == 'd' || key == 'D') && idx >= 0) {
        g_inspiration_data["items"].elements.erase(g_inspiration_data["items"].elements.begin() + idx);
        inspiration_save();
        inspiration_rebuild();
        render();
        return;
    }
    render();
}

static void handle_dict(int key) {
    IME &ime = IME::getInstance();

    if(g_dict_mode == DictMode::Add || g_dict_mode == DictMode::Search) {
        bool adding = g_dict_mode == DictMode::Add;
        if(key == KEY_IME_TOGGLE) {
            g_linux_ime.toggle();
            update_ime_bar();
            return;
        }
        std::string ime_out;
        if(g_linux_ime.handle_key(key, ime_out)) {
            if(!ime_out.empty() && g_dict_text) lv_textarea_add_text(g_dict_text, ime_out.c_str());
            update_ime_bar();
            return;
        }
        if(key == 27) {
            g_dict_mode = DictMode::List;
            g_dict_text = nullptr;
            render();
            return;
        }
        if(key == '\n' || key == '\r') {
            std::string value = g_dict_text ? lv_textarea_get_text(g_dict_text) : "";
            std::string msg;
            if(adding) {
                std::string s = dict_trim(value);
                size_t sp = s.find(' ');
                std::string code = sp == std::string::npos ? "" : dict_trim(s.substr(0, sp));
                std::string word = sp == std::string::npos ? "" : dict_trim(s.substr(sp + 1));
                if(code.empty() || word.empty()) msg = "格式应为: 编码 空格 候选词";
                else if(!ime.addUserDictEntry((IME::UserDictKind)g_dict_kind, code, word))
                    msg = "添加失败: 候选词至少一个汉字";
                else {
                    msg = "已添加 " + code + " " + word;
                    g_dict_query.clear();
                    dict_reload();
                    for(int i = 0; i < (int)g_dict_entries.size(); ++i) {
                        if(g_dict_entries[i].code == code && g_dict_entries[i].word == word) { g_dict_sel = i; break; }
                    }
                }
            } else {
                g_dict_query = dict_trim(value);
                g_dict_sel = 0;
                g_dict_scroll = 0;
            }
            dict_reload();
            g_dict_mode = DictMode::List;
            g_dict_text = nullptr;
            render();
            if(!msg.empty()) set_status(msg);
            return;
        }
        if(handle_textarea_key(g_dict_text, key, false)) return;
        return;
    }

    if(g_dict_mode == DictMode::Choose) {
        if(key == 'q' || key == 'Q' || key == 27) { goto_screen(Screen::Settings); return; }
        if(key == KEY_UP || key == 'k' || key == KEY_DOWN || key == 'j') g_dict_sel = 1 - g_dict_sel;
        else if(key == '\n' || key == '\r') {
            g_dict_kind = g_dict_sel == 0 ? IME::FIXED_DICT : IME::DYNAMIC_DICT;
            g_dict_sel = 0;
            g_dict_scroll = 0;
            g_dict_selected.clear();
            g_dict_query.clear();
            dict_reload();
            g_dict_mode = DictMode::List;
        }
        render();
        return;
    }

    int total = (int)g_dict_filtered.size();
    if(key == 'q' || key == 'Q' || key == 27) {
        g_dict_mode = DictMode::Choose;
        g_dict_sel = 0;
        g_dict_scroll = 0;
        g_dict_selected.clear();
        g_dict_query.clear();
        render();
        return;
    }
    if((key == KEY_DOWN || key == 'j') && g_dict_sel < total - 1) { g_dict_sel++; render(); return; }
    if((key == KEY_UP || key == 'k') && g_dict_sel > 0) { g_dict_sel--; render(); return; }
    if(key == KEY_PAGE_DOWN) { g_dict_sel = std::min(total - 1, g_dict_sel + 9); render(); return; }
    if(key == KEY_PAGE_UP) { g_dict_sel = std::max(0, g_dict_sel - 9); render(); return; }
    if(key == '\t') {
        g_dict_kind = g_dict_kind == IME::FIXED_DICT ? IME::DYNAMIC_DICT : IME::FIXED_DICT;
        g_dict_sel = 0;
        g_dict_scroll = 0;
        g_dict_selected.clear();
        dict_reload();
        render();
        return;
    }
    if(key == 'a' || key == 'A') {
        g_dict_mode = DictMode::Add;
        render();
        return;
    }
    if(key == '/' || key == KEY_SEARCH) {
        g_dict_mode = DictMode::Search;
        render();
        return;
    }
    if(key == ' ') {
        if(total > 0) {
            int idx = g_dict_filtered[g_dict_sel];
            if(g_dict_selected.count(idx)) g_dict_selected.erase(idx);
            else g_dict_selected.insert(idx);
        }
        render();
        return;
    }
    if((key == 'd' || key == 'D') && total > 0) {
        std::vector<int> indices;
        if(g_dict_selected.empty()) indices.push_back(g_dict_filtered[g_dict_sel]);
        else for(int idx : g_dict_selected) indices.push_back(idx);
        ime.removeUserDictEntries((IME::UserDictKind)g_dict_kind, indices);
        g_dict_selected.clear();
        dict_reload();
        render();
        return;
    }
    render();
}

static void handle_filemgr(int key) {
    if(key == 'q' || key == 'Q' || key == 27) {
        file_manager_server_stop();
        goto_screen(Screen::Settings);
        return;
    }
    render();
}

static void editor_record_undo() {
    if(!g_editor) return;
    std::string cur = lv_textarea_get_text(g_editor);
    if(!g_undo_stack.empty() && g_undo_stack.back() == cur) return;
    g_undo_stack.push_back(cur);
    if(g_undo_stack.size() > 50) g_undo_stack.erase(g_undo_stack.begin());
    g_redo_stack.clear();
}

static bool editor_undo() {
    if(!g_editor || g_undo_stack.empty()) return false;
    std::string cur = lv_textarea_get_text(g_editor);
    g_redo_stack.push_back(cur);
    std::string prev = g_undo_stack.back();
    g_undo_stack.pop_back();
    g_ed_sel_anchor = -1;
    lv_textarea_set_text(g_editor, prev.c_str());
    lv_textarea_set_cursor_pos(g_editor, LV_TEXTAREA_CURSOR_LAST);
    return true;
}

static bool editor_redo() {
    if(!g_editor || g_redo_stack.empty()) return false;
    std::string cur = lv_textarea_get_text(g_editor);
    g_undo_stack.push_back(cur);
    std::string next = g_redo_stack.back();
    g_redo_stack.pop_back();
    g_ed_sel_anchor = -1;
    lv_textarea_set_text(g_editor, next.c_str());
    lv_textarea_set_cursor_pos(g_editor, LV_TEXTAREA_CURSOR_LAST);
    return true;
}

// ---------------------------------------------------------------------------
// 编辑器 Markdown 叠加层
//
// lv_textarea 依旧是唯一的数据源;这一层只负责盖在它上面做只读渲染:隐藏成对
// 的行内标记、给块元素上色、自己画光标,并把折叠标题下面的行整段藏起来。
// "Markdown渲染"关掉时根本不创建这一层,编辑器就是原来的纯文本样子。
// ---------------------------------------------------------------------------

// 标题颜色单独给一套,主题里的 accent 在黑白主题下和正文同色,分不出来。
static lv_color_t md_heading_color() {
    return g_settings.theme() == "light" ? lv_color_hex(0x1f4e79) : lv_color_hex(0x7fb4ff);
}

// 行内样式 → 实际字体。faux_bold 表示这一片要靠"再描一遍、偏移 1px"补粗:
// FreeType 位图模式不支持 FT_Outline_Embolden,伪粗体只能自己画两遍;伪斜体则免费
// (建 face 时的 FT_Set_Transform 斜切,位图模式同样生效)。
static const lv_font_t *md_style_font(const MdStyle &st, bool &faux_bold) {
    faux_bold = false;
    const lv_font_t *base = g_editor_font ? g_editor_font : lv_font_default();
    const lv_font_t *ital = g_editor_italic_font ? g_editor_italic_font
                                                 : (g_editor_faux_italic ? g_editor_faux_italic : base);
    if(st.bold) {
        if(g_editor_bold_font) return g_editor_bold_font;
        faux_bold = true;
        return st.italic ? ital : base;
    }
    return st.italic ? ital : base;
}

static MdStyle runs_style_at(const std::vector<MdRun> &runs, int at) {
    for(const auto &r : runs) {
        if(at < r.lo) break;
        if(at < r.hi) return r.st;
    }
    return MdStyle {};
}

// 一个排版碎片:显示文本里的一段,画在第 row 个视觉行、行内 x 处。
struct MdPiece {
    int row = 0, x = 0, w = 0, lo = 0, hi = 0;
    const lv_font_t *font = nullptr;
    bool faux_bold = false;
    MdStyle st;
};

// 唯一的排版逻辑:标签摆放、光标位置、选区矩形全部从这里读,三者天然一致。
static void md_layout_line(const std::string &disp, const std::vector<MdRun> &runs,
                           int letter_space, int max_w, std::vector<MdPiece> &out) {
    out.clear();
    if(max_w < 1) max_w = 1;
    int n = (int)disp.size();
    int row = 0, x = 0, i = 0;
    while(i < n) {
        MdStyle st = runs_style_at(runs, i);
        bool faux = false;
        const lv_font_t *f = md_style_font(st, faux);
        // 本段到下一个样式边界为止
        int seg_end = n;
        for(const auto &r : runs) {
            if(r.lo > i && r.lo < seg_end) seg_end = r.lo;
            if(r.hi > i && r.hi < seg_end) seg_end = r.hi;
        }
        while(i < seg_end) {
            int avail = max_w - x;
            if(avail < 1) { ++row; x = 0; avail = max_w; }
            uint32_t len = lv_text_get_next_line(disp.c_str() + i, f, letter_space, (uint32_t)avail, NULL,
                                                 LV_TEXT_FLAG_NONE);
            if(len == 0 || (int)len > seg_end - i) len = (uint32_t)(seg_end - i);
            if(len == 0) len = (uint32_t)(md_utf8_step(disp, (size_t)i) - (size_t)i);
            if(len == 0) break;
            int wpx = (int)lv_text_get_width(disp.c_str() + i, len, f, letter_space);
            if(wpx > avail && x > 0) { ++row; x = 0; continue; }
            out.push_back({row, x, wpx, i, i + (int)len, f, faux, st});
            i += (int)len;
            x += wpx;
            if(x >= max_w) { ++row; x = 0; }
        }
    }
}

// 显示字节偏移 → 在排版碎片里的位置。
static void md_caret_from_pieces(const std::string &disp, const std::vector<MdPiece> &p, int byte_off,
                                 int letter_space, int line_h, int &out_x, int &out_y) {
    out_x = 0;
    out_y = 0;
    if(p.empty()) return;
    if(byte_off <= p.front().lo) {
        out_x = p.front().x;
        out_y = p.front().row * line_h;
        return;
    }
    for(const MdPiece &q : p) {
        if(byte_off < q.lo) continue;
        if(byte_off <= q.hi) {
            out_x = q.x + (int)lv_text_get_width(disp.c_str() + q.lo, (uint32_t)(byte_off - q.lo), q.font,
                                                 letter_space);
            out_y = q.row * line_h;
            return;
        }
    }
    const MdPiece &q = p.back();
    out_x = q.x + q.w;
    out_y = q.row * line_h;
}

// 显示字节区间 → 每段一个高亮矩形(与文字同一套排版,不会跑偏)。
static void md_sel_from_pieces(const std::string &disp, const std::vector<MdPiece> &p, int d0, int d1,
                               int letter_space, int line_h, std::vector<lv_area_t> &out) {
    if(d1 <= d0) return;
    for(const MdPiece &q : p) {
        int lo = d0 > q.lo ? d0 : q.lo;
        int hi = d1 < q.hi ? d1 : q.hi;
        if(hi <= lo) continue;
        int x0 = q.x + (int)lv_text_get_width(disp.c_str() + q.lo, (uint32_t)(lo - q.lo), q.font, letter_space);
        int x1 = q.x + (int)lv_text_get_width(disp.c_str() + q.lo, (uint32_t)(hi - q.lo), q.font, letter_space);
        if(x1 > x0)
            out.push_back({x0, q.row * line_h, x1 - 1, q.row * line_h + line_h - 1});
    }
}

// 反白底色块、着重号小方点、选区都画在内容层自己的绘制回调里(先于子标签),
// 文字正好压在色块上。绘制回调里的坐标是屏幕绝对坐标,而矩形是按内容层局部
// 坐标算的,要加上层原点。
static void editor_md_draw_deco(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    if(!layer) return;
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    if(!o) return;
    lv_area_t org;
    lv_obj_get_coords(o, &org);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.bg_color = g_theme.fg;
    dsc.bg_opa = LV_OPA_COVER;
    for(const std::vector<lv_area_t> *tbl : {&g_md_box_rects, &g_md_dot_rects, &g_md_wave_rects}) {
        for(const lv_area_t &a : *tbl) {
            lv_area_t r{a.x1 + org.x1, a.y1 + org.y1, a.x2 + org.x1, a.y2 + org.y1};
            lv_draw_rect(layer, &dsc, &r);
        }
    }
    if(g_md_sel_rects.empty()) return;
    dsc.bg_color = g_theme.accent;
    dsc.bg_opa = LV_OPA_50;
    for(const lv_area_t &a : g_md_sel_rects) {
        lv_area_t r{a.x1 + org.x1, a.y1 + org.y1, a.x2 + org.x1, a.y2 + org.y1};
        lv_draw_rect(layer, &dsc, &r);
    }
}

static lv_obj_t *md_label_in(std::vector<lv_obj_t *> &pool, lv_obj_t *parent, int idx,
                             const lv_font_t *font, int letter_space, int line_space) {
    lv_obj_t *o = idx < (int)pool.size() ? pool[idx] : nullptr;
    if(!o) {
        o = lv_label_create(parent);
        // 排版已经在 md_layout_line 里算好了,标签只负责把这一小段画出来,不能再让它自己折行。
        lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_shadow_width(o, 0, 0);
        // 布局是自己算的,标签不承担滚动职责,否则裁边时也会冒出滚动条
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        pool.push_back(o);
    }
    // 字体随片段变(粗体/斜体/正体),复用的标签必须重设,否则上一帧的字形会留在原地。
    // 比如正在输入 **粗体** 时,闭合的 * 敲下去前这一格还是斜体字形,敲完变粗体却只叠了一层。
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_letter_space(o, letter_space, 0);
    lv_obj_set_style_text_line_space(o, line_space, 0);
    return o;
}

static lv_obj_t *md_rule_in(std::vector<lv_obj_t *> &pool, lv_obj_t *parent, int idx) {
    if(idx < (int)pool.size()) return pool[idx];
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, g_theme.muted, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    pool.push_back(o);
    return o;
}

// 书名波浪线:周期 8px、振幅 2px 的三角波,和 ESP32 那条同一套几何。
// 横排沿 x 走、画在字下;竖排沿 y 走、画在列左侧。每段 2px 宽、2px 高。
static void md_push_wave(int x, int y, int w, std::vector<lv_area_t> &out) {
    for(int dx = 0; dx < w; dx += 2) {
        int ph = dx & 7;
        int dy = ph < 2 ? 0 : (ph < 4 ? 1 : (ph < 6 ? 2 : 1));
        int x2 = (dx + 1 < w) ? x + dx + 1 : x + dx;
        out.push_back({x + dx, y + dy, x2, y + dy + 1});
    }
}

static void vt_push_wave(int x, int y, int h, std::vector<lv_area_t> &out) {
    for(int dy = 0; dy < h; dy += 2) {
        int ph = dy & 7;
        int dx = ph < 2 ? 0 : (ph < 4 ? 1 : (ph < 6 ? 2 : 1));
        int y2 = (dy + 1 < h) ? y + dy + 1 : y + dy;
        out.push_back({x + dx, y + dy, x + dx + 1, y2});
    }
}

// 一行的反白底块与着重号小方点,推进绘制回调要用的矩形表。
static void md_push_deco(const std::string &disp, const std::vector<MdPiece> &pieces,
                         int line_y, int line_h, int letter_h, int letter_space) {
    for(const MdPiece &q : pieces) {
        if(q.hi <= q.lo) continue;
        int ry = line_y + q.row * line_h;
        int extra = q.faux_bold ? 1 : 0;
        if(q.st.invert)
            g_md_box_rects.push_back({q.x, ry, q.x + q.w + extra - 1, ry + letter_h - 1});
        if(q.st.wavy) md_push_wave(q.x, ry + letter_h - 3, q.w + extra, g_md_wave_rects);
        if(q.st.emph) {
            // 和 ESP32 一样落在字脚下面:基线再往下一个 descent,正好压住 em 框下沿
            int dy = ry + letter_h + 1;
            for(int k = q.lo; k < q.hi;) {
                size_t n = md_utf8_step(disp, (size_t)k);
                if(disp[k] != ' ') {
                    int w0 = (int)lv_text_get_width(disp.c_str() + q.lo, (uint32_t)(k - q.lo), q.font,
                                                    letter_space);
                    int w1 = (int)lv_text_get_width(disp.c_str() + q.lo, (uint32_t)((int)n - q.lo), q.font,
                                                    letter_space);
                    int cx = q.x + w0 + (w1 - w0 - 3) / 2;
                    g_md_dot_rects.push_back({cx, dy, cx + 2, dy + 1});
                }
                k = (int)n;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 聚焦模式:光标行之外的正文压暗
//
// 用的是两块半透明底色罩,和下面那套叠加层排版无关,所以 markdown 视图和纯文本
// textarea 两条渲染路径共用。底色取主题背景色:深色主题下压暗、浅色主题下洗淡,
// 两边都成立。罩子插在正文层(叠加层/textarea)的紧后面,既盖得住正文,又不会
// 盖到状态栏和输入法候选条。
// ---------------------------------------------------------------------------
static lv_obj_t *focus_dim_obj(int i) {
    if(g_focus_dim[i]) return g_focus_dim[i];
    lv_obj_t *o = lv_obj_create(g_root);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_60, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *base = g_md_view ? g_md_view : g_editor;
    if(base) lv_obj_move_to_index(o, (int)lv_obj_get_index(base) + 1);
    g_focus_dim[i] = o;
    return o;
}

// caret / vp 都取绝对坐标(调用方用 lv_obj_get_coords 拿)
static void focus_dim_update(bool caret_visible, lv_area_t caret, const lv_area_t &vp) {
    if(!editor_focus() || !caret_visible) {
        for(lv_obj_t *o : g_focus_dim)
            if(o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if(caret.y1 < vp.y1) caret.y1 = vp.y1;
    if(caret.y2 > vp.y2) caret.y2 = vp.y2;
    // 夹完如果区间反了,说明光标行整个落在视口外(纯文本路径不居中时会这样),
    // 这时不该留一块「亮带」——两块罩直接全隐,否则整屏都会被压暗
    bool band_ok = caret.y1 <= caret.y2;
    for(int i = 0; i < 2; ++i) {
        lv_obj_t *o = focus_dim_obj(i);
        int y1 = i == 0 ? vp.y1 : caret.y2 + 1;
        int y2 = i == 0 ? caret.y1 - 1 : vp.y2;
        if(!band_ok || y2 < y1) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(o, vp.x1, y1);
        lv_obj_set_size(o, vp.x2 - vp.x1 + 1, y2 - y1 + 1);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// 当前行的亮带。画在叠加层内容对象的 DRAW_MAIN 里,也就是文字**下面**,所以只染底色
// 不糊字。单独挂一个回调(而不是塞进 editor_md_draw_deco)是因为只读阅读视图也用了
// 那个回调,亮带只该出现在编辑器里。
static void md_focus_band_draw(lv_event_t *e) {
    if(!g_md_focus_band_on) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    if(!layer || !o) return;
    lv_area_t org;
    lv_obj_get_coords(o, &org);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.bg_color = g_theme.accent;
    dsc.bg_opa = LV_OPA_20;
    lv_area_t r{g_md_focus_band.x1 + org.x1, g_md_focus_band.y1 + org.y1,
                g_md_focus_band.x2 + org.x1, g_md_focus_band.y2 + org.y1};
    lv_draw_rect(layer, &dsc, &r);
}

static void editor_md_create(int x, int y, int w, int h, int content_w) {
    g_md_max_w = content_w > 0 ? content_w : w;
    g_md_view_h = h;

    g_md_view = lv_obj_create(g_root);
    lv_obj_set_pos(g_md_view, x, y);
    lv_obj_set_size(g_md_view, w, h);
    lv_obj_set_style_radius(g_md_view, 0, 0);
    lv_obj_set_style_border_width(g_md_view, 0, 0);
    lv_obj_set_style_pad_all(g_md_view, 0, 0);
    lv_obj_set_style_shadow_width(g_md_view, 0, 0);
    lv_obj_set_style_bg_color(g_md_view, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(g_md_view, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(g_md_view, LV_SCROLLBAR_MODE_OFF);

    g_md_content = lv_obj_create(g_md_view);
    lv_obj_set_pos(g_md_content, 0, 0);
    lv_obj_set_size(g_md_content, w, h);
    lv_obj_set_style_radius(g_md_content, 0, 0);
    lv_obj_set_style_border_width(g_md_content, 0, 0);
    lv_obj_set_style_pad_all(g_md_content, 0, 0);
    lv_obj_set_style_shadow_width(g_md_content, 0, 0);
    lv_obj_set_style_bg_opa(g_md_content, LV_OPA_TRANSP, 0);
    // 这一层只负责盛放排版好的碎片,滚动由 g_md_view 做。若它自己可滚动,碎片超出
    // 1px(伪粗体副本)就会触发 AUTO 滚动条,在文字底部画出一条横贯整屏的灰线。
    lv_obj_remove_flag(g_md_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_md_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_md_content, md_focus_band_draw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(g_md_content, editor_md_draw_deco, LV_EVENT_DRAW_MAIN, nullptr);

    g_md_caret = lv_obj_create(g_md_content);
    lv_obj_set_pos(g_md_caret, 0, 0);
    lv_obj_set_size(g_md_caret, 2, 24);
    lv_obj_set_style_radius(g_md_caret, 0, 0);
    lv_obj_set_style_border_width(g_md_caret, 0, 0);
    lv_obj_set_style_pad_all(g_md_caret, 0, 0);
    lv_obj_set_style_shadow_width(g_md_caret, 0, 0);
    lv_obj_set_style_bg_color(g_md_caret, g_theme.accent, 0);
    lv_obj_set_style_bg_opa(g_md_caret, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_md_caret, LV_OBJ_FLAG_SCROLLABLE);
}

static void editor_md_refresh() {
    if(!g_md_view || !g_md_content || !g_editor || g_md_max_w <= 0) return;
    const lv_font_t *font = g_editor_font ? g_editor_font : lv_font_default();
    int letter_space = (int)lv_obj_get_style_text_letter_space(g_editor, 0);
    int line_space = (int)lv_obj_get_style_text_line_space(g_editor, 0);
    int letter_h = lv_font_get_line_height(font);
    int line_h = letter_h + line_space;

    const char *ctxt = lv_textarea_get_text(g_editor);
    std::string text = ctxt ? ctxt : "";
    uint32_t caret_byte = lv_text_encoded_get_byte_id(ctxt ? ctxt : "", lv_textarea_get_cursor_pos(g_editor));

    g_md_sel_rects.clear();
    g_md_box_rects.clear();
    g_md_dot_rects.clear();
    g_md_wave_rects.clear();
    g_md_focus_band_on = false;
    int sel_lo = -1, sel_hi = -1;
    if(g_ed_sel_anchor >= 0 && g_ed_sel_anchor != (int)caret_byte) {
        sel_lo = g_ed_sel_anchor < (int)caret_byte ? g_ed_sel_anchor : (int)caret_byte;
        sel_hi = g_ed_sel_anchor < (int)caret_byte ? (int)caret_byte : g_ed_sel_anchor;
    }

    MdDoc ml;
    md_split_caret(text, caret_byte, ml);
    std::vector<std::string> &lines = ml.lines;
    size_t nlines = lines.size();

    // 代码块范围
    std::vector<char> in_code(nlines, 0);
    {
        bool code = false;
        for(size_t i = 0; i < nlines; ++i) {
            size_t lead = 0;
            while(lead < lines[i].size() && lines[i][lead] == ' ') lead++;
            bool fence = lines[i].compare(lead, 3, "```") == 0 || lines[i].compare(lead, 3, "~~~") == 0;
            in_code[i] = code ? 1 : 0;
            if(fence) code = !code;
        }
    }

    // 标题级别
    std::vector<int> hlevel(nlines, 0);
    for(size_t i = 0; i < nlines; ++i) {
        if(!in_code[i]) hlevel[i] = md_heading_level_of(lines[i]);
    }

    // 行号变了(增删行)之后旧折叠就不可信了,顺手清掉失效项
    for(auto it = g_md_folded.begin(); it != g_md_folded.end();) {
        if(*it >= (int)nlines || hlevel[*it] == 0) it = g_md_folded.erase(it);
        else ++it;
    }

    // 折叠:被折叠标题之后、直到同级或更高级标题之前的所有行都藏起来
    std::vector<char> hidden(nlines, 0);
    for(int guard = 0; guard < 32; ++guard) {
        bool in_fold = false;
        int fold_level = 0;
        for(size_t i = 0; i < nlines; ++i) {
            if(in_fold && hlevel[i] && hlevel[i] <= fold_level) in_fold = false;
            hidden[i] = in_fold ? 1 : 0;
            if(!in_fold && hlevel[i] && g_md_folded.count((int)i)) {
                in_fold = true;
                fold_level = hlevel[i];
            }
        }
        if(!hidden[ml.caret_line]) break;
        // 光标跑进了折叠区,把盖住它的那个折叠展开
        int victim = -1;
        for(int j = ml.caret_line - 1; j >= 0; --j) {
            if(g_md_folded.count(j)) { victim = j; break; }
        }
        if(victim < 0) break;
        g_md_folded.erase(victim);
    }

    int lbi = 0, rbi = 0, y = 0;
    int caret_x = -1, caret_y = -1;
    for(size_t i = 0; i < nlines; ++i) {
        if(hidden[i]) continue;
        MdRender d = md_build_line(lines[i], in_code[i] != 0, ((int)i == ml.caret_line) ? ml.caret_rel : -1);

        if(d.rule) {
            lv_obj_t *o = md_rule_in(g_md_rules, g_md_content, rbi++);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(o, 0, y + letter_h / 2 - 1);
            lv_obj_set_size(o, g_md_max_w, 2);
            y += line_h;
            continue;
        }

        std::string disp = d.text;
        if(d.heading) disp += g_md_folded.count((int)i) ? "  [+]" : "  [-]";
        // 首行缩进:纯段落行首补两个全角空格(标题/列表/引用/代码/分隔线除外)
        int indent_bytes = 0;
        if(g_settings.first_line_indent() && !d.heading && !d.rule && !d.muted &&
           d.prefix_bytes == 0 && in_code[i] == 0 && !disp.empty()) {
            disp = "\xe3\x80\x80\xe3\x80\x80" + disp;
            indent_bytes = 6;
        }

        // 行内样式片段跟着 disp 平移(缩进 6 字节);标题的折叠标志也跟着整行加粗
        std::vector<MdRun> druns = d.runs;
        for(MdRun &r : druns) {
            r.lo += indent_bytes;
            r.hi += indent_bytes;
        }
        if(d.heading && !druns.empty()) druns.back().hi = (int)disp.size();

        std::vector<MdPiece> pieces;
        md_layout_line(disp, druns, letter_space, g_md_max_w, pieces);
        int rows = 1;
        for(const MdPiece &q : pieces)
            if(q.row + 1 > rows) rows = q.row + 1;
        int h = rows * line_h;

        lv_color_t color = d.heading ? md_heading_color() : (d.muted ? g_theme.muted : g_theme.fg);

        // 选区:落在本行的部分映射成显示坐标,再按排版碎片切成矩形
        if(sel_lo >= 0) {
            int lb = (int)ml.start[i];
            int le = lb + (int)lines[i].size();
            if(sel_hi > lb && sel_lo < le) {
                int a = sel_lo > lb ? sel_lo : lb;
                int b = sel_hi < le ? sel_hi : le;
                int d0 = md_display_offset(d, a - lb) + indent_bytes;
                int d1 = md_display_offset(d, b - lb) + indent_bytes;
                size_t before = g_md_sel_rects.size();
                md_sel_from_pieces(disp, pieces, d0, d1, letter_space, line_h, g_md_sel_rects);
                for(size_t k = before; k < g_md_sel_rects.size(); ++k) {
                    g_md_sel_rects[k].y1 += y;
                    g_md_sel_rects[k].y2 += y;
                }
            }
        }

        // 反白底块与着重号小方点:按排版碎片算好,交给内容层的绘制回调
        md_push_deco(disp, pieces, y, line_h, letter_h, letter_space);

        for(const MdPiece &q : pieces) {
            int pw = g_md_max_w - q.x;
            if(pw < 1) pw = 1;
            std::string seg = disp.substr((size_t)q.lo, (size_t)(q.hi - q.lo));
            uint32_t decor = LV_TEXT_DECOR_NONE;
            if(q.st.underline) decor |= LV_TEXT_DECOR_UNDERLINE;
            if(q.st.strike) decor |= LV_TEXT_DECOR_STRIKETHROUGH;
            int nlab = q.faux_bold ? 2 : 1;
            for(int d2 = 0; d2 < nlab; ++d2) {
                lv_obj_t *o = md_label_in(g_md_labels, g_md_content, lbi++, q.font, letter_space, line_space);
                lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(o, q.x + d2, y + q.row * line_h);
                lv_obj_set_size(o, pw, letter_h);
                lv_label_set_text(o, seg.c_str());
                lv_obj_set_style_text_decor(o, (lv_text_decor_t)decor, 0);
                lv_obj_set_style_text_color(o, q.st.invert ? g_theme.bg : color, 0);
            }
        }

        if((int)i == ml.caret_line) {
            md_caret_from_pieces(disp, pieces, md_display_offset(d, ml.caret_rel) + indent_bytes, letter_space,
                                 line_h, caret_x, caret_y);
            caret_y += y;
            if(editor_focus() && !g_search_panel) {
                g_md_focus_band = {0, y, g_md_max_w - 1, y + h - 1};
                g_md_focus_band_on = true;
            }
        }
        y += h;
    }

    for(int k = lbi; k < (int)g_md_labels.size(); ++k) lv_obj_add_flag(g_md_labels[k], LV_OBJ_FLAG_HIDDEN);
    for(int k = rbi; k < (int)g_md_rules.size(); ++k) lv_obj_add_flag(g_md_rules[k], LV_OBJ_FLAG_HIDDEN);
    // 打字机模式:光标行钉在「编辑区正中再往上一行」。中线按**编辑区**算(下边取
    // chrome_bottom()),不含状态栏,不是屏幕正中。
    // 内容层整体下移 pad、高度也多出这一截,滚动范围因此上下各多出 pad 的空白,
    // 首行/末行都能落到 target——不留白的话 lv_obj_scroll_to_y 会把滚动量夹进
    // [0, 内容高-视口高],文首/文末的居中看着就像没生效。pad 的取值是反推出来的:
    // 末行需要的滚动量 = 可滚上限时刚好够,所以既不浪费空白也不会被夹。
    bool tw = editor_typewriter();
    int view_y = (int)lv_obj_get_y(g_md_view);
    int view_h = g_md_view_h;
    int center = (view_y + chrome_bottom()) / 2;
    int target = center - line_h;
    int pad = tw ? view_y + view_h - center : 0;
    if(pad < 0) pad = 0;
    lv_obj_set_pos(g_md_content, 0, pad);
    lv_obj_set_height(g_md_content, (y > 0 ? y : letter_h) + pad);
    // 滚动范围是按子对象坐标算的,先把这一层排完版再滚动
    lv_obj_update_layout(g_md_view);

    if(g_md_caret) {
        if(caret_y >= 0 && !g_search_panel) {
            if(caret_x > g_md_max_w - 2) caret_x = g_md_max_w - 2;
            if(caret_x < 0) caret_x = 0;
            lv_obj_remove_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(g_md_caret, caret_x, caret_y);
            lv_obj_set_size(g_md_caret, 2, letter_h);
            lv_obj_move_to_index(g_md_caret, -1);
        } else {
            lv_obj_add_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if(caret_y >= 0) {
        int scroll;
        if(tw) {
            // 正文已经整体下移了 pad,要让光标行顶落在 target,滚这一点就够了
            scroll = view_y + pad + caret_y - target;
        } else {
            scroll = (int)lv_obj_get_scroll_y(g_md_view);
            if(caret_y - scroll < 0) scroll = caret_y;
            else if(caret_y + line_h - scroll > g_md_view_h) scroll = caret_y + line_h - g_md_view_h;
        }
        lv_obj_scroll_to_y(g_md_view, scroll, LV_ANIM_OFF);
    }

    // 聚焦模式:光标行不动,上下盖两块罩把别处压暗。暗罩的「亮带」直接取上面那块亮带
    // 的几何,而不是去读光标对象的 coords:光标是在这一步之后才 set_pos 的,它的
    // coords 要等下一次排版才更新,这里读到的会落后一行(实测),亮带和暗罩就会差一行。
    // 亮带和内容层用的是同一次算出来的行坐标,两者都在内容层局部坐标里,只差一个
    // 内容层原点,拿它折算绝对坐标最稳。
    lv_area_t vp {};
    lv_obj_get_coords(g_md_view, &vp);
    lv_area_t org {};
    lv_obj_get_coords(g_md_content, &org);
    bool caret_on = g_md_focus_band_on && g_md_caret &&
                    !lv_obj_has_flag(g_md_caret, LV_OBJ_FLAG_HIDDEN);
    lv_area_t ca = vp;
    if(caret_on) {
        ca.y1 = org.y1 + g_md_focus_band.y1;
        ca.y2 = org.y1 + g_md_focus_band.y2;
    }
    focus_dim_update(caret_on, ca, vp);
}

// ---------------------------------------------------------------------------
// 只读 Markdown 渲染(阅读视图 / 历史预览)
//
// 复用编辑器那套行级排版:同样的隐藏标记、行内样式、代码围栏、分隔线、任务框。
// 区别只是没有光标、没有选区、没有折叠,滚动由调用方按行号驱动。
// ---------------------------------------------------------------------------

static void md_code_flags(const std::vector<std::string> &lines, std::vector<char> &in_code) {
    in_code.assign(lines.size(), 0);
    bool code = false;
    for(size_t i = 0; i < lines.size(); ++i) {
        size_t lead = 0;
        while(lead < lines[i].size() && lines[i][lead] == ' ') lead++;
        bool fence = lines[i].compare(lead, 3, "```") == 0 || lines[i].compare(lead, 3, "~~~") == 0;
        in_code[i] = code ? 1 : 0;
        if(fence) code = !code;
    }
}

static void md_readonly_create(int x, int y, int w, int h) {
    g_ro_view = lv_obj_create(g_root);
    lv_obj_set_pos(g_ro_view, x, y);
    lv_obj_set_size(g_ro_view, w, h);
    base_style(g_ro_view);
    lv_obj_set_style_radius(g_ro_view, 0, 0);
    lv_obj_set_style_border_width(g_ro_view, 0, 0);
    lv_obj_set_style_pad_all(g_ro_view, 0, 0);
    lv_obj_set_style_shadow_width(g_ro_view, 0, 0);
    lv_obj_set_scrollbar_mode(g_ro_view, LV_SCROLLBAR_MODE_OFF);

    g_ro_content = lv_obj_create(g_ro_view);
    lv_obj_set_pos(g_ro_content, 0, 0);
    lv_obj_set_size(g_ro_content, w, h);
    lv_obj_set_style_radius(g_ro_content, 0, 0);
    lv_obj_set_style_border_width(g_ro_content, 0, 0);
    lv_obj_set_style_pad_all(g_ro_content, 0, 0);
    lv_obj_set_style_shadow_width(g_ro_content, 0, 0);
    lv_obj_set_style_bg_opa(g_ro_content, LV_OPA_TRANSP, 0);
    // 同编辑器:内容层自己不能滚动,否则伪粗体多出的 1px 会冒出假滚动条横线。
    lv_obj_remove_flag(g_ro_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_ro_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_ro_content, editor_md_draw_deco, LV_EVENT_DRAW_MAIN, nullptr);
}

static void md_render_readonly(const std::string &text, int content_w, int line_space,
                               std::vector<int> *line_y_out) {
    if(!g_ro_view || !g_ro_content || content_w <= 0) return;
    const lv_font_t *font = g_editor_font ? g_editor_font : lv_font_default();
    const int letter_space = 0;
    const int letter_h = lv_font_get_line_height(font);
    const int line_h = letter_h + line_space;

    g_md_box_rects.clear();
    g_md_dot_rects.clear();
    g_md_wave_rects.clear();

    MdDoc doc;
    md_split_caret(text, 0, doc);
    std::vector<char> in_code;
    md_code_flags(doc.lines, in_code);
    if(line_y_out) line_y_out->assign(doc.lines.size(), 0);

    int lbi = 0, rbi = 0, y = 0;
    for(size_t i = 0; i < doc.lines.size(); ++i) {
        if(line_y_out) (*line_y_out)[i] = y;
        MdRender d = md_build_line(doc.lines[i], in_code[i] != 0, -1);

        if(d.rule) {
            lv_obj_t *o = md_rule_in(g_ro_rules, g_ro_content, rbi++);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(o, 0, y + letter_h / 2 - 1);
            lv_obj_set_size(o, content_w, 2);
            y += line_h;
            continue;
        }

        std::string disp = d.text;
        int indent_bytes = 0;
        if(g_settings.first_line_indent() && !d.heading && !d.muted &&
           d.prefix_bytes == 0 && in_code[i] == 0 && !disp.empty()) {
            disp = "\xe3\x80\x80\xe3\x80\x80" + disp;
            indent_bytes = 6;
        }
        std::vector<MdRun> druns = d.runs;
        for(MdRun &r : druns) {
            r.lo += indent_bytes;
            r.hi += indent_bytes;
        }
        if(d.heading && !druns.empty()) druns.back().hi = (int)disp.size();

        std::vector<MdPiece> pieces;
        md_layout_line(disp, druns, letter_space, content_w, pieces);
        int rows = 1;
        for(const MdPiece &q : pieces)
            if(q.row + 1 > rows) rows = q.row + 1;
        int h = rows * line_h;
        lv_color_t color = d.heading ? md_heading_color() : (d.muted ? g_theme.muted : g_theme.fg);

        md_push_deco(disp, pieces, y, line_h, letter_h, letter_space);

        for(const MdPiece &q : pieces) {
            int pw = content_w - q.x;
            if(pw < 1) pw = 1;
            std::string seg = disp.substr((size_t)q.lo, (size_t)(q.hi - q.lo));
            uint32_t decor = LV_TEXT_DECOR_NONE;
            if(q.st.underline) decor |= LV_TEXT_DECOR_UNDERLINE;
            if(q.st.strike) decor |= LV_TEXT_DECOR_STRIKETHROUGH;
            int nlab = q.faux_bold ? 2 : 1;
            for(int d2 = 0; d2 < nlab; ++d2) {
                lv_obj_t *o = md_label_in(g_ro_labels, g_ro_content, lbi++, q.font, letter_space, line_space);
                lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(o, q.x + d2, y + q.row * line_h);
                lv_obj_set_size(o, pw, letter_h);
                lv_label_set_text(o, seg.c_str());
                lv_obj_set_style_text_decor(o, (lv_text_decor_t)decor, 0);
                lv_obj_set_style_text_color(o, q.st.invert ? g_theme.bg : color, 0);
            }
        }
        y += h;
    }

    for(int k = lbi; k < (int)g_ro_labels.size(); ++k) lv_obj_add_flag(g_ro_labels[k], LV_OBJ_FLAG_HIDDEN);
    for(int k = rbi; k < (int)g_ro_rules.size(); ++k) lv_obj_add_flag(g_ro_rules[k], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(g_ro_content, y > 0 ? y : letter_h);
    // 滚动范围是按子对象坐标算的,先把这一层排完版,调用方再滚动
    lv_obj_update_layout(g_ro_view);
}

// ---------------------------------------------------------------------------
// 竖排编辑器
//
// 与 Markdown 叠加层同一套路:lv_textarea 仍是唯一数据源,这一层按「格」重排,
// 每列是一个用 \n 连接的标签,参考线/选区/光标自己画。设置里的「文字方向」
// 选竖排时才创建,切换回横排就整个不存在。
// ---------------------------------------------------------------------------

static bool editor_vertical() {
    return g_settings.editor_orientation() == "vertical";
}

static const lv_font_t *editor_font() {
    return g_editor_font ? g_editor_font : lv_font_default();
}

// 提示写作的题目:只有「提示写作」起始的这次编辑才有(新建/编辑已有日记都没有)。
static std::string editor_prompt_text() {
    if(g_quick_slot >= 0 || !g_edit_file.empty() || g_prompt.empty()) return "";
    return "提示:" + g_prompt;
}

// 参考线画在视口自己的图层上,子标签之后绘制,正好压在线右侧。
static void editor_vt_draw_guides(lv_event_t *e) {
    if(!g_vt_view || !g_settings.vertical_reference_line()) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    if(!layer) return;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = g_theme.muted;
    dsc.bg_opa = LV_OPA_50;
    dsc.radius = 0;
    dsc.border_width = 0;

    std::string sk = g_settings.vertical_reference_line_style();
    VerticalGuideStyle style = VerticalGuideStyle::Solid;
    if(sk == "dash") style = VerticalGuideStyle::Dash;
    else if(sk == "dot") style = VerticalGuideStyle::Dot;

    // 绘制回调里的坐标是屏幕绝对坐标,而列位置是视口局部坐标,必须加上视口原点,
    // 否则参考线会整体偏到左上角,压在字上。
    lv_area_t org;
    lv_obj_get_coords(g_vt_view, &org);

    // 参考线落在两列之间的空档正中。列宽和间距都随字号变,这里现算而不是写死。
    int lh = lv_font_get_line_height(editor_font());
    int gap = g_vt_m.colAdvance - lh;
    if(gap < 2) gap = 2;
    int rightGuideX = org.x1 + g_vt_m.x + g_vt_m.w - g_vt_m.colAdvance + lh + gap / 2;
    int top = org.y1 + g_vt_m.y;
    int bottom = org.y1 + g_vt_m.y + g_vt_m.h;

    for(int i = 0; i <= g_vt_m.cols; ++i) {
        int gx = rightGuideX - i * g_vt_m.colAdvance;
        if(style == VerticalGuideStyle::Solid) {
            lv_area_t a{gx, top, gx, bottom};
            lv_draw_rect(layer, &dsc, &a);
            continue;
        }
        int step = style == VerticalGuideStyle::Dash ? 8 : 4;
        int seg = style == VerticalGuideStyle::Dash ? 5 : 1;
        for(int y = top; y < bottom; y += step) {
            int h2 = seg;
            if(y + h2 > bottom) h2 = bottom - y;
            if(h2 <= 0) continue;
            lv_area_t a{gx, y, gx, y + h2 - 1};
            lv_draw_rect(layer, &dsc, &a);
        }
    }
}

// 竖排的装饰几何与 ESP32 对齐:反白是整格实心块,下划线/删除线是列左侧/中央的
// 竖线(横排的线画在字下,竖排就得画在列的侧向),着重号是每行一个 3×3 小方块。
static void editor_vt_draw_deco(lv_event_t *e) {
    if(g_vt_box_rects.empty() && g_vt_deco_rects.empty()) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    if(!layer) return;
    lv_area_t org;
    lv_obj_get_coords(g_vt_view, &org);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.border_width = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = g_theme.fg;
    for(const std::vector<lv_area_t> *tbl : {&g_vt_box_rects, &g_vt_deco_rects}) {
        for(const lv_area_t &a : *tbl) {
            lv_area_t r{a.x1 + org.x1, a.y1 + org.y1, a.x2 + org.x1, a.y2 + org.y1};
            lv_draw_rect(layer, &dsc, &r);
        }
    }
}

static lv_obj_t *vt_col_at(int idx) {
    if(idx < (int)g_vt_cols.size()) return g_vt_cols[idx];
    lv_obj_t *o = lv_label_create(g_vt_view);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(o, editor_font(), 0);
    lv_obj_set_style_text_letter_space(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    g_vt_cols.push_back(o);
    return o;
}

// 提示词的竖列:只用弱化色,与正文区分。
static lv_obj_t *vt_prompt_col_at(int idx) {
    if(idx < (int)g_vt_prompt_cols.size()) return g_vt_prompt_cols[idx];
    lv_obj_t *o = lv_label_create(g_vt_view);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(o, editor_font(), 0);
    lv_obj_set_style_text_color(o, g_theme.muted, 0);
    lv_obj_set_style_text_letter_space(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    g_vt_prompt_cols.push_back(o);
    return o;
}

// 列里有带样式的格时整列改成逐格标签:紧贴该列,尺寸一格一拍。
static lv_obj_t *vt_cell_at(int idx) {
    if(idx < (int)g_vt_cells.size()) return g_vt_cells[idx];
    lv_obj_t *o = lv_label_create(g_vt_view);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    g_vt_cells.push_back(o);
    return o;
}

static lv_obj_t *vt_mark_at(int idx) {
    if(idx < (int)g_vt_marks.size()) return g_vt_marks[idx];
    lv_obj_t *o = lv_obj_create(g_vt_view);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, g_theme.accent, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    g_vt_marks.push_back(o);
    return o;
}

static lv_color_t vt_kind_color(VerticalCellKind kind) {
    if(kind == VerticalCellKind::Heading) return md_heading_color();
    if(kind == VerticalCellKind::Muted || kind == VerticalCellKind::Rule) return g_theme.muted;
    return g_theme.fg;
}

// ---------------------------------------------------------------------------
// 竖排标点
//
// 横排标点的墨迹是给横排设计的:、。，在字面左下角,：；！？贴着左边。竖排里
// 这些得挪到格的右上角(句读)或居中。字体自带竖排表现形(︵︶ ︽︾ ﹁﹂ ﹃﹄ ︱ 那批)
// 就直接换码点,让字体给真正的竖排字形;字体没有的(︐︑︒︓︔︕︖︙ 全缺)才退回
// 平移:把原字形在格内挪一下,不去动字库。
// ---------------------------------------------------------------------------

// 横排标点 → 竖排表现形(与 ESP32 同一张表)
static uint32_t vt_punct_cp(uint32_t cp) {
    switch(cp) {
    case 0x3001: return 0xFE11;  // 、 → ︑
    case 0x3002: return 0xFE12;  // 。 → ︒
    case 0xFF0C: return 0xFE10;  // ， → ︐
    case 0xFF61: return 0xFE12;  // ｡ → ︒
    case 0xFF62: return 0xFE41;  // ｢ → ﹁
    case 0xFF63: return 0xFE42;  // ｣ → ﹂
    case 0xFF64: return 0xFE11;  // ､ → ︑
    case 0xFF1A: return 0xFE13;  // ： → ︓
    case 0xFF1B: return 0xFE14;  // ； → ︔
    case 0xFF01: return 0xFE15;  // ！ → ︕
    case 0xFF1F: return 0xFE16;  // ？ → ︖
    case 0x2026: return 0xFE19;  // … → ︙
    case 0xFF08: return 0xFE35;  // （ → ︵
    case 0xFF09: return 0xFE36;  // ） → ︶
    case 0x3008: return 0xFE3F;  // 〈 → ︿
    case 0x3009: return 0xFE40;  // 〉 → ﹀
    case 0x300A: return 0xFE3D;  // 《 → ︽
    case 0x300B: return 0xFE3E;  // 》 → ︾
    case 0x300C: return 0xFE41;  // 「 → ﹁
    case 0x300D: return 0xFE42;  // 」 → ﹂
    case 0x300E: return 0xFE43;  // 『 → ﹃
    case 0x300F: return 0xFE44;  // 』 → ﹄
    case 0x201C: return 0xFE41;  // “ → ﹁
    case 0x201D: return 0xFE42;  // ” → ﹂
    case 0x2018: return 0xFE43;  // ‘ → ﹃
    case 0x2019: return 0xFE44;  // ’ → ﹄
    case 0x2014: return 0xFE31;  // — → ︱
    default: return 0;
    }
}

// 字库里有这个字形吗。必须看 is_placeholder:缺字时 FreeType 会退回 .notdef,
// 而这份字体的 .notdef 是有轮廓的方框,靠 box_w 判会把缺字当成有字。
static bool vt_font_has_glyph(const lv_font_t *f, uint32_t cp) {
    if(!f) return false;
    lv_font_glyph_dsc_t dsc {};
    if(!lv_font_get_glyph_dsc(f, &dsc, cp, 0)) return false;
    if(dsc.is_placeholder) return false;
    return dsc.box_w > 0 || dsc.box_h > 0;
}

static std::string vt_utf8_from_cp(uint32_t cp) {
    std::string s;
    if(cp < 0x80) {
        s += (char)cp;
    } else if(cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if(cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

// 整格正好一个字符时给出它的码点;空串/多字符格返回 0
static uint32_t vt_cell_cp(const std::string &g) {
    if(g.empty()) return 0;
    unsigned char c = (unsigned char)g[0];
    size_t n = 1;
    uint32_t cp = c;
    if((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    if(n != g.size()) return 0;
    for(size_t k = 1; k < n; ++k) {
        unsigned char cc = (unsigned char)g[k];
        if((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

// 字体没有竖排字形、只能平移的几类。TopRight/MidRight 是格内偏移,Stack 是省略号:
// 横排的三点要竖过来,平移做不到,改画三个小方点。
enum class VtPunctPlace { None, TopRight, MidRight, Stack };

static VtPunctPlace vt_punct_place(uint32_t cp) {
    switch(cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF61: case 0xFF64:
        return VtPunctPlace::TopRight;  // 、。，｡､ → 右上角
    case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F:
        return VtPunctPlace::MidRight;  // ：；！？ → 向右居中
    case 0x2026:
        return VtPunctPlace::Stack;
    default: return VtPunctPlace::None;
    }
}

// 该列有没有需要格内挪位的格。有的话整列得逐格摆,列标签没法只挪其中一格。
static bool vt_col_has_placed_punct(const std::vector<VerticalCell> &cells, int start, int end) {
    for(int i = start; i < end; ++i)
        if(vt_punct_place(vt_cell_cp(cells[i].glyph)) != VtPunctPlace::None) return true;
    return false;
}

// 同上,给不是格数据的整串文字(提示词列)用
static bool vt_needs_place(const std::string &s) {
    for(size_t p = 0; p < s.size();) {
        size_t n = md_utf8_step(s, p);
        if(vt_punct_place(vt_cell_cp(s.substr(p, n - p))) != VtPunctPlace::None) return true;
        p = n;
    }
    return false;
}

// 整串过一遍竖排标点(提示词列不走格数据,自己换码点)
static std::string vt_punct_text(const std::string &s, const lv_font_t *f) {
    std::string out;
    for(size_t p = 0; p < s.size();) {
        size_t n = md_utf8_step(s, p);
        std::string ch = s.substr(p, n - p);
        uint32_t vcp = vt_punct_cp(vt_cell_cp(ch));
        out += (vcp && vt_font_has_glyph(f, vcp)) ? vt_utf8_from_cp(vcp) : ch;
        p = n;
    }
    return out;
}

// 格内偏移(相对格的左上角,格宽是 lh)。字体行高 = 字号(Go-Lava 的 ascent+descent
// 正好是一个 em),所以按 lh 取分数就能落在该落的地方。
static lv_point_t vt_punct_offset(VtPunctPlace pl, int lh) {
    if(pl == VtPunctPlace::TopRight) return {lh / 2, -lh / 2};
    if(pl == VtPunctPlace::MidRight) return {lh / 3, 0};
    return {0, 0};
}

static bool vt_cell_is_ascii_alnum(const std::vector<VerticalCell> &cells, int idx) {
    if(idx < 0 || idx >= (int)cells.size()) return false;
    const std::string &g = cells[idx].glyph;
    if(g.size() != 1) return false;
    unsigned char c = (unsigned char)g[0];
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// ASCII 直引号在竖排里按开合次序换成竖排引号(﹁﹂ / ﹃﹄)。词内的撇号(don't)不动,
// 否则英文单词会被拆成引号对。
static void vt_normalize_quotes(std::vector<VerticalCell> &cells, const lv_font_t *f) {
    bool dq_open = true, sq_open = true;
    for(size_t i = 0; i < cells.size(); ++i) {
        uint32_t cp = vt_cell_cp(cells[i].glyph);
        bool dq = (cp == '"' || cp == 0xFF02);        // " / ＂
        bool sq = (cp == '\'' || cp == 0xFF07);       // ' / ＇
        if(!dq && !sq) continue;
        if(sq && cp == '\'' && vt_cell_is_ascii_alnum(cells, (int)i - 1) &&
           vt_cell_is_ascii_alnum(cells, (int)i + 1))
            continue;
        uint32_t want = dq ? (dq_open ? 0xFE41 : 0xFE42) : (sq_open ? 0xFE43 : 0xFE44);
        if(!vt_font_has_glyph(f, want)) continue;
        cells[i].glyph = vt_utf8_from_cp(want);
        if(dq) dq_open = !dq_open;
        else sq_open = !sq_open;
    }
}

// 逐格过一遍竖排标点:能换竖排字形的换掉,换不了的留在原字形上由渲染层挪位。
static void vt_apply_punct(VerticalData &data, const lv_font_t *f) {
    if(!f) return;
    for(auto &cells : data.cells) {
        vt_normalize_quotes(cells, f);
        for(auto &c : cells) {
            uint32_t cp = vt_cell_cp(c.glyph);
            if(!cp) continue;
            uint32_t vcp = vt_punct_cp(cp);
            if(vcp && vt_font_has_glyph(f, vcp)) c.glyph = vt_utf8_from_cp(vcp);
        }
    }
}

static void editor_vt_create(int x, int y, int w, int h) {
    g_vt_cols.clear();
    g_vt_marks.clear();
    g_vt_cells.clear();
    g_vt_prompt_cols.clear();
    g_vt_box_rects.clear();
    g_vt_deco_rects.clear();
    g_ed_sel_anchor = -1;
    g_vt_scroll = 0;
    g_vt_h = h;

    g_vt_view = lv_obj_create(g_root);
    lv_obj_set_pos(g_vt_view, x, y);
    lv_obj_set_size(g_vt_view, w, h);
    lv_obj_set_style_radius(g_vt_view, 0, 0);
    lv_obj_set_style_border_width(g_vt_view, 0, 0);
    lv_obj_set_style_pad_all(g_vt_view, 0, 0);
    lv_obj_set_style_shadow_width(g_vt_view, 0, 0);
    lv_obj_set_style_bg_color(g_vt_view, g_theme.bg, 0);
    lv_obj_set_style_bg_opa(g_vt_view, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(g_vt_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_vt_view, editor_vt_draw_guides, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(g_vt_view, editor_vt_draw_deco, LV_EVENT_DRAW_MAIN, nullptr);

    g_vt_caret = lv_obj_create(g_vt_view);
    lv_obj_set_style_radius(g_vt_caret, 0, 0);
    lv_obj_set_style_border_width(g_vt_caret, 0, 0);
    lv_obj_set_style_pad_all(g_vt_caret, 0, 0);
    lv_obj_set_style_shadow_width(g_vt_caret, 0, 0);
    lv_obj_set_style_bg_color(g_vt_caret, g_theme.accent, 0);
    lv_obj_set_style_bg_opa(g_vt_caret, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_vt_caret, LV_OBJ_FLAG_SCROLLABLE);
    // 尺寸/坐标是存进样式的,要跑一次布局才落到 coords 上;刷新时要读视口宽度,
    // 不先更新的话拿到的是 0,整列会被排到屏幕外。
    lv_obj_update_layout(g_vt_view);
}

static void editor_vt_refresh() {
    if(!g_vt_view || !g_editor) return;
    g_vt_box_rects.clear();
    g_vt_deco_rects.clear();
    const char *ctxt = lv_textarea_get_text(g_editor);
    std::string text = ctxt ? ctxt : "";
    uint32_t caret_byte = lv_text_encoded_get_byte_id(ctxt ? ctxt : "",
                                                      lv_textarea_get_cursor_pos(g_editor));
    MdDoc ml;
    md_split_caret(text, caret_byte, ml);

    bool md_on = g_settings.markdown_render();
    // 行号变了之后旧折叠不可信,顺手清掉失效项
    for(auto it = g_md_folded.begin(); it != g_md_folded.end();) {
        if(*it >= (int)ml.lines.size() || md_heading_level_of(ml.lines[*it]) == 0)
            it = g_md_folded.erase(it);
        else ++it;
    }

    int lh = lv_font_get_line_height(editor_font());
    int vt_w = lv_obj_get_width(g_vt_view);
    g_vt_m = vertical_metrics(0, 0, vt_w, g_vt_h, lh);

    // 提示写作的题目竖排在右侧:正文整块让出左边,提示自己占最右的若干列。
    // 提示列数与正文列数一样受窗口高度限制,最多占一半宽度,免得正文挤没了。
    std::string prompt = vt_punct_text(editor_prompt_text(), editor_font());
    int prompt_cols = 0;
    if(!prompt.empty()) {
        int chars = 0;
        for(size_t p = 0; p < prompt.size();) {
            p = md_utf8_step(prompt, p);
            ++chars;
        }
        int need = (chars + g_vt_m.rows - 1) / g_vt_m.rows;
        int cap = g_vt_m.cols / 2;
        if(need > cap) need = cap;
        if(need > 0) {
            prompt_cols = need;
            g_vt_m = vertical_metrics(0, 0, vt_w - need * g_vt_m.colAdvance, g_vt_h, lh);
        }
    }

    auto hidden = vertical_fold_hidden(ml.lines, &g_md_folded, md_on);
    // 光标跑进折叠区时把它展开,否则打字看不见
    if(ml.caret_line < (int)hidden.size() && hidden[ml.caret_line]) {
        for(int j = ml.caret_line - 1; j >= 0; --j) {
            if(g_md_folded.count(j)) { g_md_folded.erase(j); break; }
        }
        hidden = vertical_fold_hidden(ml.lines, &g_md_folded, md_on);
    }

    VerticalData data = build_vertical_data(ml.lines, g_vt_m.rows, &hidden, md_on, &g_md_folded,
                                            ml.caret_line, ml.caret_rel);
    vt_apply_punct(data, editor_font());
    g_vt_data = data;
    g_vt_doc = ml;

    int cursorCol = vertical_find_col(data, ml.caret_line, ml.caret_rel);
    bool tw = editor_typewriter();
    if(tw) {
        g_vt_scroll = cursorCol - g_vt_m.cols / 2;
    } else {
        if(cursorCol < g_vt_scroll) g_vt_scroll = cursorCol;
        if(cursorCol >= g_vt_scroll + g_vt_m.cols) g_vt_scroll = cursorCol - g_vt_m.cols + 1;
    }
    int maxScroll = (int)data.cols.size() - g_vt_m.cols;
    if(maxScroll < 0) maxScroll = 0;
    // 打字机模式下上下都不夹:首列/末列也要能停在正中,越界的列由下面的绘制循环
    // 跳过,自然变成留白(和横排那条走同一个思路)。
    if(!tw && g_vt_scroll > maxScroll) g_vt_scroll = maxScroll;
    if(!tw && g_vt_scroll < 0) g_vt_scroll = 0;

    int right = g_vt_m.x + g_vt_m.w - g_vt_m.colAdvance;
    int col_h = g_vt_m.rows * g_vt_m.rowAdvance;
    int cell_i = 0;

    for(int ci = 0; ci < g_vt_m.cols; ++ci) {
        int colIdx = g_vt_scroll + ci;
        if(colIdx < 0 || colIdx >= (int)data.cols.size()) {
            if(ci < (int)g_vt_cols.size()) lv_obj_add_flag(g_vt_cols[ci], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const VerticalCol &col = data.cols[colIdx];
        const std::vector<VerticalCell> &cells = data.cells[col.lineIdx];

        bool styled = false;
        for(int i = col.start; i < col.end && !styled; ++i)
            styled = cells[i].style.bold || cells[i].style.italic || cells[i].style.strike ||
                     cells[i].style.underline || cells[i].style.invert || cells[i].style.emph ||
                     cells[i].style.wavy;
        // 有需要格内挪位的竖排标点时整列也得逐格摆:列标签里没法只挪其中一个字
        bool punct = !styled && vt_col_has_placed_punct(cells, col.start, col.end);

        if(styled || punct) {
            // 带样式的列整列改成逐格标签:每格一个字符、用自己的字体和颜色,
            // 伪粗体的格再多画一层偏移 1px 的副本。
            lv_obj_add_flag(vt_col_at(ci), LV_OBJ_FLAG_HIDDEN);
            int cx = right - ci * g_vt_m.colAdvance;
            int row = 0;
            for(int i = col.start; i < col.end; ++i, ++row) {
                const VerticalCell &c = cells[i];
                bool faux = false;
                const lv_font_t *f = md_style_font(c.style, faux);
                int cy = g_vt_m.y + row * g_vt_m.rowAdvance;
                if(c.style.invert) {
                    // 反白块铺满整格行距,否则连续反白的两字之间会露出 2px 缝
                    bool next_inv = (i + 1 < col.end) && cells[i + 1].style.invert;
                    int bh = next_inv ? g_vt_m.rowAdvance : lh;
                    g_vt_box_rects.push_back({cx, cy, cx + lh - 1, cy + bh - 1});
                }
                VtPunctPlace pl = vt_punct_place(vt_cell_cp(c.glyph));
                lv_point_t off = vt_punct_offset(pl, lh);
                for(int d = 0; d < (faux ? 2 : 1); ++d) {
                    lv_obj_t *g = vt_cell_at(cell_i++);
                    lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_text_font(g, f, 0);
                    lv_obj_set_style_text_decor(g, LV_TEXT_DECOR_NONE, 0);
                    lv_obj_set_style_text_color(g, c.style.invert ? g_theme.bg : vt_kind_color(c.kind), 0);
                    lv_obj_set_pos(g, cx + d + off.x, cy + off.y);
                    lv_obj_set_size(g, lh, g_vt_m.rowAdvance);
                    // 省略号画成小方点,横排那三个点就别再画了
                    lv_label_set_text(g, pl == VtPunctPlace::Stack ? "" : c.glyph.c_str());
                }
                if(pl == VtPunctPlace::Stack) {
                    // 省略号:横排的三点竖过来,平移做不到,画成三个小方点
                    int px = cx + lh / 2 - 1;
                    for(int py : {cy + 3, cy + lh / 2 - 1, cy + lh - 5})
                        g_vt_deco_rects.push_back({px, py, px + 2, py + 2});
                }
            }
            // 下划线/删除线是竖线,波浪线是折线:同意样式的连续格并成一条
            for(int flag = 0; flag < 3; ++flag) {
                int rs = -1;
                for(int i = col.start; i <= col.end; ++i) {
                    const MdStyle *s = i < col.end ? &cells[i].style : nullptr;
                    bool on = s && (flag == 0 ? s->underline : (flag == 1 ? s->strike : s->wavy));
                    if(on && rs < 0) rs = i;
                    if(rs >= 0 && !on) {
                        int row0 = rs - col.start, row1 = i - col.start;
                        int y0 = g_vt_m.y + row0 * g_vt_m.rowAdvance + 2;
                        int hh = (row1 - row0 - 1) * g_vt_m.rowAdvance + lh - 4;
                        if(hh > 0) {
                            if(flag == 2) {
                                vt_push_wave(cx - 4, y0, hh, g_vt_deco_rects);
                            } else {
                                int x0 = flag == 0 ? cx - 2 : cx + lh / 2;
                                int w0 = flag == 0 ? 1 : 2;
                                g_vt_deco_rects.push_back({x0, y0, x0 + w0 - 1, y0 + hh - 1});
                            }
                        }
                        rs = -1;
                    }
                }
            }
            for(int i = col.start; i < col.end; ++i) {
                if(!cells[i].style.emph) continue;
                int cy = g_vt_m.y + (i - col.start) * g_vt_m.rowAdvance + lh / 2 - 1;
                g_vt_deco_rects.push_back({cx - 6, cy, cx - 4, cy + 2});
            }
            continue;
        }

        std::string disp;
        for(int i = col.start; i < col.end; ++i) {
            if(i > col.start) disp += "\n";
            disp += cells[i].glyph;
        }
        lv_obj_t *o = vt_col_at(ci);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(o, right - ci * g_vt_m.colAdvance, g_vt_m.y);
        lv_obj_set_size(o, lh, col_h);
        lv_obj_set_style_text_line_space(o, g_vt_m.rowAdvance - lh, 0);
        lv_obj_set_style_text_color(
            o, vt_kind_color(col.start < col.end ? cells[col.start].kind : VerticalCellKind::Normal), 0);
        lv_label_set_text(o, disp.c_str());
    }
    for(int ci = g_vt_m.cols; ci < (int)g_vt_cols.size(); ++ci)
        lv_obj_add_flag(g_vt_cols[ci], LV_OBJ_FLAG_HIDDEN);

    // 提示词的列:从最右列往左排,每列顶天立地放满 rows 个字
    if(prompt_cols > 0) {
        std::vector<std::string> parts;
        std::string cur;
        int n = 0;
        for(size_t p = 0; p < prompt.size();) {
            size_t step = md_utf8_step(prompt, p);
            cur += prompt.substr(p, step - p);
            p = step;
            if(++n == g_vt_m.rows && (int)parts.size() + 1 < prompt_cols) {
                parts.push_back(cur);
                cur.clear();
                n = 0;
            }
        }
        if(!cur.empty()) parts.push_back(cur);
        int prow = g_vt_m.x + vt_w - g_vt_m.colAdvance;
        int pcol_h = g_vt_m.rows * g_vt_m.rowAdvance;
        for(int k = 0; k < (int)parts.size(); ++k) {
            int px = prow - k * g_vt_m.colAdvance;
            lv_obj_t *o = vt_prompt_col_at(k);
            if(vt_needs_place(parts[k])) {
                // 这一列里有要挪位的标点,整列逐字摆(与正文同一套偏移)
                lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
                int row = 0;
                for(size_t p = 0; p < parts[k].size(); ++row) {
                    size_t step = md_utf8_step(parts[k], p);
                    std::string ch = parts[k].substr(p, step - p);
                    p = step;
                    VtPunctPlace pl = vt_punct_place(vt_cell_cp(ch));
                    lv_point_t off = vt_punct_offset(pl, lh);
                    int cy = g_vt_m.y + row * g_vt_m.rowAdvance;
                    lv_obj_t *g = vt_cell_at(cell_i++);
                    lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_text_font(g, editor_font(), 0);
                    lv_obj_set_style_text_decor(g, LV_TEXT_DECOR_NONE, 0);
                    lv_obj_set_style_text_color(g, g_theme.muted, 0);
                    lv_obj_set_pos(g, px + off.x, cy + off.y);
                    lv_obj_set_size(g, lh, g_vt_m.rowAdvance);
                    lv_label_set_text(g, pl == VtPunctPlace::Stack ? "" : ch.c_str());
                    if(pl == VtPunctPlace::Stack) {
                        int dx = px + lh / 2 - 1;
                        for(int py : {cy + 3, cy + lh / 2 - 1, cy + lh - 5})
                            g_vt_deco_rects.push_back({dx, py, dx + 2, py + 2});
                    }
                }
                continue;
            }
            std::string disp;
            for(size_t p = 0; p < parts[k].size();) {
                size_t step = md_utf8_step(parts[k], p);
                if(!disp.empty()) disp += "\n";
                disp += parts[k].substr(p, step - p);
                p = step;
            }
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(o, px, g_vt_m.y);
            lv_obj_set_size(o, lh, pcol_h);
            lv_obj_set_style_text_line_space(o, g_vt_m.rowAdvance - lh, 0);
            lv_label_set_text(o, disp.c_str());
        }
        for(int k = (int)parts.size(); k < (int)g_vt_prompt_cols.size(); ++k)
            lv_obj_add_flag(g_vt_prompt_cols[k], LV_OBJ_FLAG_HIDDEN);
    } else {
        for(lv_obj_t *o : g_vt_prompt_cols) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    // 提示列也用格标签,残留的空闲标签等它排完再收
    for(int k = cell_i; k < (int)g_vt_cells.size(); ++k)
        lv_obj_add_flag(g_vt_cells[k], LV_OBJ_FLAG_HIDDEN);

    // 选区:锚点到光标之间的格反白
    int nmark = 0;
    if(g_ed_sel_anchor >= 0 && g_ed_sel_anchor != (int)caret_byte) {
        int lo = g_ed_sel_anchor < (int)caret_byte ? g_ed_sel_anchor : (int)caret_byte;
        int hi = g_ed_sel_anchor < (int)caret_byte ? (int)caret_byte : g_ed_sel_anchor;
        auto abs_of = [&](int line, int off) {
            if(line < 0 || line >= (int)ml.start.size()) return 0;
            return (int)ml.start[line] + off;
        };

        for(int ci = 0; ci < g_vt_m.cols; ++ci) {
            int colIdx = g_vt_scroll + ci;
            if(colIdx < 0 || colIdx >= (int)data.cols.size()) continue;
            const VerticalCol &col = data.cols[colIdx];
            const std::vector<VerticalCell> &cells = data.cells[col.lineIdx];
            int runStart = -1, runEnd = -1;
            auto flush_run = [&]() {
                if(runStart < 0) return;
                lv_obj_t *o = vt_mark_at(nmark++);
                lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(o, right - ci * g_vt_m.colAdvance - 1,
                               g_vt_m.y + runStart * g_vt_m.rowAdvance - 1);
                lv_obj_set_size(o, lh + 2, (runEnd - runStart) * g_vt_m.rowAdvance + 2);
                lv_obj_move_to_index(o, -1);
                runStart = runEnd = -1;
            };
            for(int i = col.start; i < col.end; ++i) {
                int a = abs_of(col.lineIdx, cells[i].start);
                bool sel = a >= lo && a < hi;
                if(sel) {
                    if(runStart < 0) runStart = i - col.start;
                    runEnd = i - col.start + 1;
                } else {
                    flush_run();
                }
            }
            flush_run();
        }
    }
    for(int k = nmark; k < (int)g_vt_marks.size(); ++k)
        lv_obj_add_flag(g_vt_marks[k], LV_OBJ_FLAG_HIDDEN);

    // 光标
    if(g_vt_caret) {
        if(cursorCol >= g_vt_scroll && cursorCol < g_vt_scroll + g_vt_m.cols) {
            const VerticalCol &col = data.cols[cursorCol];
            int row = vertical_cell_row(data.cells[col.lineIdx], ml.caret_rel) - col.start;
            if(row >= g_vt_m.rows) row = g_vt_m.rows - 1;
            if(row < 0) row = 0;
            int ci = cursorCol - g_vt_scroll;
            lv_obj_remove_flag(g_vt_caret, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(g_vt_caret, right - ci * g_vt_m.colAdvance + 2,
                           g_vt_m.y + row * g_vt_m.rowAdvance + g_vt_m.rowAdvance - 4);
            lv_obj_set_size(g_vt_caret, lh - 4 > 2 ? lh - 4 : 2, 3);
            lv_obj_move_to_index(g_vt_caret, -1);
        } else {
            lv_obj_add_flag(g_vt_caret, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---------------------------------------------------------------------------
// 竖排的格级导航与选区。选区用整篇文本里的字节区间表示(锚点 + 光标),
// 因为 lv_textarea 里文本本就是一条含 \n 的扁平字符串。
// ---------------------------------------------------------------------------

static void vt_set_caret_byte(int abs) {
    const char *t = g_editor ? lv_textarea_get_text(g_editor) : nullptr;
    std::string s = t ? t : "";
    if(abs < 0) abs = 0;
    if(abs > (int)s.size()) abs = (int)s.size();
    lv_textarea_set_cursor_pos(g_editor, lv_text_encoded_get_char_id(s.c_str(), (uint32_t)abs));
}

// 沿文本前后挪一个 UTF-8 字符(跨行,与 lv_textarea 光标步进一致)。
static int vt_step_byte(int abs, int dir) {
    const char *t = g_editor ? lv_textarea_get_text(g_editor) : nullptr;
    std::string s = t ? t : "";
    if(dir < 0) {
        if(abs <= 0) return 0;
        int p = abs - 1;
        while(p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) --p;
        return p;
    }
    if(abs >= (int)s.size()) return (int)s.size();
    return (int)md_utf8_step(s, (size_t)abs);
}

static void editor_sel_range(int &lo, int &hi) {
    int caret = editor_caret_byte();
    if(g_ed_sel_anchor < 0) { lo = hi = caret; return; }
    lo = g_ed_sel_anchor < caret ? g_ed_sel_anchor : caret;
    hi = g_ed_sel_anchor < caret ? caret : g_ed_sel_anchor;
}

static std::string editor_selected_text() {
    int lo, hi;
    editor_sel_range(lo, hi);
    const char *t = g_editor ? lv_textarea_get_text(g_editor) : nullptr;
    std::string s = t ? t : "";
    if(lo < 0) lo = 0;
    if(hi > (int)s.size()) hi = (int)s.size();
    if(hi <= lo) return "";
    return s.substr(lo, hi - lo);
}

static void editor_delete_selection() {
    int lo, hi;
    editor_sel_range(lo, hi);
    if(hi <= lo) { g_ed_sel_anchor = -1; return; }
    const char *t = g_editor ? lv_textarea_get_text(g_editor) : nullptr;
    std::string s = t ? t : "";
    s.erase(lo, hi - lo);
    editor_record_undo();
    lv_textarea_set_text(g_editor, s.c_str());
    g_ed_sel_anchor = -1;
    vt_set_caret_byte(lo);
}

// 纯文本模式(没有 Markdown/竖排叠加层)时,选区交给 lv_textarea 自带的标签
// 选区来显示:把字节区间换算成字符下标设进去即可。
static void editor_sel_sync_label() {
    if(!g_editor) return;
    lv_obj_t *l = lv_textarea_get_label(g_editor);
    if(!l) return;
    int lo, hi;
    editor_sel_range(lo, hi);
    const char *t = lv_textarea_get_text(g_editor);
    if(!t || hi <= lo) {
        lv_label_set_text_selection_start(l, LV_DRAW_LABEL_NO_TXT_SEL);
        lv_label_set_text_selection_end(l, LV_DRAW_LABEL_NO_TXT_SEL);
        return;
    }
    lv_label_set_text_selection_start(l, lv_text_encoded_get_char_id(t, (uint32_t)lo));
    lv_label_set_text_selection_end(l, lv_text_encoded_get_char_id(t, (uint32_t)hi));
}

// 纯文本模式:让 lv_textarea 自带的标签能画出选区色块(否则选中了也看不见)。
static void editor_sel_style_plain() {
    if(!g_editor) return;
    lv_obj_t *l = lv_textarea_get_label(g_editor);
    if(!l) return;
    lv_obj_set_style_bg_color(l, g_theme.accent, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(l, g_theme.bg, LV_PART_SELECTED);
}

// 把润色结果写回编辑器:有选区就只替换选区,否则整篇替换。
static void editor_apply_polish(const std::string &text) {
    if(!g_editor) return;
    int lo, hi;
    editor_sel_range(lo, hi);
    const char *t = lv_textarea_get_text(g_editor);
    std::string s = t ? t : "";
    editor_record_undo();
    if(hi > lo) {
        s = s.substr(0, (size_t)lo) + text + s.substr((size_t)hi);
        g_ed_sel_anchor = -1;
        lv_textarea_set_text(g_editor, s.c_str());
        vt_set_caret_byte(lo + (int)text.size());
    } else {
        g_ed_sel_anchor = -1;
        lv_textarea_set_text(g_editor, text.c_str());
        lv_textarea_set_cursor_pos(g_editor, LV_TEXTAREA_CURSOR_LAST);
    }
}

// ---------------------------------------------------------------------------
// AI 润色:选区(Ctrl+O 前先 Shift+方向键选中)或整篇送 DeepSeek;结果页里
// R 可补一条自定义指令重润色,Enter 把结果写回编辑器。
// 和查找框一样是模态面板:打开期间按键全由它接管,不触发整屏 render()。
// ---------------------------------------------------------------------------

static bool g_polish_ime_saved = false;

static void close_polish_panel() {
    if(g_polish_panel) lv_obj_delete(g_polish_panel);
    g_polish_panel = nullptr;
    g_polish_preview = nullptr;
    g_polish_instr = nullptr;
    g_polish_instr_mode = false;
    g_linux_ime.set_active(g_polish_ime_saved);
    if(g_editor) lv_group_focus_obj(g_editor);
    update_ime_bar();
}

static void open_polish_result() {
    close_polish_panel();
    g_polish_panel = box(g_root, 60, 30, 904, 496, false);
    lv_obj_set_style_bg_color(g_polish_panel, g_theme.bg, 0);
    lv_obj_set_style_border_width(g_polish_panel, 2, 0);
    label(g_polish_panel, g_polish_is_sel ? "AI润色(选区)" : "AI润色", 8, 8, 500, 32);
    g_polish_preview = lv_textarea_create(g_polish_panel);
    lv_obj_set_pos(g_polish_preview, 8, 44);
    lv_obj_set_size(g_polish_preview, 888, 440);
    base_style(g_polish_preview);
    lv_obj_set_style_border_width(g_polish_preview, 1, 0);
    lv_textarea_set_one_line(g_polish_preview, false);
    lv_obj_remove_flag(g_polish_preview, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_textarea_set_cursor_click_pos(g_polish_preview, false);
    lv_textarea_set_text(g_polish_preview, g_polish_result.c_str());
    lv_obj_scroll_to_y(g_polish_preview, 0, LV_ANIM_OFF);
    lv_obj_move_foreground(g_polish_panel);
    set_status("Enter 应用 · R 改指令重润色 · Esc 取消");
}

static void open_polish_instr() {
    close_polish_panel();
    g_polish_instr_mode = true;
    g_polish_panel = box(g_root, 120, 190, 784, 210, false);
    lv_obj_set_style_bg_color(g_polish_panel, g_theme.bg, 0);
    lv_obj_set_style_border_width(g_polish_panel, 2, 0);
    label(g_polish_panel, "AI润色 · 优化指令", 8, 8, 600, 32);
    lv_obj_t *hint = label(g_polish_panel, "默认:轻度润色,保留原文风格与叙事,以语义通顺为主。", 8, 46, 760, 28);
    lv_obj_set_style_text_color(hint, g_theme.muted, 0);
    label(g_polish_panel, "补充指令:", 8, 84, 110, 32);
    g_polish_instr = lv_textarea_create(g_polish_panel);
    lv_obj_set_pos(g_polish_instr, 120, 78);
    lv_obj_set_size(g_polish_instr, 648, 44);
    base_style(g_polish_instr);
    lv_textarea_set_one_line(g_polish_instr, true);
    lv_textarea_set_placeholder_text(g_polish_instr, "留空则按默认原则润色");
    lv_textarea_set_text(g_polish_instr, g_polish_instr_text.c_str());
    lv_textarea_set_cursor_pos(g_polish_instr, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_state(g_polish_instr, LV_STATE_FOCUSED);
    lv_group_focus_obj(g_polish_instr);
    lv_obj_move_foreground(g_polish_panel);
    g_linux_ime.set_active(true);
    update_ime_bar();
    set_status("Enter 重润色 · Esc 返回结果");
}

// 拿当前指令重跑一次润色,结果仍回到结果页。
static void polish_run() {
    set_status("正在润色...");
    lv_timer_handler();
    std::string out, err;
    if(!deepseek_polish_text(g_polish_source, g_polish_instr_text, out, err)) {
        close_polish_panel();
        set_status(err);
        return;
    }
    g_polish_result = out;
    open_polish_result();
}

// Ctrl+O 入口:有选区就润色选区,否则润色整篇。
static void polish_begin() {
    if(!g_editor) return;
    std::string sel = editor_selected_text();
    bool has_sel = !sel.empty();
    const char *t = lv_textarea_get_text(g_editor);
    g_polish_source = has_sel ? sel : std::string(t ? t : "");
    if(!has_sel) g_ed_sel_anchor = -1;   // 整篇润色:清掉可能残留的选区
    bool blank = true;
    for(char c : g_polish_source)
        if(c != ' ' && c != '\n' && c != '\r' && c != '\t') { blank = false; break; }
    if(blank) { set_status("编辑区内容为空"); return; }
    g_polish_is_sel = has_sel;
    g_polish_instr_text.clear();
    g_polish_ime_saved = g_linux_ime.active();
    polish_run();
}

// 返回 true 表示面板已消费该键。
static bool polish_panel_key(int key) {
    if(!g_polish_panel) return false;

    if(g_polish_instr_mode) {
        if(key == KEY_IME_TOGGLE) { g_linux_ime.toggle(); update_ime_bar(); return true; }
        std::string ime_out;
        if(g_linux_ime.handle_key(key, ime_out)) {
            if(!ime_out.empty() && g_polish_instr) lv_textarea_add_text(g_polish_instr, ime_out.c_str());
            update_ime_bar();
            return true;
        }
        if(key == 27) { open_polish_result(); return true; }   // Esc 回到结果页
        if(key == '\n' || key == '\r') {
            if(g_polish_instr) g_polish_instr_text = lv_textarea_get_text(g_polish_instr);
            polish_run();
            return true;
        }
        if(key == KEY_LEFT) lv_textarea_cursor_left(g_polish_instr);
        else if(key == KEY_RIGHT) lv_textarea_cursor_right(g_polish_instr);
        else if(key == KEY_HOME) lv_textarea_set_cursor_pos(g_polish_instr, 0);
        else if(key == KEY_END) lv_textarea_set_cursor_pos(g_polish_instr, LV_TEXTAREA_CURSOR_LAST);
        else if(key == 8 || key == 127) lv_textarea_delete_char(g_polish_instr);
        else if(key >= 32 && key < 127) lv_textarea_add_char(g_polish_instr, (uint32_t)key);
        update_ime_bar();
        return true;
    }

    if(key == '\n' || key == '\r') {          // 应用结果
        std::string r = g_polish_result;
        close_polish_panel();
        editor_apply_polish(r);
        set_status(g_polish_is_sel ? "已替换选区" : "已替换全文");
        return true;
    }
    if(key == 'r' || key == 'R') { open_polish_instr(); return true; }
    if(key == 27 || key == 'q' || key == 'Q') {
        close_polish_panel();
        set_status(editor_status_text());
        return true;
    }
    if(g_polish_preview) {
        if(key == KEY_UP) lv_obj_scroll_by(g_polish_preview, 0, 40, LV_ANIM_OFF);
        else if(key == KEY_DOWN) lv_obj_scroll_by(g_polish_preview, 0, -40, LV_ANIM_OFF);
        else if(key == KEY_PAGE_UP) lv_obj_scroll_by(g_polish_preview, 0, 400, LV_ANIM_OFF);
        else if(key == KEY_PAGE_DOWN) lv_obj_scroll_by(g_polish_preview, 0, -400, LV_ANIM_OFF);
    }
    return true;
}

// 列内左右挪一格(dcol > 0 是往左一列),行内保持同一格位。
static void vt_move_col(int dcol) {
    if(g_vt_data.cols.empty()) return;
    int curCol = vertical_find_col(g_vt_data, g_vt_doc.caret_line, g_vt_doc.caret_rel);
    int dstCol = curCol + dcol;
    if(dstCol < 0) dstCol = 0;
    if(dstCol >= (int)g_vt_data.cols.size()) dstCol = (int)g_vt_data.cols.size() - 1;
    if(dstCol == curCol) return;
    int row = vertical_cell_row(g_vt_data.cells[g_vt_doc.caret_line], g_vt_doc.caret_rel) -
              g_vt_data.cols[curCol].start;
    const VerticalCol &dst = g_vt_data.cols[dstCol];
    if(dst.lineIdx >= (int)g_vt_doc.start.size()) return;
    int rel = vertical_row_to_byte(g_vt_data.cells[dst.lineIdx], dst.start, dst.end, row);
    vt_set_caret_byte((int)g_vt_doc.start[dst.lineIdx] + rel);
}

// 竖排按键映射:Up/Down = 前后一字符,Left/Right = 左右一列,PageUp/PageDown =
// 一屏列;带 Shift 时先落选区锚点。返回 true 表示该键已由竖排消费。
static bool editor_vt_key(int key) {
    int dcol = 0, dchar = 0;
    bool shift = false, move = false;
    switch(key) {
    case KEY_UP: dchar = -1; move = true; break;
    case KEY_DOWN: dchar = 1; move = true; break;
    case KEY_LEFT: dcol = 1; move = true; break;
    case KEY_RIGHT: dcol = -1; move = true; break;
    case KEY_PAGE_UP: dcol = -g_vt_m.cols; move = true; break;
    case KEY_PAGE_DOWN: dcol = g_vt_m.cols; move = true; break;
    case KEY_SHIFT_UP: dchar = -1; shift = move = true; break;
    case KEY_SHIFT_DOWN: dchar = 1; shift = move = true; break;
    case KEY_SHIFT_LEFT: dcol = 1; shift = move = true; break;
    case KEY_SHIFT_RIGHT: dcol = -1; shift = move = true; break;
    default: return false;
    }
    if(!move) return false;
    if(shift) {
        if(g_ed_sel_anchor < 0) g_ed_sel_anchor = editor_caret_byte();
    } else {
        g_ed_sel_anchor = -1;
    }
    if(dchar) vt_set_caret_byte(vt_step_byte(editor_caret_byte(), dchar));
    if(dcol) vt_move_col(dcol);
    return true;
}

static void editor_toggle_fold() {
    if((!g_md_view && !g_vt_view) || !g_editor) {
        set_status("先打开设置里的 Markdown 渲染");
        return;
    }
    if(!g_settings.markdown_render()) { set_status("先打开设置里的 Markdown 渲染"); return; }
    const char *ctxt = lv_textarea_get_text(g_editor);
    std::string text = ctxt ? ctxt : "";
    uint32_t caret_byte = lv_text_encoded_get_byte_id(ctxt ? ctxt : "", lv_textarea_get_cursor_pos(g_editor));
    MdDoc ml;
    md_split_caret(text, caret_byte, ml);

    int target = -1;
    for(int j = ml.caret_line; j >= 0; --j) {
        if(md_heading_level_of(ml.lines[j]) > 0) { target = j; break; }
    }
    if(target < 0) { set_status("光标之前没有标题"); return; }
    if(g_md_folded.count(target)) {
        g_md_folded.erase(target);
        set_status("已展开标题");
    } else {
        g_md_folded.insert(target);
        set_status("已折叠标题 · Ctrl+T 再按一次展开");
    }
    if(g_vt_view) editor_vt_refresh();
    else editor_md_refresh();
}

// 打字机模式:光标行钉在「编辑区正中再往上一行」,随输入滚动;聚焦模式则在这儿更新压暗罩。
// 纯文本视图没有叠加层可用,留白做不出来——textarea 的 padding 同时就是裁剪区,上下各留
// 一截会把视口压成 0,整屏变黑。所以这里的落点要夹进 [0, 可滚距离]:文档首尾那一屏做不到
// 居中,但也不像以前那样算出负数目标、把内容顶成负偏移,把光标顶到视口外面去。
static void editor_follow_cursor() {
    if(!g_editor || g_md_view || g_vt_view) return;
    if((int)lv_obj_get_style_pad_top(g_editor, 0) != 0 ||
       (int)lv_obj_get_style_pad_bottom(g_editor, 0) != 0) {
        lv_obj_set_style_pad_top(g_editor, 0, 0);
        lv_obj_set_style_pad_bottom(g_editor, 0, 0);
    }
    lv_obj_t *ta_label = lv_textarea_get_label(g_editor);
    if(!ta_label) return;
    lv_point_t p {};
    lv_label_get_letter_pos(ta_label, lv_textarea_get_cursor_pos(g_editor), &p);
    const lv_font_t *f = lv_obj_get_style_text_font(g_editor, 0);
    int line_h = (f ? lv_font_get_line_height(f) : 0) + (int)lv_obj_get_style_text_line_space(g_editor, 0);

    // 聚焦模式(和 markdown 路径共用同一套罩子):p 是 label 内坐标,label 的屏幕坐标
    // 已经把滚动算进去了,两者相加就是光标行的绝对位置。打字机模式下要先滚再取坐标,
    // 否则暗罩会停在滚动之前的位置、和光标差出滚动的量。
    if(editor_typewriter()) {
        int view_y = (int)lv_obj_get_y(g_editor);
        int center = (view_y + chrome_bottom()) / 2;
        int target = view_y + (int)p.y - (center - line_h);
        if(target < 0) target = 0;
        int maxs = (int)lv_obj_get_scroll_bottom(g_editor);
        if(target > maxs) target = maxs;
        int cur = (int)lv_obj_get_scroll_y(g_editor);
        if(cur != target) lv_obj_scroll_by(g_editor, 0, cur - target, LV_ANIM_OFF);
    }

    lv_area_t vp {}, la {};
    lv_obj_get_coords(g_editor, &vp);
    lv_obj_get_coords(ta_label, &la);
    lv_area_t ca {vp.x1, la.y1 + (int)p.y, vp.x2, la.y1 + (int)p.y + line_h - 1};
    focus_dim_update(true, ca, vp);
}

static void handle_editor_keys(int key);

static void handle_editor(int key) {
    handle_editor_keys(key);
    if(g_vt_view) editor_vt_refresh();
    else if(g_md_view) editor_md_refresh();
    else { editor_sel_sync_label(); editor_follow_cursor(); }
    if(g_file_panel_state.active) draw_file_panel();
}

// 行尾回车时,若当前行是列表项,续行带上递增后的同款标记(空列表项只换行)。
static std::string editor_list_continue() {
    if(!g_editor) return "\n";
    std::string full = lv_textarea_get_text(g_editor);
    int cb = editor_caret_byte();
    size_t ls = 0;
    if(cb > 0) {
        size_t p = full.rfind('\n', (size_t)cb - 1);
        ls = (p == std::string::npos) ? 0 : p + 1;
    }
    size_t le = full.find('\n', (size_t)cb);
    if(le == std::string::npos) le = full.size();
    if((size_t)cb != le) return "\n";  // 只在行尾续行
    std::string cur = full.substr(ls, le - ls);
    MdListMarker m = md_list_marker(cur);
    if(!m.ok) return "\n";
    if(cur.substr((size_t)(m.start + m.len)).find_first_not_of(" \t") == std::string::npos)
        return "\n";  // 空列表项:只换行,不续标记
    std::string lead = cur.substr(0, (size_t)m.start);
    if(m.task) return "\n" + lead + "- [ ] ";
    if(m.ordered) {
        std::string after = cur.substr((size_t)m.start + (size_t)m.num_len,
                                       (size_t)m.len - (size_t)m.num_len);
        return "\n" + lead + (m.cn ? md_cn_numeral(m.num + 1) : std::to_string(m.num + 1)) + after;
    }
    return "\n" + lead + cur.substr((size_t)m.start, 1) + " ";  // 保留 -/*/+
}

static void handle_editor_keys(int key) {
    if(handle_file_panel_key(key)) return;
    if(g_editor_exit_asking) {
        if(key == '\n' || key == '\r') {
            g_editor_exit_asking = false;
            save_editor_text();
            editor_leave();
        } else if(key == 'n' || key == 'N') {
            g_editor_exit_asking = false;
            editor_leave();
        } else if(key == 27 || key == 'q') {
            g_editor_exit_asking = false;
            if(g_editor_exit_dlg) {
                lv_obj_delete(g_editor_exit_dlg);
                g_editor_exit_dlg = nullptr;
            }
        }
        return;
    }
    if(g_polish_panel) { polish_panel_key(key); return; }
    if(g_search_panel) {
        if(key == KEY_IME_TOGGLE) {
            g_linux_ime.toggle();
            update_ime_bar();
            return;
        }
        lv_obj_t *fld = g_search_focus_rep ? g_search_replace : g_search_input;
        std::string ime_out;
        if(g_linux_ime.handle_key(key, ime_out)) {
            if(!ime_out.empty() && fld) lv_textarea_add_text(fld, ime_out.c_str());
            update_ime_bar();
            return;
        }
        if(key == 27) {
            close_search_panel();
            return;
        }
        if(key == 0x09) {  // Tab:切换查找/替换字段
            g_search_focus_rep = !g_search_focus_rep;
            lv_group_focus_obj(g_search_focus_rep ? g_search_replace : g_search_input);
            return;
        }
        if(key == 0x12) { search_replace_current(); return; }   // Ctrl+R 替换当前
        if(key == 0x01) { search_replace_all(); return; }       // Ctrl+A 全部替换
        if(key == KEY_SEARCH || key == '\n' || key == '\r' || key == KEY_DOWN) {
            search_step(true);
            return;
        }
        if(key == KEY_CTRL_ENTER || key == KEY_UP) {
            search_step(false);
            return;
        }
        if(key == KEY_LEFT) lv_textarea_cursor_left(fld);
        else if(key == KEY_RIGHT) lv_textarea_cursor_right(fld);
        else if(key == KEY_HOME) lv_textarea_set_cursor_pos(fld, 0);
        else if(key == KEY_END) lv_textarea_set_cursor_pos(fld, LV_TEXTAREA_CURSOR_LAST);
        else if(key == 8 || key == 127) lv_textarea_delete_char(fld);
        else if(key >= 32 && key < 127) lv_textarea_add_char(fld, (uint32_t)key);
        return;
    }
    if(key == KEY_IME_TOGGLE) {
        g_linux_ime.toggle();
        update_ime_bar();
        return;
    }
    std::string ime_out;
    if(g_linux_ime.handle_key(key, ime_out)) {
        if(!ime_out.empty()) {
            if(g_ed_sel_anchor >= 0) editor_delete_selection();
            else editor_record_undo();
            lv_textarea_add_text(g_editor, ime_out.c_str());
        }
        update_ime_bar();
        return;
    }
    if(key == KEY_HELP) { goto_screen(Screen::Help); return; }
    if(key == KEY_SEARCH) { open_search_panel(); return; }
    if(key == 0x05 && !g_file_edit_path.empty()) {
        g_file_panel_state.active = true;
        g_file_panel_state.prompt = false;
        g_file_panel_state.confirmDelete = false;
        g_file_panel_state.message.clear();
        file_panel_reload();
        draw_file_panel();
        return;
    }
    if(key == 0x13) { save_editor_text(); return; }
    if(key == 0x06) {
        std::string text = g_editor ? lv_textarea_get_text(g_editor) : "";
        std::string msg;
        set_status("正在发送Flomo...");
        lv_timer_handler();
        bool ok = flomo_send_text(text, msg);
        set_status(ok ? msg : msg);
        return;
    }
    if(key == 27) {
        editor_exit_target_from_state();
        // 自动保存开着就不用问,内容迟早会落到文件里
        if(editor_dirty() && !g_settings.auto_save()) {
            g_editor_exit_asking = true;
            draw_editor_exit_confirm();
            return;
        }
        if(editor_dirty()) save_editor_text();
        editor_leave();
        return;
    }
    if(g_quick_slot >= 0 && key == 0x0E) { open_quick_editor((g_quick_slot + 1) % 10); return; }
    if(g_quick_slot >= 0 && key == 0x10) { open_quick_editor((g_quick_slot + 9) % 10); return; }
    if(g_settings.app_mode() == "journal" && key == 0x10) {
        std::string prompt, err;
        set_status("正在生成提示...");
        lv_timer_handler();
        if(!deepseek_generate_prompt(prompt, err)) prompt = random_builtin_prompt();
        g_prompt = prompt;
        g_edit_file.clear();
        set_status(editor_status_text());
        return;
    }
    if(key == 0x0F) { polish_begin(); return; }
    if(key == 0x01 && g_editor) { g_ed_sel_anchor = 0; lv_textarea_set_cursor_pos(g_editor, LV_TEXTAREA_CURSOR_LAST); return; }
    if(key == 0x03 && g_editor) {
        if(g_ed_sel_anchor >= 0) {
            std::string sel = editor_selected_text();
            if(!sel.empty()) { g_clipboard = sel; set_status("已复制选中文本"); return; }
        }
        g_clipboard = lv_textarea_get_text(g_editor);
        set_status("已复制全部文本");
        return;
    }
    if(key == 0x18 && g_editor) {
        if(g_ed_sel_anchor >= 0) {
            std::string sel = editor_selected_text();
            if(!sel.empty()) { g_clipboard = sel; editor_delete_selection(); set_status("已剪切选中文本"); return; }
        }
        editor_record_undo();
        g_clipboard = lv_textarea_get_text(g_editor);
        lv_textarea_set_text(g_editor, "");
        return;
    }
    if(key == 0x16 && g_editor) {
        if(g_ed_sel_anchor >= 0) editor_delete_selection();
        else editor_record_undo();
        lv_textarea_add_text(g_editor, g_clipboard.c_str());
        return;
    }
    if(key == 0x19) {
        if(save_editor_text_for_history()) open_history(g_edit_file, Screen::Editor);
        else set_status("保存失败，无法查看历史");
        return;
    }
    if(key == 0x14) { editor_toggle_fold(); return; }
    if(key == 0x1A) { set_status(editor_undo() ? "已撤销" : "没有可撤销内容"); return; }
    if(key == KEY_REDO || key == 0x12) { set_status(editor_redo() ? "已重做" : "没有可重做内容"); return; }
    // 竖排:方向键按「格」走(上下 = 前后一字符,左右 = 左右一列),Shift 拉选区。
    if(g_vt_view && editor_vt_key(key)) {
        update_ime_bar();
        return;
    }
    // 横排:Shift+方向键拉选区,先落锚点,退回锚点即取消。
    if(key == KEY_SHIFT_LEFT || key == KEY_SHIFT_RIGHT || key == KEY_SHIFT_UP || key == KEY_SHIFT_DOWN) {
        if(g_ed_sel_anchor < 0) g_ed_sel_anchor = editor_caret_byte();
        if(key == KEY_SHIFT_LEFT) lv_textarea_cursor_left(g_editor);
        else if(key == KEY_SHIFT_RIGHT) lv_textarea_cursor_right(g_editor);
        else if(key == KEY_SHIFT_UP) lv_textarea_cursor_up(g_editor);
        else lv_textarea_cursor_down(g_editor);
        if(g_ed_sel_anchor == editor_caret_byte()) g_ed_sel_anchor = -1;
        update_ime_bar();
        return;
    }
    if(key == KEY_LEFT) { g_ed_sel_anchor = -1; lv_textarea_cursor_left(g_editor); }
    else if(key == KEY_RIGHT) { g_ed_sel_anchor = -1; lv_textarea_cursor_right(g_editor); }
    else if(key == KEY_UP) { g_ed_sel_anchor = -1; lv_textarea_cursor_up(g_editor); }
    else if(key == KEY_DOWN) { g_ed_sel_anchor = -1; lv_textarea_cursor_down(g_editor); }
    else if(key == KEY_HOME) { g_ed_sel_anchor = -1; lv_textarea_set_cursor_pos(g_editor, 0); }
    else if(key == KEY_END) { g_ed_sel_anchor = -1; lv_textarea_set_cursor_pos(g_editor, LV_TEXTAREA_CURSOR_LAST); }
    else if(key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // textarea 有焦点时会自己把视图滚回光标处,所以翻页只能靠移光标:一页 = 当前可见行数。
        g_ed_sel_anchor = -1;
        const lv_font_t *pf = lv_obj_get_style_text_font(g_editor, LV_PART_MAIN);
        int lh = pf ? lv_font_get_line_height(pf) : 0;
        int ls = (int)lv_obj_get_style_text_line_space(g_editor, LV_PART_MAIN);
        int chh = (int)lv_obj_get_content_height(g_editor);
        int page = (lh > 0 && chh > 0) ? (chh + ls) / (lh + ls) : 10;
        if(page < 1) page = 1;
        for(int i = 0; i < page; ++i) {
            if(key == KEY_PAGE_UP) lv_textarea_cursor_up(g_editor);
            else lv_textarea_cursor_down(g_editor);
        }
    }
    else if(key == 8 || key == 127) {
        if(g_ed_sel_anchor >= 0) editor_delete_selection();
        else { editor_record_undo(); lv_textarea_delete_char(g_editor); }
    } else if(key == '\n' || key == '\r') {
        if(g_ed_sel_anchor >= 0) {
            editor_delete_selection();
            lv_textarea_add_char(g_editor, '\n');
        } else {
            editor_record_undo();
            lv_textarea_add_text(g_editor, editor_list_continue().c_str());
        }
    } else if(key >= 32 && key < 127) {
        if(g_ed_sel_anchor >= 0) editor_delete_selection();
        else editor_record_undo();
        lv_textarea_add_char(g_editor, (uint32_t)key);
    }
    update_ime_bar();
}

static void handle_key(int key) {
    // 退出确认框是模态的:吃掉所有按键。Ctrl+Q 在三种工作模式下都灵,也放在最前面,
    // 免得被下面的输入法/文件槽位快捷键抢走。
    if(g_quit_asking) { quit_confirm_key(key); return; }
    if(key == 0x11) { quit_begin(); return; }
    if(g_linux_ime.active()) {
        if(key == KEY_FULLWIDTH_TOGGLE) {
            g_linux_ime.toggle_fullwidth();
            update_ime_bar();
            return;
        }
        if(key == KEY_TRAD_TOGGLE) {
            g_linux_ime.toggle_trad();
            update_ime_bar();
            return;
        }
        if(key == KEY_LSHIFT_TAP) {
            g_linux_ime.toggle_english();
            update_ime_bar();
            return;
        }
    }
    if(g_settings.app_mode() == "quick" && key >= KEY_FILE_BASE && key <= KEY_FILE_BASE + 9) {
        open_quick_editor(key - KEY_FILE_BASE);
        return;
    }
    if(key == KEY_HELP && g_screen != Screen::Editor) { g_screen = g_screen == Screen::Help ? g_prev_screen : Screen::Help; render(); return; }
    if(g_screen == Screen::Help) { if(key == KEY_HELP || key == 'q' || key == 27) goto_screen(g_prev_screen); return; }
    if(key == KEY_CTRL_I) { goto_screen(Screen::Inspiration); return; }
    switch(g_screen) {
    case Screen::Main: handle_main(key); break;
    case Screen::Editor: handle_editor(key); break;
    case Screen::Browser: handle_browser(key); break;
    case Screen::Viewer:
        if(key == 'q' || key == 27) goto_screen(Screen::Browser);
        else if(key == 'e' || key == 'E') { g_quick_slot = -1; g_file_edit_path.clear(); g_edit_file = g_view_file; g_prompt.clear(); goto_screen(Screen::Editor); }
        else if(key == 'h' || key == 'H') { open_history(g_view_file, Screen::Viewer); }
        else if(key == KEY_DOWN || key == 'j') { g_viewer_scroll++; render(); }
        else if(key == KEY_UP || key == 'k') { g_viewer_scroll--; render(); }
        else if(key == KEY_PAGE_DOWN) { g_viewer_scroll += 10; render(); }
        else if(key == KEY_PAGE_UP) { g_viewer_scroll -= 10; render(); }
        else render();
        break;
    case Screen::History: handle_history(key); break;
    case Screen::SettingText: handle_setting_text(key); break;
    case Screen::Gtd: handle_gtd(key); break;
    case Screen::Outline: handle_outline(key); break;
    case Screen::Sync: handle_sync(key); break;
    case Screen::Settings: handle_settings(key); break;
    case Screen::Wifi: handle_wifi(key); break;
    case Screen::Inspiration: handle_inspiration(key); break;
    case Screen::Dict: handle_dict(key); break;
    case Screen::FileMgr: handle_filemgr(key); break;
    case Screen::Help: break;
    }
}

extern "C" void app_ui_handle_key(int key) {
    handle_key(key);
}

extern "C" void app_ui_request_quit() {
    g_should_quit = true;
}

extern "C" bool app_ui_is_main_screen() {
    return g_screen == Screen::Main;
}

extern "C" void app_ui_insert_utf8(const char *text) {
    if(text) insert_utf8(text);
}

void app_ui_create() {
    srand((unsigned)time(nullptr));
    apply_theme_values();
    g_root = lv_screen_active();
    lv_obj_remove_style_all(g_root);
    g_linux_ime.begin();
    g_linux_ime.set_width_fn(ime_measure_width);
    app_ui_reload_font();
    if(g_settings.app_mode() == "quick") {
        open_quick_editor(0);
        return;
    }
    if(g_settings.app_mode() == "file") {
        open_file_editor(file_edit_last_name());
        return;
    }
    render();
}

bool app_ui_should_quit() {
    return g_should_quit;
}

void app_ui_tick() {
    static uint32_t next_main_status_ms = 0;
    if(g_screen == Screen::Main && g_status) {
        uint32_t tick = lv_tick_get();
        if(next_main_status_ms == 0 || (int32_t)(tick - next_main_status_ms) >= 0) {
            set_status(main_status_text());
            next_main_status_ms = tick + 30000;
        }
    }
    if(g_screen != Screen::Editor || !g_editor) return;
    if(!g_file_edit_path.empty()) {
        if(g_settings.auto_save() && editor_dirty()) save_editor_text();
        return;
    }
    if(g_quick_slot >= 0) {
        if(g_settings.auto_save() && editor_dirty()) save_editor_text();
        return;
    }
    if(!g_ol.editPath.empty()) return;  // 大纲正文文件不做草稿恢复
    uint32_t now = lv_tick_get();
    if(g_next_recovery_ms != 0 && (int32_t)(now - g_next_recovery_ms) < 0) return;
    g_next_recovery_ms = now + 3000;

    std::string body = lv_textarea_get_text(g_editor);
    if(body.empty() || !utf8_valid(body)) return;
    std::string key = body + "\n" + g_edit_file + "\n" + g_prompt;
    size_t h = std::hash<std::string>{}(key);
    if(h == g_last_recovery_hash) return;

    if(g_settings.auto_save()) {
        // 空闲自动提交:沿用同一文件,不写历史版本,避免重复建档
        std::string content = make_journal_text(g_prompt, body);
        bool ok = false;
        if(g_edit_file.empty()) {
            ok = g_journal.save_entry(content);
            if(ok) {
                g_entries = g_journal.list_entries();
                if(!g_entries.empty()) g_edit_file = g_entries.front().filename;
            }
        } else {
            ok = g_journal.save_entry_raw(g_edit_file, content, false);
        }
        if(ok) {
            g_journal.clear_recovery_draft();
            g_last_recovery_hash = h;
        }
        return;
    }
    if(!g_settings.recovery_draft()) return;

    std::string meta;
    meta += "mode=journal\n";
    meta += "filename=" + g_edit_file + "\n";
    meta += "prompt=" + g_prompt + "\n";
    meta += "timestamp=" + std::to_string((long long)time(nullptr)) + "\n";
    if(g_journal.save_recovery_draft(body, meta)) {
        g_last_recovery_hash = h;
    }
}
