#include "fcitx5_ime.h"

#include "key_codes.h"

#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char *kService = "org.fcitx.Fcitx5";
// 新版本 fcitx5 把 InputMethod1 挂在 /org/freedesktop/portal/inputmethod 上;
// 老的那个 /inputmethod 在另一条连接的名字 org.freedesktop.portal.Fcitx 底下,不用。
constexpr const char *kImPath = "/org/freedesktop/portal/inputmethod";
constexpr const char *kImIface = "org.fcitx.Fcitx.InputMethod1";
constexpr const char *kIcIface = "org.fcitx.Fcitx.InputContext1";
constexpr const char *kControllerPath = "/controller";
constexpr const char *kControllerIface = "org.fcitx.Fcitx.Controller1";
constexpr const char *kRimePath = "/rime";
constexpr const char *kRimeIface = "org.fcitx.Fcitx.Rime1";

// capabilityflags.h 里 ClientSideInputPanel 是 1ULL<<39。不报这个能力 fcitx5 就自己
// 画候选框去了(这台机器上没有能画的 UI),UpdateClientSideUI 也不会发过来。
constexpr guint64 kCapClientSideInputPanel = 1ULL << 39;

// KeyState 的位值,来自 fcitx-utils/keysym.h
constexpr uint32_t kStateShift = 1u << 0;
constexpr uint32_t kStateCtrl = 1u << 2;

// keysym:拉丁可打印字符就是 ASCII 本身,其余用 X11 的固定值。
constexpr uint32_t XK_space = 0x0020;
constexpr uint32_t XK_BackSpace = 0xff08;
constexpr uint32_t XK_Tab = 0xff09;
constexpr uint32_t XK_Return = 0xff0d;
constexpr uint32_t XK_Escape = 0xff1b;
constexpr uint32_t XK_Delete = 0xffff;
constexpr uint32_t XK_Left = 0xff51;
constexpr uint32_t XK_Up = 0xff52;
constexpr uint32_t XK_Right = 0xff53;
constexpr uint32_t XK_Down = 0xff54;

// app 通常是从 tty1 那个 shell 起来的,环境里就有 DBUS_SESSION_BUS_ADDRESS;换成 ssh
// 或者别的服务起就没有了。fcitx5 自己一定在这条 session bus 上,从它的 environ 里抄一份
// 出来(和 fcitx5-status.sh 一个思路)。
void ensure_session_bus_address() {
    const char *cur = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if(cur && *cur) return;

    DIR *d = opendir("/proc");
    if(!d) return;
    std::string addr;
    while(dirent *e = readdir(d)) {
        if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string base = std::string("/proc/") + e->d_name + "/";
        gchar *comm = nullptr;
        if(!g_file_get_contents((base + "comm").c_str(), &comm, nullptr, nullptr)) continue;
        std::string name(comm);
        g_free(comm);
        while(!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
        if(name != "fcitx5") continue;

        gchar *buf = nullptr;
        gsize len = 0;
        if(!g_file_get_contents((base + "environ").c_str(), &buf, &len, nullptr)) continue;
        for(gsize i = 0; i < len;) {
            const char *p = buf + i;
            const void *z = memchr(p, '\0', len - i);
            gsize seg = z ? (gsize)((const char *)z - p) : len - i;
            if(seg > 24 && strncmp(p, "DBUS_SESSION_BUS_ADDRESS=", 24) == 0) {
                addr.assign(p + 24, seg - 24);
                break;
            }
            i += seg + 1;
        }
        g_free(buf);
        if(!addr.empty()) break;
    }
    closedir(d);
    if(!addr.empty()) g_setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), TRUE);
}

struct KeySymState {
    uint32_t sym;
    uint32_t state;
};

