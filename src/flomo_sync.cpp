// 同步流程移植自 C1ancher/Flomo 的 syncStore(),并按 Flomo 官方 Web 客户端的做法
// 换成 memo/updated/ 的游标增量拉取:不再每次重拉全部历史。
#include "flomo_sync.h"

#include "flomo_api.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <utility>

namespace {

constexpr int kPageSize = 200;
constexpr int kMaxPages = 50;
constexpr int kReviewMax = 20;

std::mutex g_mu;
bool g_busy = false;
bool g_has_result = false;
Store g_result_store;
std::string g_result_status;

bool g_review_busy = false;
bool g_review_has_result = false;
std::vector<Memo> g_review_memos;
std::string g_review_status;

// 响应可能是裸数组,也可能包在 memos/memo_list/items/list 里。
const JsonValue *memo_array(const JsonValue &data) {
    if(data.isArray()) return &data;
    static const char *kKeys[] = {"memos", "memo_list", "items", "list"};
    for(const char *k : kKeys) {
        const JsonValue &v = data[k];
        if(v.isArray()) return &v;
    }
    return nullptr;
}

void push_dirty(FlomoApi &api, Store &store, std::string &status, bool &stop) {
    for(auto &m : store.memos) {
        if(!m.dirty) continue;
        ApiResult r;
        std::string op_name;
        if(m.pendingOp == "create") {
            r = api.create_memo(m.contentText);
            op_name = "创建";
        } else if(m.pendingOp == "update") {
            r = api.update_memo(m.slug, m.contentText, m.pin);
            op_name = "编辑";
        } else if(m.pendingOp == "delete") {
            // 本地新建、还没拿到远端 slug 的,删了就是删了,不用惊动服务器
            if(isLocalOnly(m)) {
                m.dirty = false;
                m.pendingOp.clear();
                continue;
            }
            r = api.delete_memo(m.slug);
            op_name = "删除";
        } else {
            continue;
        }
        if(!r.ok) {
            status = op_name + "失败: " + r.message;
            stop = true;
            return;
        }

        if(m.pendingOp == "create") {
            Memo remote = FlomoApi::memo_from_result(r);
            if(remote.slug.empty()) {
                m.dirty = false;
                m.pendingOp.clear();
                status = "创建已提交,但响应里没有 slug";
                stop = true;
                return;
            }
            remote.pin = m.pin;   // 服务端响应不一定回 pin,本地刚设的置顶别丢
            m = remote;
            if(m.pin != 0 && !m.slug.empty()) {
                // 创建接口不收 pin,置顶要单独补一次更新。失败不中止整轮同步 ——
                // 笔记已经建出来了,标回 dirty 让下一次同步补推。
                ApiResult u = api.update_memo(m.slug, m.contentText, m.pin);
                if(u.ok) {
                    Memo pinned = FlomoApi::memo_from_result(u);
                    if(!pinned.slug.empty()) {
                        pinned.pin = m.pin;
                        m = pinned;
                    }
                } else {
                    m.dirty = true;
                    m.pendingOp = "update";
                }
            }
        } else if(m.pendingOp == "update") {
            Memo remote = FlomoApi::memo_from_result(r);
            if(!remote.slug.empty()) {
                remote.pin = m.pin;
                m = remote;
            } else {
                m.dirty = false;
                m.pendingOp.clear();
            }
        } else {
            m.dirty = false;
            m.pendingOp.clear();
        }
    }
}

// 按游标增量拉取,每页推进一次游标并立刻写回 store —— 中途网络断了,
// 下次同步从这一页继续,不用重头拉。
bool pull_updated(FlomoApi &api, Store &store, std::string &status) {
    for(int page = 0; page < kMaxPages; ++page) {
        ApiResult r = api.list_updated(store.cursorUpdatedAt, store.cursorSlug);
        if(!r.ok) {
            status = "刷新失败: " + r.message;
            return false;
        }
        const JsonValue *arr = memo_array(r.data);
        if(!arr || arr->size() == 0) break;
        const size_t count = arr->size();

        for(size_t i = 0; i < count; ++i) {
            Memo m = memo_from_json((*arr)[i]);
            if(m.deleted) remove_memo_by_slug(store.memos, m.slug);
            else upsert_memo(store.memos, m);
        }

        // 游标一定用原始数组最后一条(即使它是删除项)推进,否则会卡死在同一页。
        const JsonValue &last = (*arr)[count - 1];
        const long long next = flomo_cursor_from_stamp(last["updated_at"].asString());
        const std::string nextSlug = last["slug"].asString();
        if(next == 0) {
            // updated_at 解析失败就不再推进,免得无限循环;下一次同步会重复这一页。
            break;
        }
        if(next == store.cursorUpdatedAt && nextSlug == store.cursorSlug) break;
        store.cursorUpdatedAt = next;
        store.cursorSlug = nextSlug;

        if(count < (size_t)kPageSize) break;
    }
    return true;
}

void sync_store(Store &store, std::string &status) {
    if(store.token.empty()) {
        status = "未登录 · L 键登录";
        return;
    }
    FlomoApi api(store.token);

    // 1) 推本地改动。任何一条失败就整体中止 —— 已经推成功的那些保留在 store 里,
    //    交回界面后照样落盘,用户改天再按 R 重试剩下的。
    bool stop = false;
    push_dirty(api, store, status, stop);
    if(stop) return;

    // 2) 已经确认删掉、也没有别的本地改动的,从本地库里真正去掉
    store.memos.erase(
        std::remove_if(store.memos.begin(), store.memos.end(),
                       [](const Memo &m) { return m.deleted && !m.dirty; }),
        store.memos.end());

    // 3) 增量拉远端
    if(!pull_updated(api, store, status)) return;

    store.lastSync = now_stamp();
    sort_memos(store.memos);
    int visible = 0;
    for(const auto &m : store.memos)
        if(!m.deleted) ++visible;
    status = "同步完成 " + std::to_string(visible) + " 条";
}

void worker(Store store) {
    std::string status;
    sync_store(store, status);
    // 结果发出去之后才放开 busy:主线程看到结果时,下一次同步一定可以安全开始。
    std::lock_guard<std::mutex> lk(g_mu);
    g_result_store = std::move(store);
    g_result_status = std::move(status);
    g_has_result = true;
    g_busy = false;
}

void review_worker(std::string token) {
    std::vector<Memo> memos;
    std::string status;
    if(token.empty()) {
        status = "未登录 · L 键登录";
    } else {
        FlomoApi api(token);
        ApiResult r = api.daily_review_slugs();
        if(!r.ok) {
            status = "今日回顾失败: " + r.message;
        } else {
            std::vector<std::string> slugs;
            const JsonValue *arr = memo_array(r.data);
            if(!arr && r.data.isArray()) arr = &r.data;
            if(arr) {
                for(size_t i = 0; i < arr->size() && (int)slugs.size() < kReviewMax; ++i) {
                    const JsonValue &item = (*arr)[i];
                    std::string slug = item.asString();
                    if(slug.empty()) slug = item["slug"].asString(item["memo_slug"].asString());
                    if(!slug.empty()) slugs.push_back(slug);
                }
            }
            for(const auto &slug : slugs) {
                ApiResult one = api.get_memo(slug);
                if(!one.ok) continue;   // 404(已删)和网络抖动都跳过
                Memo m = FlomoApi::memo_from_result(one);
                if(!m.slug.empty()) memos.push_back(m);
            }
            status = "今日回顾 " + std::to_string(memos.size()) + " 条";
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_review_memos = std::move(memos);
    g_review_status = std::move(status);
    g_review_has_result = true;
    g_review_busy = false;
}

}  // namespace

bool sync_start(const Store &snapshot) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if(g_busy) return false;
        g_busy = true;
        g_has_result = false;
    }
    std::thread(worker, snapshot).detach();
    return true;
}

bool sync_busy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_busy;
}

bool sync_take(Store &out_store, std::string &out_status) {
    std::lock_guard<std::mutex> lk(g_mu);
    if(!g_has_result) return false;
    out_store = std::move(g_result_store);
    out_status = std::move(g_result_status);
    g_result_store = Store {};
    g_result_status.clear();
    g_has_result = false;
    return true;
}

bool review_start(const std::string &token) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if(g_review_busy) return false;
        g_review_busy = true;
        g_review_has_result = false;
    }
    std::thread(review_worker, token).detach();
    return true;
}

bool review_busy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_review_busy;
}

bool review_take(std::vector<Memo> &out_memos, std::string &out_status) {
    std::lock_guard<std::mutex> lk(g_mu);
    if(!g_review_has_result) return false;
    out_memos = std::move(g_review_memos);
    out_status = std::move(g_review_status);
    g_review_memos.clear();
    g_review_status.clear();
    g_review_has_result = false;
    return true;
}
