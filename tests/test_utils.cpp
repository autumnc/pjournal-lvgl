#include "flomo_api.h"
#include "flomo_db.h"
#include "flomo_model.h"
#include "hash_utils.h"
#include "journal_storage.h"
#include "outline_model.h"
#include "process_utils.h"
#include "settings.h"
#include "text_utils.h"

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const std::string &msg) {
    std::cerr << msg << "\n";
    return 1;
}

static bool exists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

static int test_journal_storage() {
    char templ[] = "/tmp/pjournal-test-XXXXXX";
    char *home = mkdtemp(templ);
    if(!home) return fail("mkdtemp failed");
    std::string root = home;
    std::string journal = root + "/journal";
    setenv("HOME", root.c_str(), 1);
    g_settings.begin();
    g_settings.set("journal_dir", journal);
    g_settings.set("version_history", "1");
    if(!g_journal.begin()) return fail("journal begin failed");

    if(!g_journal.save_entry_raw("entry.md", "first", false)) return fail("save entry failed");
    if(g_journal.read_entry("entry.md") != "first") return fail("read entry mismatch");
    if(g_journal.save_entry_raw("../escape.md", "bad", false)) return fail("unsafe save accepted");
    if(exists(root + "/escape.md")) return fail("unsafe save escaped journal dir");

    FILE *f = fopen((journal + "/plain.json").c_str(), "w");
    if(!f) return fail("create json fixture failed");
    fputs("{}", f);
    fclose(f);
    if(!g_journal.read_entry("plain.json").empty()) return fail("non-journal read accepted");
    if(g_journal.delete_entry("plain.json")) return fail("non-journal delete accepted");
    if(!exists(journal + "/plain.json")) return fail("non-journal file was deleted");

    if(!g_journal.save_entry_raw("entry.md", "second", true)) return fail("save entry history failed");
    auto versions = g_journal.list_history_versions("entry.md");
    if(versions.empty()) return fail("history version missing");
    if(g_journal.read_history_version("entry.md", versions.front().filename) != "first") return fail("history content mismatch");

    if(!g_journal.save_entry_raw("empty.md", "", false)) return fail("save empty entry failed");
    if(!g_journal.save_entry_raw("empty.md", "nonempty", true)) return fail("save empty history failed");
    auto empty_versions = g_journal.list_history_versions("empty.md");
    if(empty_versions.empty()) return fail("empty history missing");
    if(!g_journal.restore_history_version("empty.md", empty_versions.front().filename)) return fail("restore empty history failed");
    if(!g_journal.read_entry("empty.md").empty()) return fail("empty history restore content mismatch");

    // 列表只读文件头部,长正文不能把标题/预览解析弄坏
    const std::string long_body(20000, 'a');
    if(!g_journal.save_entry_raw("2026-01-02_030405.txt", make_journal_text("提示", long_body), false))
        return fail("save long entry failed");
    bool checked = false;
    for(const auto &e : g_journal.list_entries()) {
        if(e.filename != "2026-01-02_030405.txt") continue;
        checked = true;
        if(e.date != "2026-01-02") return fail("entry date mismatch");
        if(e.title != "提示写作") return fail("long entry title mismatch: " + e.title);
        if(e.preview != std::string(80, 'a')) return fail("long entry preview mismatch");
    }
    if(!checked) return fail("long entry missing from list");
    return 0;
}

static JsonValue ol_node(int level, const std::string &title) {
    JsonValue n = JsonValue::object();
    n.set("level", level);
    n.set("title", title);
    return n;
}

// 把整棵树压成一行 level 数字,方便和期望值直接比。
static std::string ol_levels(const JsonValue &nodes) {
    std::string s;
    for(int i = 0; i < (int)nodes.size(); ++i) s += std::to_string(nodes[i]["level"].asInt(0));
    return s;
}

static int test_outline_shift_subtree() {
    // A ─ B ─ C ─ C1 ─ D(C1 是 C 的子节点)
    JsonValue nodes = JsonValue::array();
    nodes.pushBack(ol_node(0, "A"));
    nodes.pushBack(ol_node(1, "B"));
    nodes.pushBack(ol_node(1, "C"));
    nodes.pushBack(ol_node(2, "C1"));
    nodes.pushBack(ol_node(0, "D"));
    if(ol_levels(nodes) != "01120") return fail("outline fixture levels wrong");

    // B 是 A 的第一个孩子,前面没有同层兄弟 → 降不了
    if(outline_shift_subtree(nodes, 1, +1)) return fail("demote accepted with no previous sibling");
    if(ol_levels(nodes) != "01120") return fail("rejected demote still changed levels");

    // C 的上一同层兄弟是 B → C 连同子节点 C1 一起降
    if(!outline_shift_subtree(nodes, 2, +1)) return fail("demote C rejected");
    if(ol_levels(nodes) != "01230") return fail("subtree did not follow demote: " + ol_levels(nodes));

    if(!outline_shift_subtree(nodes, 2, -1)) return fail("promote C rejected");
    if(ol_levels(nodes) != "01120") return fail("subtree did not follow promote: " + ol_levels(nodes));

    if(outline_shift_subtree(nodes, 0, -1)) return fail("promote accepted at top level");
    if(ol_levels(nodes) != "01120") return fail("rejected promote still changed levels");

    // D 得越过 C1(2)、C(1)、B(1)才找到同层的 A —— 找的是上一同层兄弟,不是上一行
    if(!outline_shift_subtree(nodes, 4, +1)) return fail("demote D rejected");
    if(ol_levels(nodes) != "01121") return fail("demote D landed under the wrong node: " + ol_levels(nodes));
    return 0;
}