KeySymState translate_app_key(int key, const std::string &preedit) {
    // 控制台这条路只有按下没有抬起,而且 tty 已经把键表翻译过了,所以这里拿到的基本都是
    // 「最终字符」。控制字符(8/9/10/27/127)先挑出来,免得和 Ctrl+字母 的 1..26 撞车。
    switch(key) {
    case '\n': return {XK_Return, 0};
    case '\t': return {XK_Tab, 0};
    case 8: return {XK_BackSpace, 0};
    case 27: return {XK_Escape, 0};
    case 127: return {XK_Delete, 0};
    default: break;
    }
    if(key >= 'A' && key <= 'Z') {
        // uU 是 rime 的拆字前缀(radical_lookup 认 ^uU[a-z]+$)。下面那条一律折成
        // 「小写基本键 + Shift」,'U' 就进不了输入串,拆字支路永远走不到;只有紧跟在 u
        // 后面这一次放行字面 'U'(rime 会把它当字面字符插进输入串)。
        if(key == 'U' && preedit == "u") return {(uint32_t)'U', 0};
        // 字母要还原成「小写基本键 + Shift」:rime 的 speller 只认小写键,把 'A' 原样
        // 当 keysym 发过去它当字面文本收(实测 preedit 出 "NI"、候选只有一个 "N")。
        return {(uint32_t)(key - 'A' + 'a'), kStateShift};
    }
    if(key >= 0x20 && key <= 0x7e) {
        // 标点/符号反过来:rime 的 punctuator 只看 keysym 自己,Shift 位不参与判断
        // (实测 ('%',Shift) 和 ('%',0) 都上屏 '%'),所以必须把移位后的字符原样发过去。
        // 拆成「基本键+Shift」就出不来了——'#' 会变成 '3' 或什么都不出。
        return {(uint32_t)key, 0};
    }
    if(key >= 0x01 && key <= 0x1a) return {(uint32_t)('a' + key - 1), kStateCtrl};

    switch(key) {
    case KEY_UP: return {XK_Up, 0};
    case KEY_DOWN: return {XK_Down, 0};
    case KEY_LEFT: return {XK_Left, 0};
    case KEY_RIGHT: return {XK_Right, 0};
    case KEY_SHIFT_UP: return {XK_Up, kStateShift};
    case KEY_SHIFT_DOWN: return {XK_Down, kStateShift};
    case KEY_SHIFT_LEFT: return {XK_Left, kStateShift};
    case KEY_SHIFT_RIGHT: return {XK_Right, kStateShift};
    // Home/End 不当普通键发:翻页在 handle_key 里单独走 prev_page()/next_page()(见下)。
    // PageUp/PageDown 更是别当翻页用 —— rime 的默认键表里根本没绑这两个键,
    // 发过去只会被忽略,还挡住编辑器自己的翻页滚动。留 {} 让它们落回编辑器。
    case KEY_CTRL_ENTER: return {XK_Return, kStateCtrl};
    case KEY_CTRL_I: return {(uint32_t)'i', kStateCtrl};
    case KEY_SEARCH: return {(uint32_t)'/', kStateCtrl};
    case KEY_HELP: return {(uint32_t)'/', kStateCtrl | kStateShift};
    case KEY_REDO: return {(uint32_t)'z', kStateCtrl | kStateShift};
    default: return {0, 0};
    }
}

std::string join_formatted_text(GVariant *arr) {
    std::string out;
    GVariantIter *it = g_variant_iter_new(arr);
    GVariant *item = nullptr;
    while((item = g_variant_iter_next_value(it))) {
        const gchar *s = nullptr;
        gint32 fmt = 0;
        g_variant_get(item, "(&si)", &s, &fmt);
        if(s) out += s;
        g_variant_unref(item);
    }
    g_variant_iter_free(it);
    return out;
}

std::vector<std::string> read_candidates(GVariant *arr) {
    std::vector<std::string> out;
    GVariantIter *it = g_variant_iter_new(arr);
    GVariant *item = nullptr;
    while((item = g_variant_iter_next_value(it))) {
        const gchar *label = nullptr;
        const gchar *text = nullptr;
        g_variant_get(item, "(&s&s)", &label, &text);
        if(text) out.emplace_back(text);
        g_variant_unref(item);
    }
    g_variant_iter_free(it);
    return out;
}

GVariant *bus_call(GDBusConnection *conn, const char *path, const char *iface,
                   const char *method, GVariant *params, const GVariantType *reply,
                   int timeout_ms = 3000) {
    if(!conn) return nullptr;
    GError *err = nullptr;
    GVariant *r = g_dbus_connection_call_sync(conn, kService, path, iface, method, params,
                                              reply, G_DBUS_CALL_FLAGS_NONE, timeout_ms,
                                              nullptr, &err);
    if(!r && err) g_error_free(err);
    return r;
}

