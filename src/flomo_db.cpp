#include "flomo_db.h"

#include "flomo_api.h"   // memo_from_json / html_to_text
#include "json_parser.h"
#include "safe_file.h"   // pjournal src 的版本:safe_write_file 是原子写 + fsync
#include "settings.h"    // home_dir() / g_settings

#include <cstdio>
#include <string>

namespace {

std::string db_path() {
    return flomo_db_dir() + "/memos.json";
}

std::string legacy_db_path() {
    return home_dir() + "/.flomo-typix/memos.json";
}

bool load_from(const std::string &path, Store &store) {
    const std::string raw = read_whole_file(path);
    if(raw.empty()) return true;
    JsonValue root = JsonValue::parse(raw);
    if(!root.isObject()) {
        // 库文件坏了:挪到旁边留证据,然后按空库启动,别让下一次保存覆盖掉原文件。
        std::rename(path.c_str(), (path + ".corrupt-" + now_stamp()).c_str());
        return true;
    }
    store.token = root["token"].asString();
    store.lastSync = root["last_sync"].asString();
    store.cursorUpdatedAt = (long long)root["cursor_updated_at"].asNumber(0);
    store.cursorSlug = root["cursor_slug"].asString();
    const JsonValue &arr = root["memos"];
    if(arr.isArray()) {
        for(size_t i = 0; i < arr.size(); ++i) {
            Memo m = memo_from_json(arr[i]);
            // content_text 存的是本地编辑用的纯文本;远端返回的 HTML 还原结果可能和它
            // 不一致(比如用户手改过),以本地存的为准。
            m.contentText = arr[i]["content_text"].asString(m.contentText);
            m.dirty = arr[i]["dirty"].asBool(false);
            m.pendingOp = arr[i]["pending_op"].asString();
            m.pin = arr[i]["pin"].asInt(m.pin);
            // 按 slug 去重:旧版把库重复追加过一次,顺手把这种库修回来。
            upsert_memo(store.memos, m);
        }
    }
    sort_memos(store.memos);
    return true;
}

}  // namespace

std::string flomo_db_dir() {
    return home_dir() + "/.pjournal-lvgl/flomo";
}

bool flomo_db_load(Store &store) {
    // 调用方可能把同一份 store 反复传进来(界面每次进入都会加载),先清空,
    // 否则库内容会一遍遍追加到旧数据后面。
    store = Store {};
    ensure_dir_path(flomo_db_dir());
    const std::string path = db_path();
    if(!file_exists(path) && file_exists(legacy_db_path())) {
        // 旧版独立 app 的库:读进来、立刻写到新路径,老文件留着不动。
        load_from(legacy_db_path(), store);
        flomo_db_save(store);
    } else {
        load_from(path, store);
    }
    if(store.token.empty()) store.token = g_settings.get("flomo_token");
    return true;
}

bool flomo_db_save(const Store &store) {
    ensure_dir_path(flomo_db_dir());
    JsonValue root = JsonValue::object();
    root.set("token", JsonValue(store.token));
    root.set("last_sync", JsonValue(store.lastSync));
    root.set("cursor_updated_at", JsonValue((double)store.cursorUpdatedAt));
    root.set("cursor_slug", JsonValue(store.cursorSlug));
    JsonValue arr = JsonValue::array();
    for(const auto &m : store.memos) {
        JsonValue v = JsonValue::object();
        v.set("slug", JsonValue(m.slug));
        v.set("content", JsonValue(m.contentHtml));
        v.set("content_text", JsonValue(m.contentText));
        v.set("created_at", JsonValue(m.createdAt));
        v.set("updated_at", JsonValue(m.updatedAt));
        v.set("deleted_at", m.deleted ? JsonValue("1") : JsonValue(nullptr));
        v.set("pin", JsonValue(m.pin));
        v.set("dirty", JsonValue(m.dirty));
        v.set("pending_op", JsonValue(m.pendingOp));
        JsonValue tags = JsonValue::array();
        for(const auto &t : m.tags) tags.pushBack(JsonValue(t));
        v.set("tags", tags);
        arr.pushBack(v);
    }
    root.set("memos", arr);
    return safe_write_file(db_path(), root.serialize());
}