static int test_flomo_protocol() {
    if(text_to_html("hello") != "<p>hello</p>") return fail("text_to_html paragraph mismatch");
    if(html_to_text("<p>hello</p>") != "hello") return fail("html_to_text paragraph mismatch");
    if(html_to_text(text_to_html("a\n\nb")) != "a\n\nb") return fail("blank line roundtrip mismatch");
    if(html_to_text(text_to_html("- a\n- b")) != "- a\n- b") return fail("list roundtrip mismatch");
    if(html_to_text(text_to_html("1. a\n2. b")) != "1. a\n2. b") return fail("ordered list roundtrip mismatch");
    const std::string inline_md = "**粗** __下划线__ ==高亮==";
    if(html_to_text(text_to_html(inline_md)) != inline_md)
        return fail("inline markdown roundtrip mismatch: " + html_to_text(text_to_html(inline_md)));

    auto tags = extract_tags("今天 #a 还有 #a/b 和重复 #a");
    if(tags.size() != 3) return fail("extract_tags count mismatch");
    if(tags[0] != "a" || tags[1] != "a/b" || tags[2] != "a") return fail("extract_tags value mismatch");

    JsonValue memo = JsonValue::object();
    memo.set("slug", JsonValue("abc"));
    memo.set("content", JsonValue("<p>正文 #tag</p>"));
    memo.set("content_text", JsonValue("正文 #tag"));
    memo.set("updated_at", JsonValue("2026-11-07 09:00:00"));
    memo.set("deleted_at", JsonValue(nullptr));
    memo.set("pin", JsonValue(1));
    Memo m = memo_from_json(memo);
    if(m.slug != "abc" || !m.pinned()) return fail("memo_from_json slug/pin mismatch");
    if(m.deleted) return fail("memo_from_json deleted flag mismatch");
    if(m.tags.size() != 1 || m.tags[0] != "tag") return fail("memo_from_json tag extraction mismatch");

    memo.set("deleted_at", JsonValue("2026-11-07 10:00:00"));
    if(!memo_from_json(memo).deleted) return fail("memo_from_json deleted_at ignored");

    // 2026-11-07 09:00:00 (UTC+8) == 2026-11-07 01:00:00 UTC
    if(flomo_cursor_from_stamp("2026-11-07 09:00:00") != 1794013200LL)
        return fail("cursor conversion mismatch");
    if(flomo_cursor_from_stamp("1970-01-01 08:00:00") != 0) return fail("cursor epoch boundary mismatch");
    if(flomo_cursor_from_stamp("") != 0) return fail("cursor empty stamp mismatch");
    return 0;
}

static int test_flomo_db() {
    Store store;
    store.token = "tok-123";
    store.lastSync = "2026-11-07 09:30:00";
    store.cursorUpdatedAt = 1794018000LL;
    store.cursorSlug = "slug-last";
    Memo m;
    m.slug = "slug-1";
    m.contentHtml = "<p>hi</p>";
    m.contentText = "hi";
    m.updatedAt = "2026-11-07 09:00:00";
    m.pin = 1;
    m.dirty = true;
    m.pendingOp = "update";
    m.tags = {"a", "a/b"};
    store.memos.push_back(m);
    if(!flomo_db_save(store)) return fail("flomo_db_save failed");

    Store loaded;
    if(!flomo_db_load(loaded)) return fail("flomo_db_load failed");
    if(loaded.token != store.token) return fail("flomo db token mismatch");
    if(loaded.lastSync != store.lastSync) return fail("flomo db last_sync mismatch");
    if(loaded.cursorUpdatedAt != store.cursorUpdatedAt) return fail("flomo db cursor mismatch");
    if(loaded.cursorSlug != store.cursorSlug) return fail("flomo db cursor slug mismatch");
    if(loaded.memos.size() != 1) return fail("flomo db memo count mismatch");
    const Memo &lm = loaded.memos[0];
    if(lm.slug != "slug-1" || lm.contentText != "hi") return fail("flomo db memo content mismatch");
    if(!lm.pinned() || !lm.dirty || lm.pendingOp != "update") return fail("flomo db memo flags mismatch");
    if(lm.tags.size() != 2 || lm.tags[1] != "a/b") return fail("flomo db memo tags mismatch");

    // 界面每次进入都会对同一个 Store 调一次 load;不能把库内容追加成两份
    if(!flomo_db_load(loaded)) return fail("flomo db reload failed");
    if(loaded.memos.size() != 1) return fail("flomo db reload duplicated memos");
    return 0;
}

int main() {
    if(hash::md5_hex("") != "d41d8cd98f00b204e9800998ecf8427e") return fail("md5 empty mismatch");
    if(hash::md5_hex("abc") != "900150983cd24fb0d6963f7d28e17f72") return fail("md5 abc mismatch");
    if(hash::md5_hex("message digest") != "f96b697d7cb7938d525a2f31aaf161d0") return fail("md5 phrase mismatch");

    if(!process::command_exists("sh")) return fail("expected sh in PATH");
    if(process::command_exists("pjournal-command-that-should-not-exist")) return fail("unexpected command exists");

    const char *printf_path = access("/usr/bin/printf", X_OK) == 0 ? "/usr/bin/printf" : "/bin/printf";
    if(access(printf_path, X_OK) != 0) return fail("printf not found");
    if(process::run_capture({printf_path, "hello"}) != "hello") return fail("run_capture printf mismatch");
    if(int rc = test_journal_storage()) return rc;
    if(int rc = test_flomo_protocol()) return rc;
    if(int rc = test_flomo_db()) return rc;
    if(int rc = test_outline_shift_subtree()) return rc;
    return 0;
}