// begin() 那串调用的超时。它们跑在 lv_timer_handler 里、压着 lv_lock,fcitx5 活着但
// 不吭声时每一个都要等满超时 —— 默认 3s × 5 个就是十几秒的冻屏。连接阶段的调用全部
// 按这个短超时走,失败的交给 pump() 的退避去重试。
constexpr int kConnectTimeoutMs = 1200;

// 发一次键。返回值是 fcitx5 说的「这个键我吃了没有」——没吃的话我们交回给 app 自己
// 插字符,所以英文直通、光标键、退格这些不用前端再判一遍。
bool send_key_raw(GDBusConnection *conn, const std::string &ic_path, uint32_t sym,
                  uint32_t state) {
    GVariant *r = bus_call(conn, ic_path.c_str(), kIcIface, "ProcessKeyEvent",
                           g_variant_new("(uuubu)", sym, 0u, state, FALSE,
                                         (guint32)lv_tick_get()),
                           G_VARIANT_TYPE("(b)"));
    if(!r) return false;
    gboolean handled = FALSE;
    g_variant_get(r, "(b)", &handled);
    g_variant_unref(r);
    return handled != FALSE;
}

// rime 的方案文件:<用户数据目录>/fcitx5/rime/<id>.schema.yaml。设备上 HOME=/root →
// g_get_user_data_dir() = /root/.local/share;/usr/share/rime-data 里只有 fcitx5.yaml,
// 方案文件只在用户目录下,所以不用再找别处。
std::string rime_schema_file(const std::string &id) {
    if(id.empty()) return std::string();
    char *path = g_build_filename(g_get_user_data_dir(), "fcitx5", "rime",
                                  (id + ".schema.yaml").c_str(), nullptr);
    std::string out = path ? path : "";
    g_free(path);
    return out;
}

// 方案文件在不在。rime 的 ListAllSchemas 会把 default.custom.yaml 里 schema_list 写过的
// 方案全列出来,但设备上只部署了 rime_ice / radical_pinyin 两个,其余 8 个(各种双拼、
// t9)连文件都没有,选过去也没有候选。切方案和显示名字都按这个过滤。
bool rime_schema_available(const std::string &id) {
    if(id.empty()) return false;
    return g_file_test(rime_schema_file(id).c_str(), G_FILE_TEST_IS_REGULAR);
}

// 方案文件里的显示名(雾凇拼音)。读第一处行首(允许缩进)的 name:。文件不在或没有
// name: 就返回空串,由调用方决定怎么退化(状态栏会退回「中」)。
std::string rime_schema_display_name(const std::string &id) {
    if(!rime_schema_available(id)) return std::string();
    static std::map<std::string, std::string> cache;
    auto it = cache.find(id);
    if(it != cache.end()) return it->second;

    std::string name;
    gchar *data = nullptr;
    gsize len = 0;
    if(g_file_get_contents(rime_schema_file(id).c_str(), &data, &len, nullptr)) {
        const char *p = data;
        while(p && *p) {
            const char *eol = strchr(p, '\n');
            size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
            const char *q = p;
            while(linelen > 0 && (*q == ' ' || *q == '\t')) { q++; linelen--; }
            if(linelen > 5 && strncmp(q, "name:", 5) == 0) {
                std::string v(q + 5, linelen - 5);
                size_t a = v.find_first_not_of(" \t\r");
                size_t b = v.find_last_not_of(" \t\r");
                if(a != std::string::npos) {
                    v = v.substr(a, b - a + 1);
                    if(v.size() >= 2 && (v.front() == '"' || v.front() == '\'') &&
                       v.back() == v.front())
                        v = v.substr(1, v.size() - 2);
                    name = v;
                }
                break;
            }
            if(!eol) break;
            p = eol + 1;
        }
        g_free(data);
    }
    cache[id] = name;
    return name;
}

}  // namespace

Fcitx5Ime &fcitx5_ime() {
    static Fcitx5Ime inst;
    return inst;
}

