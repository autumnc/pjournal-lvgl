#pragma once

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

// 一条笔记。字段与原 C1ancher/Flomo 的 db.cpp 一一对应,本地库文件的格式不变,
// 换台机器也能接着用。
struct Memo {
    std::string slug;           // 远端主键;本地新建尚未同步时是 "local-<时间戳>"
    std::string contentHtml;    // Flomo 那侧的 HTML
    std::string contentText;    // 上面那串 HTML 还原成纯文本;编辑和显示都用它
    std::vector<std::string> tags;
    std::string createdAt;
    std::string updatedAt;
    bool deleted = false;
    bool dirty = false;         // 有本地改动还没推上去
    std::string pendingOp;      // "create" / "update" / "delete"
    int pin = 0;                // 0 普通,非 0 置顶

    bool pinned() const { return pin != 0; }
};

struct Store {
    std::string token;
    std::vector<Memo> memos;
    std::string lastSync;
    // memo/updated/ 的增量游标:上次拉到的最后一条的 updated_at 与 slug。
    long long cursorUpdatedAt = 0;
    std::string cursorSlug;
};

enum class View {
    List,          // 默认:卡片列表
    Detail,        // 单条正文,可滚
    Edit,          // 新建 / 编辑
    Search,        // 搜索
    Tags,          // 标签列表(可筛选、可改名)
    TagFilter,     // 按标签筛选的输入框
    TagRename,     // 重命名标签的输入框
    Review,        // 今日回顾
    Related,       // 相关笔记
    Help,          // 快捷键
    Settings,      // 设置
    LoginEmail,
    LoginPassword,
    LoginToken,    // 直接粘 web token 登录
};

struct AppState {
    View view = View::List;
    std::vector<Memo> memos;    // 全量(含已删除待同步的)
    std::vector<int> filtered;  // memos 的下标,按 query/tag 过滤后的
    int selected = 0;
    std::string query;
    std::string tag;
    std::string status;         // 状态栏左边那句话,空则显示按键提示
    std::string loginEmail;     // 登录界面之间传递
    bool loggedIn = false;
};

// 有没有本地专属的笔记(还没拿到远端 slug) —— 这种删除不用惊动服务器。
inline bool isLocalOnly(const Memo &m) {
    return m.pendingOp == "create" || m.slug.rfind("local-", 0) == 0;
}

inline std::string now_stamp() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::tm local {};
    localtime_r(&t, &local);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

// 最近更新的排前面。两个时间戳都是 "YYYY-MM-DD HH:MM:SS" 的定长串,字节序即时间序。
inline void sort_memos(std::vector<Memo> &memos) {
    std::sort(memos.begin(), memos.end(),
              [](const Memo &a, const Memo &b) { return a.updatedAt > b.updatedAt; });
}

// 按 slug 合并:本地有未推的改动(dirty 且不是待删除)时保留本地那份,别被远端覆盖。
inline void upsert_memo(std::vector<Memo> &memos, const Memo &memo) {
    for(auto &m : memos) {
        if(m.slug != memo.slug) continue;
        if(!(m.dirty && m.pendingOp != "delete")) m = memo;
        return;
    }
    memos.push_back(memo);
}

inline void remove_memo_by_slug(std::vector<Memo> &memos, const std::string &slug) {
    if(slug.empty()) return;
    memos.erase(std::remove_if(memos.begin(), memos.end(),
                               [&](const Memo &m) { return m.slug == slug; }),
                memos.end());
}

// 选中父标签时,子标签(以 "父/" 开头)也要算命中。
inline bool memo_has_tag(const Memo &m, const std::string &tag) {
    if(tag.empty()) return true;
    const std::string prefix = tag + "/";
    for(const auto &t : m.tags)
        if(t == tag || t.compare(0, prefix.size(), prefix) == 0) return true;
    return false;
}

// 按 query/tag 重算可见行。已标记删除的不出现。
inline void rebuild_filter(AppState &app) {
    app.filtered.clear();
    for(size_t i = 0; i < app.memos.size(); ++i) {
        const Memo &m = app.memos[i];
        if(m.deleted) continue;
        if(!app.query.empty() && m.contentText.find(app.query) == std::string::npos) continue;
        if(!memo_has_tag(m, app.tag)) continue;
        app.filtered.push_back((int)i);
    }
    if(app.selected >= (int)app.filtered.size()) app.selected = (int)app.filtered.size() - 1;
    if(app.selected < 0) app.selected = 0;
}
