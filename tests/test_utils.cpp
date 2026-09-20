#include "hash_utils.h"
#include "journal_storage.h"
#include "outline_model.h"
#include "process_utils.h"
#include "settings.h"

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
    if(int rc = test_outline_shift_subtree()) return rc;
    return 0;
}