bool Fcitx5Ime::begin() {
    if(connected()) return true;
    // 兜底定时器要先建:begin() 失败(fcitx5 比 app 起得晚)时后面每个 return false 都会
    // 直接跳出去,把定时器留在最后建就永远没人重试了。
    if(!_timer) _timer = lv_timer_create(&Fcitx5Ime::on_timer_trampoline, 100, nullptr);
    // 会话 bus 本身断了(session 挂了)连订阅全废,得整个重来;只是 fcitx5 重启的话
    // 连接还活着,走下面 NameOwnerChanged 那条路清 IC 就够了。
    if(_conn && g_dbus_connection_is_closed(_conn)) {
        if(_sig) g_dbus_connection_signal_unsubscribe(_conn, _sig);
        if(_bus_sig) g_dbus_connection_signal_unsubscribe(_conn, _bus_sig);
        g_object_unref(_conn);
        _conn = nullptr;
        _sig = 0;
        _bus_sig = 0;
        _ic_path.clear();
    }
    ensure_session_bus_address();
    // 地址还是没有就别让 g_bus_get_sync 去 autolaunch —— 这台机器上那只会多起一个没用的
    // dbus-daemon,还不如直接认输。
    const char *addr = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if(!addr || !*addr) return false;

    // worker 线程建好的连接在这里取走。取走之后这条连接的引用只归主线程管。
    if(!_conn) {
        GDBusConnection *ready = _pending_conn.exchange(nullptr);
        if(ready) _conn = ready;
    }
    // 还没取到就把活派出去,begin() 自己这一步不阻塞。下次 pump() 再来收。
    if(!_conn) {
        if(!_connecting.exchange(true)) std::thread(&Fcitx5Ime::connect_worker, this).detach();
        return false;
    }

    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(&b, "(ss)", "program", "pjournal-lvgl");
    GVariant *r = bus_call(_conn, kImPath, kImIface, "CreateInputContext",
                           g_variant_new("(a(ss))", &b), G_VARIANT_TYPE("(oay)"),
                           kConnectTimeoutMs);
    if(!r) return false;
    const gchar *path = nullptr;
    GVariant *uuid = nullptr;
    g_variant_get(r, "(&o@ay)", &path, &uuid);
    if(path) _ic_path = path;
    if(uuid) g_variant_unref(uuid);
    g_variant_unref(r);
    if(_ic_path.empty()) return false;

    _sig = g_dbus_connection_signal_subscribe(_conn, kService, kIcIface, nullptr,
                                              _ic_path.c_str(), nullptr,
                                              G_DBUS_SIGNAL_FLAGS_NONE,
                                              &Fcitx5Ime::on_signal_trampoline, this,
                                              nullptr);
    // fcitx5 重启(改配置、崩了被拉起来)是老进程退出、新进程同名重生,IC path 作废但
    // bus 连接还在。盯 NameOwnerChanged:owner 变空就是它没了,清掉 IC 让 pump() 重连。
    // arg0 过滤只收这一个名字,别的服务起落不吵我们。
    if(!_bus_sig)
        _bus_sig = g_dbus_connection_signal_subscribe(
            _conn, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
            nullptr, kService, G_DBUS_SIGNAL_FLAGS_NONE, &Fcitx5Ime::on_signal_trampoline,
            this, nullptr);

    // 不报 ClientSideInputPanel 能力,fcix5 就自己画候选框(这台机器上没有能画的 UI),
    // UpdateClientSideUI 也不会发过来。
    bus_call(_conn, _ic_path.c_str(), kIcIface, "SetCapability",
             g_variant_new("(t)", (guint64)kCapClientSideInputPanel), nullptr,
             kConnectTimeoutMs);
    // IC 默认可能落在 keyboard-us 上,显式指到 rime。
    bus_call(_conn, kControllerPath, kControllerIface, "SetCurrentIM",
             g_variant_new("(s)", "rime"), nullptr, kConnectTimeoutMs);
    bus_call(_conn, _ic_path.c_str(), kIcIface, "FocusIn", nullptr, nullptr,
             kConnectTimeoutMs);
    drain();
    refresh_current_schema();
    return true;
}

// 方案 id 只在切方案时变,这里探一次存下来给状态栏用。别在 schema_name() 里现查:
// 它会被 update_ime_bar() → refresh_ime_status() 每个按键调一次。
void Fcitx5Ime::refresh_current_schema() {
    GVariant *c = bus_call(_conn, kRimePath, kRimeIface, "GetCurrentSchema", nullptr,
                           G_VARIANT_TYPE("(s)"), kConnectTimeoutMs);
    const gchar *name = nullptr;
    if(c) {
        g_variant_get(c, "(&s)", &name);
        if(name && *name) _schema = name;
        g_variant_unref(c);
    }
}

