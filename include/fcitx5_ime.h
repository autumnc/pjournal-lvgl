#pragma once

#include <gio/gio.h>
#include <lvgl.h>

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

// fcitx5 + rime 输入法后端。走 org.fcitx.Fcitx5 的 InputContext1 DBus 协议:
// 按键翻成 keysym 发过去,preedit/候选/上屏串从 UpdateClientSideUI 和 CommitString
// 信号收回来。词库和匹配全在 fcitx5+rime 那头,这边只当客户端。
//
// 只跑在主线程:glib 对象都建在默认 GMainContext 上,每次发键之后当场把排队的信号
// 抽干(g_main_context_iteration),所以 handle_key 能同步返回上屏串,和内置输入法
// 的契约完全一致。起不来的话(没有 session bus / fcitx5 没跑)一律降级成「没有输入法」,
// handle_key 恒返回 false,不崩。
class Fcitx5Ime {
public:
    using WidthFn = int (*)(const char *text);

    bool begin();
    void pump();

    bool connected() const {
        return _conn != nullptr && !g_dbus_connection_is_closed(_conn) && !_ic_path.empty();
    }

    bool active() const { return _active && connected(); }
    bool composing() const { return !_preedit.empty() || !_cands.empty(); }
    void toggle() { set_active(!_active); }
    void set_active(bool on);
    bool handle_key(int key, std::string &out);

    std::string composition() const { return _preedit; }
    const std::vector<std::string> &candidates() const { return _visible; }
    // 当前方案的显示名(雾凇拼音),给状态栏用。拿不到返回空串。
    std::string schema_name() const;
    int current_page() const { return _page; }
    // fcitx5 的客户端 UI 只给 hasPrev/hasNext,给不出总页数,只能这么近似:还有下一页
    // 就多算一页,否则当前页就是最后一页。
    int total_pages() const { return _page + (_has_next ? 1 : 0); }
    int total_candidates() const { return (int)_cands.size(); }
    int page_size() const { return _cands.empty() ? 1 : (int)_cands.size(); }
    void set_page_size(int) {}
    int highlight_index() const { return _highlight; }

    bool fullwidth() const { return _fullwidth; }
    bool trad() const { return _trad; }
    bool english() const { return _english; }
    void toggle_fullwidth();
    void toggle_trad();
    void toggle_english();

    // 分页权在 rime 手里:每页几个由它的 menu/page_size 定(设备上是 5),fcitx5 5.1 的
    // InputContext1 没有 SetPageSize 方法,客户端改不了这个数,也拿不到整份候选表,
    // 所以没法像内置输入法那样按宽度重排。只能按像素把这一页放不下的候选取掉,免得
    // 候选条撑出屏幕;编号不重排,「按 3 选第 3 个」对 rime 仍然成立——只是那一项看不见。
    // 5 个正常宽度下放得下,这里是防长词的兜底。
    void set_display_width(int);
    void set_width_fn(WidthFn);

    // Ctrl+Shift:循环切 rime 方案(ListAllSchemas + SetSchema)
    bool switch_schema();

    void prev_page();
    void next_page();

private:
    static void on_signal_trampoline(GDBusConnection *, const char *, const char *,
                                     const char *, const char *, GVariant *, void *);
    static void on_timer_trampoline(lv_timer_t *);

    bool send_key(uint32_t sym, uint32_t state);
    void connect_worker();
    void drain();
    void refresh_current_schema();
    void on_signal(const char *name, GVariant *params);
    void clear_composition();
    void rebuild_visible();
    void on_service_lost();

    GDBusConnection *_conn = nullptr;
    // g_bus_get_sync 没有超时参数:session bus 那个 daemon 一卡住它就永远不返回。而
    // begin() 是跑在 lv_timer_handler 里的(还握着 lv_lock),一卡整个界面就再也回不来
    // —— 这正是「卡死」那类症状。所以建连接丢给一条 worker 线程,主线程只来取结果,
    // 取不到就当作「还没有输入法」照常跑。
    //
    // _connecting 保证最多只有一条 worker 在飞。真卡死的那条**不会**被反复重启:glib
    // 自己给 g_bus_get 加了全局锁和缓存,再起一条也只是堵在同一把锁上,只会攒线程。
    // 代价是 session bus 彻底坏掉时输入法就一直不可用 —— 但那是很明确的降级,比冻屏好。
    std::atomic<GDBusConnection *> _pending_conn{nullptr};
    std::atomic<bool> _connecting{false};
    // 兜底定时器。没连上时按 100ms 跑(pump() 是来收 worker 建好的连接的地方,周期太长
    // 会让「启动后马上打字」那段时间没有输入法),连上后退回 1000ms 只用来发现掉线。
    lv_timer_t *_timer = nullptr;
    // 重连退避:连续失败 1s→2s→4s…最多 30s。失败时那一串同步 D-Bus 往返(每个都压着
    // lv_lock)不该每秒重来一遍。
    int _retry_backoff = 0;
    uint32_t _next_retry_ms = 0;
    guint _sig = 0;
    // NameOwnerChanged 的订阅;一直挂着(服务重启后还要靠它再发现一次),所以不跟着
    // _sig 一起退。
    guint _bus_sig = 0;
    std::string _ic_path;
    // 当前 rime 方案 id。GetCurrentSchema 是从「最近的输入上下文」里取的,没有会话时返回
    // 空串,所以本地记一份:begin() 探一次、switch_schema() 切完再更新。
    std::string _schema;

    std::string _preedit;
    std::vector<std::string> _cands;    // rime 这一页的全部候选
    std::vector<std::string> _visible;  // 按像素裁过、真正画出来的那一截
    int _budget = 0;                    // 候选区可用像素宽(0 = 不裁)
    WidthFn _width_fn = nullptr;
    int _highlight = -1;
    int _page = 1;
    bool _has_prev = false;
    bool _has_next = false;
    std::string _pending_commit;

    // 三个开关 rime 那边查不到状态(只有 ascii_mode 有 DBus 方法),本地跟着翻;
    // 因为 default.custom.yaml 的 save_options 里记着传统/全角,跨方案不会漂。
    bool _active = false;
    bool _fullwidth = false;
    bool _trad = false;
    bool _english = false;
};

Fcitx5Ime &fcitx5_ime();