// 只在 worker 线程上跑。g_bus_get_sync 是这里唯一一个没有超时的调用,把整条 UI 线程
// 压在它上面风险太大 —— 见头文件里 _pending_conn 那段说明。
void Fcitx5Ime::connect_worker() {
    GError *err = nullptr;
    GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if(err) g_error_free(err);
    // 极端情况下可能已经有别的连接躺在那儿(理论上不会,留着防漏)。
    GDBusConnection *old = _pending_conn.exchange(c);
    if(old) g_object_unref(old);
    _connecting.store(false);
}

void Fcitx5Ime::on_timer_trampoline(lv_timer_t *) {
    fcitx5_ime().pump();
}

// 兜底:主循环之外的事件(掉线、fcitx5 比 app 起得晚)靠这个一秒钟一次的定时器收。
// 连不上就退避,别每秒都去敲一遍 bus:begin() 那串同步调用是压着 lv_lock 跑的,
// fcitx5 一直不在时会变成周期性卡顿。
void Fcitx5Ime::pump() {
    drain();
    if(connected()) {
        _retry_backoff = 0;
        _next_retry_ms = 0;
        // 连上之后定时器退回 1 秒:它的活只剩「发现掉线」了。(这版 LVGL 没有
        // lv_timer_get_period,set_period 也只是个赋值,直接写就行。)
        if(_timer) lv_timer_set_period(_timer, 1000);
        return;
    }
    uint32_t now = lv_tick_get();
    if(_next_retry_ms != 0 && (int32_t)(now - _next_retry_ms) < 0) return;
    if(begin()) {
        _retry_backoff = 0;
        _next_retry_ms = 0;
        return;
    }
    int ms = _retry_backoff > 0 ? _retry_backoff * 2 : 1000;
    if(ms > 30000) ms = 30000;
    _retry_backoff = ms;
    _next_retry_ms = now + (uint32_t)ms;
}

void Fcitx5Ime::on_signal_trampoline(GDBusConnection *, const char *, const char *,
                                     const char *iface, const char *name, GVariant *params,
                                     void *user_data) {
    auto *self = static_cast<Fcitx5Ime *>(user_data);
    if(iface && strcmp(iface, "org.freedesktop.DBus") == 0) {
        // NameOwnerChanged(s name, s old_owner, s new_owner);订阅时 arg0 已经过滤成
        // kService 了,只管看 new_owner 空不空。
        if(g_variant_n_children(params) < 3) return;
        GVariant *nv = g_variant_get_child_value(params, 2);
        const gchar *new_owner = g_variant_get_string(nv, nullptr);
        bool lost = new_owner && !*new_owner;
        g_variant_unref(nv);
        if(lost) self->on_service_lost();
        return;
    }
    if(!iface || strcmp(iface, kIcIface) != 0) return;
    self->on_signal(name, params);
}

// fcitx5 自己没了(重启/被杀):IC path 指向的对象已经作废,清掉它和组字状态,
// connected() 就变 false,pump() 下一秒会重新 CreateInputContext。bus 连接不动。
void Fcitx5Ime::on_service_lost() {
    if(_sig && _conn) {
        g_dbus_connection_signal_unsubscribe(_conn, _sig);
        _sig = 0;
    }
    _ic_path.clear();
    clear_composition();
    _schema.clear();
}

void Fcitx5Ime::drain() {
    // 只跑默认 GMainContext 上已经排好队的事件,不阻塞。发键和信号回包是同一条连接、
    // 同一个 context,所以 ProcessKeyEvent 返回后抽干一次就能拿到 CommitString 和
    // UpdateClientSideUI,handle_key 才可能同步返回上屏串。
    for(int i = 0; i < 64 && g_main_context_iteration(nullptr, FALSE); ++i) {}
}

void Fcitx5Ime::on_signal(const char *name, GVariant *params) {
    if(strcmp(name, "CommitString") == 0) {
        const gchar *s = nullptr;
        g_variant_get(params, "(&s)", &s);
        if(s) _pending_commit += s;
        return;
    }
    if(strcmp(name, "UpdateClientSideUI") != 0) return;

    // a(si)i a(si) a(si) a(ss) i i b b
    // preedit / 光标 / 上屏前提示 / 下提示 / 候选[标签,文本] / 高亮 / 排版提示 / 前页 / 后页
    GVariant *preedit = nullptr;
    GVariant *auxup = nullptr;
    GVariant *auxdn = nullptr;
    GVariant *cands = nullptr;
    gint32 cursor = 0, highlight = 0, layout = 0;
    gboolean has_prev = FALSE, has_next = FALSE;
    g_variant_get(params, "(@a(si)i@a(si)@a(si)@a(ss)iibb)", &preedit, &cursor, &auxup,
                  &auxdn, &cands, &highlight, &layout, &has_prev, &has_next);

    std::string text = join_formatted_text(preedit);
    std::vector<std::string> list = read_candidates(cands);

    // fcitx5 只给「有没有上/下一页」,给不出页码,只能靠 hasPrev/hasNext 的跳变猜:
    // hasPrev 从无到有 = 翻到了第 2 页;hasNext 从有到无 = 翻到了最后一页。
    if(text != _preedit) {
        _page = 1;
    } else if(!has_prev) {
        _page = 1;
    } else if(!_has_prev) {
        _page++;
    } else if(_has_next && !has_next) {
        _page++;
    } else if(!_has_next && has_next) {
        if(_page > 1) _page--;
    }

    _preedit = text;
    _cands = std::move(list);
    rebuild_visible();
    _highlight = (highlight >= 0 && highlight < (int)_cands.size()) ? (int)highlight : -1;
    _has_prev = has_prev;
    _has_next = has_next;

    g_variant_unref(preedit);
    g_variant_unref(auxup);
    g_variant_unref(auxdn);
    g_variant_unref(cands);
}

void Fcitx5Ime::clear_composition() {
    _preedit.clear();
    _cands.clear();
    _visible.clear();
    _highlight = -1;
    _page = 1;
    _has_prev = false;
    _has_next = false;
    _pending_commit.clear();
}

// 把这一页候选裁到 _budget 像素以内。量宽方式和 app_ui 的 ime_bar_layout_horizontal 对齐:
// 每项画成 " 编号.候选",编号用的是本页里的原序号,裁了也不重排。
void Fcitx5Ime::rebuild_visible() {
    _visible.clear();
    if(_budget <= 0 || !_width_fn) {
        _visible = _cands;
        return;
    }
    int used = 0;
    for(size_t i = 0; i < _cands.size(); ++i) {
        std::string part = " " + std::to_string((int)i + 1) + "." + _cands[i];
        int w = _width_fn(part.c_str());
        if(i > 0 && used + w > _budget) break;
        used += w;
        _visible.push_back(_cands[i]);
    }
}

void Fcitx5Ime::set_display_width(int px) {
    _budget = px;
    rebuild_visible();
}

void Fcitx5Ime::set_width_fn(WidthFn fn) {
    _width_fn = fn;
    rebuild_visible();
}

void Fcitx5Ime::set_active(bool on) {
    if(_active == on) return;
    _active = on;
    clear_composition();
    if(connected()) {
        if(on) bus_call(_conn, _ic_path.c_str(), kIcIface, "FocusIn", nullptr, nullptr);
        bus_call(_conn, _ic_path.c_str(), kIcIface, "Reset", nullptr, nullptr);
    }
}

bool Fcitx5Ime::send_key(uint32_t sym, uint32_t state) {
    _pending_commit.clear();
    bool handled = send_key_raw(_conn, _ic_path, sym, state);
    drain();
    return handled;
}

bool Fcitx5Ime::handle_key(int key, std::string &out) {
    out.clear();
    if(!active()) return false;
    // 没组字时的退格交回编辑器自己删,别发给 rime。万象拼音的 super_processor 里有一层
    // 「退格限制」(handle_backspace):它记着「上一次退格把编码从 1 个字符删到 0」这个状态,
    // 之后**连续**再按退格就直接 return true(吃掉这个键)。它靠**抬起**事件复位,而这条
    // 控制台通路只有按下没有抬起(见 translate_app_key 上面的注释),状态一旦置上就再也下不来
    // —— 于是每个退格都被 rime 收下当「已处理」,编辑器根本轮不到删除。纯英文模式没有编码,
    // 那条守卫走不到(cur_len 一直是 0),所以那边看着正常。
    // 没组字本来也不该麻烦 rime:编辑器正文的内容 rime 一无所知。内置输入法和 yong 后端
    // 也是这么做的(没编码时退格还给编辑器)。
    if(key == 8 && !composing()) return false;
    // 组字时 Home/End 翻页(和内置输入法一致)。翻页键由 rime 那边定,不硬编在这里。
    // 没组字时返回 false,Home/End 还是编辑器的「光标到行首/行尾」。
    if(composing()) {
        if(key == KEY_HOME) {
            prev_page();
            return true;
        }
        if(key == KEY_END) {
            next_page();
            return true;
        }
    }
    KeySymState ks = translate_app_key(key, _preedit);
    if(ks.sym == 0) return false;
    bool handled = send_key(ks.sym, ks.state);
    if(!_pending_commit.empty()) {
        out = _pending_commit;
        _pending_commit.clear();
    }
    return handled || !out.empty();
}

void Fcitx5Ime::toggle_fullwidth() {
    _fullwidth = !_fullwidth;
    send_key(XK_space, kStateShift);
}

void Fcitx5Ime::toggle_trad() {
    _trad = !_trad;
    send_key((uint32_t)'f', kStateCtrl | kStateShift);
}

// ascii_mode 是 rime 唯一开了 DBus 的开关,直接查/直接设,比发合成键可靠。
void Fcitx5Ime::toggle_english() {
    bool cur = _english;
    GVariant *r = bus_call(_conn, kRimePath, kRimeIface, "IsAsciiMode", nullptr,
                           G_VARIANT_TYPE("(b)"));
    if(r) {
        gboolean b = FALSE;
        g_variant_get(r, "(b)", &b);
        cur = b != FALSE;
        g_variant_unref(r);
    }
    _english = !cur;
    bus_call(_conn, kRimePath, kRimeIface, "SetAsciiMode",
             g_variant_new("(b)", (gboolean)_english), nullptr);
}

bool Fcitx5Ime::switch_schema() {
    if(!_conn) return false;

    std::vector<std::string> schemas;
    GVariant *r = bus_call(_conn, kRimePath, kRimeIface, "ListAllSchemas", nullptr,
                           G_VARIANT_TYPE("(as)"));
    if(!r) return false;
    GVariant *arr = g_variant_get_child_value(r, 0);
    GVariantIter it;
    g_variant_iter_init(&it, arr);
    const gchar *s = nullptr;
    // 只留真部署了的方案(schema_list 里写了、但没文件的那些选过去没候选,名字也查不到)
    while(g_variant_iter_next(&it, "&s", &s))
        if(s && rime_schema_available(s)) schemas.emplace_back(s);
    g_variant_unref(arr);
    g_variant_unref(r);
    if(schemas.empty()) return false;

    std::string cur = _schema;
    GVariant *c = bus_call(_conn, kRimePath, kRimeIface, "GetCurrentSchema", nullptr,
                           G_VARIANT_TYPE("(s)"));
    if(c) {
        const gchar *name = nullptr;
        g_variant_get(c, "(&s)", &name);
        if(name && *name) cur = name;
        g_variant_unref(c);
    }

    size_t next = 0;
    for(size_t i = 0; i < schemas.size(); ++i) {
        if(schemas[i] == cur) {
            next = (i + 1) % schemas.size();
            break;
        }
    }
    bus_call(_conn, kRimePath, kRimeIface, "SetSchema",
             g_variant_new("(s)", schemas[next].c_str()), nullptr);
    _schema = schemas[next];
    clear_composition();
    return true;
}

std::string Fcitx5Ime::schema_name() const {
    return rime_schema_display_name(_schema);
}

// 翻页发的是 -/= 两个 keysym,不是 IC 的 PrevPage/NextPage 方法。fcitx5 的
// PrevPage/NextPage 底下是 RimeCandidateList::prev()/next(),那两个直接朝 librime 发
// Page_Up/Page_Down —— 这套 rime 的默认键表(以及设备上的 default.custom.yaml)压根没绑
// 这两个键,实测发过去连候选都不动。而 -/= 走 ProcessKeyEvent,会被 rime 的 key_binder
// 转成 Page_Up/Page_Down 在引擎内部执行,实测有效(用户报的「-=能翻页」就是这个)。
// 没有上/下一页时不发:那样 rime 会把 -/= 当标点插进编码里。
void Fcitx5Ime::prev_page() {
    if(!_has_prev) return;
    send_key((uint32_t)'-', 0);
}

void Fcitx5Ime::next_page() {
    if(!_has_next) return;
    send_key((uint32_t)'=', 0);
}
