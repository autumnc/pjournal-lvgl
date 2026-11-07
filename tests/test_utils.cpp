#include "flomo_api.h"
#include "flomo_db.h"
#include "flomo_model.h"
#include "hash_utils.h"
#include "journal_storage.h"
#include "outline_model.h"
#include "process_utils.h"
#include "settings.h"
#include "term_keys.h"
#include "term_settings.h"
#include "term_sixel.h"
#include "term_theme.h"
#include "term_vt.h"
#include "text_utils.h"

#include <clocale>
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

static int test_term_vt() {
    if(!setlocale(LC_CTYPE, "C.utf8") && !setlocale(LC_CTYPE, "en_US.utf8"))
        return fail("no utf-8 locale for wcwidth");

    term::Vt vt(20, 5);
    vt.feed("abc\n", 4);
    if(vt.row_text(0) != "abc") return fail("vt row text mismatch");
    if(vt.cursor_y() != 1 || vt.cursor_x() != 3) return fail("vt newline cursor mismatch");

    vt.feed("\033[2J", 4);
    if(!vt.row_text(0).empty()) return fail("vt erase display failed");

    // SGR:粗体 + 红(调色板 1)
    term::Vt styled(20, 5);
    styled.feed("\033[1;31mX", 8);
    const term::Cell *c = styled.display_row(0);
    if(!c || c->ch != 'X') return fail("vt sgr text mismatch");
    if(!(c->attr & term::kAttrBold)) return fail("vt bold attr missing");
    if(c->fg != term::palette_color(1)) return fail("vt fg palette mismatch");

    // 中文双宽:第一格是汉字,第二格是宽字尾
    term::Vt cjk(20, 5);
    cjk.feed("中", 3);
    const term::Cell *cc = cjk.display_row(0);
    if(!cc || cc->ch != 0x4e2d) return fail("vt wide char mismatch");
    if(!(cc[1].attr & term::kAttrWideTail)) return fail("vt wide tail missing");

    // 回看
    term::Vt scroll(20, 3);
    for(int i = 0; i < 10; ++i) {
        const std::string line = "line" + std::to_string(i) + "\n";
        scroll.feed(line.data(), line.size());
    }
    if(scroll.scrollback_lines() <= 0) return fail("vt scrollback empty");
    scroll.set_view_offset(1);
    if(!scroll.viewing_scrollback()) return fail("vt view offset not applied");
    scroll.scroll_to_bottom();
    if(scroll.viewing_scrollback()) return fail("vt scroll to bottom failed");

    // 备用屏
    term::Vt alt(20, 5);
    alt.feed("main", 4);
    alt.feed("\033[?1049h", 8);
    if(!alt.alt_screen()) return fail("vt alt screen not entered");
    alt.feed("alt", 3);
    alt.feed("\033[?1049l", 8);
    if(alt.alt_screen()) return fail("vt alt screen not left");
    if(alt.row_text(0) != "main") return fail("vt main screen not restored");

    // DSR/CPR
    term::Vt reply(20, 5);
    reply.feed("\033[2;3H\033[6n", 10);
    std::string out;
    if(!reply.take_reply(out) || out != "\033[2;3R") return fail("vt cpr reply mismatch: " + out);

    // PUA 图标(Nerd Font)永远占一格 —— 字体里墨迹再宽也不能变双宽字
    term::Vt pua(20, 5);
    pua.feed("\xef\x84\xa0X", 4);  // U+F120 终端图标 + 'X'
    if(pua.row_text(0) != "\xef\x84\xa0X") return fail("vt pua row mismatch");
    if(pua.cursor_x() != 2) return fail("vt pua glyph was widened");

    // sixel:以光标为锚入库,画完光标落到图像下方
    term::Vt six(20, 5);
    six.set_cell_size(8, 16);
    const std::string sixel = "\033Pq\"1;1;2;2#0;2;100;0;0~~\033\\";
    six.feed(sixel.data(), sixel.size());
    if(six.images().size() != 1) return fail("vt sixel image missing");
    if(six.cursor_x() != 0 || six.cursor_y() != 1) return fail("vt sixel cursor advance mismatch");

    // 清屏(ED 2)必须把图一起清掉,否则 clear 之后图还浮在空屏上
    six.feed("\033[2J", 4);
    if(!six.images().empty()) return fail("vt clear screen left sixel image");

    // 文字写进图占的格子时图也留不住(全屏 TUI 靠写空格擦图)
    term::Vt over(20, 5);
    over.set_cell_size(8, 16);
    over.feed(sixel.data(), sixel.size());
    const std::string jump = "\033[1;1Hx";
    over.feed(jump.data(), jump.size());
    if(!over.images().empty()) return fail("vt text overwrite kept sixel image");

    // 只盖住图的一角:图要留着,被盖住的像素变透明 —— img2sixel 画完图后 shell
    // 在收紧后的光标行打印 prompt,不能因此把整张图抹掉
    term::Vt punch(20, 5);
    punch.set_cell_size(8, 16);
    std::string big = "\033Pq\"1;1;20;24#0;2;100;0;0";
    for(int band = 0; band < 4; ++band) {
        if(band) big += "-";
        big += "!20~";
    }
    big += "\033\\";
    punch.feed(big.data(), big.size());
    if(punch.images().size() != 1 || punch.images()[0].w != 20 || punch.images()[0].h != 24)
        return fail("vt wide sixel fixture mismatch");
    const std::string corner = "\033[1;2Hx";
    punch.feed(corner.data(), corner.size());
    if(punch.images().size() != 1) return fail("partial overwrite deleted the whole image");
    const term::SixelImage &pimg = punch.images()[0];
    if(pimg.argb[0] == 0) return fail("partial overwrite punched untouched pixels");
    if(pimg.argb[5 * 20 + 8] != 0) return fail("partial overwrite left covered pixels visible");

    // 备用屏(yazi 等全屏 TUI)上的 sixel 也要画;回到主屏时随备用屏丢弃
    term::Vt altimg(20, 5);
    altimg.set_cell_size(8, 16);
    altimg.feed("\033[?1049h", 8);
    altimg.feed(sixel.data(), sixel.size());
    if(altimg.images().size() != 1 || !altimg.images()[0].alt)
        return fail("vt alt screen sixel missing");
    altimg.feed("\033[?1049l", 8);
    if(!altimg.images().empty()) return fail("vt alt screen kept image after exit");

    // DA1 要报 sixel 属性(4),yazi 靠它判断能不能直接发图
    term::Vt da(20, 5);
    da.feed("\033[0c", 4);
    std::string da_reply;
    if(!da.take_reply(da_reply) || da_reply.find("?62;4c") == std::string::npos)
        return fail("vt da1 did not advertise sixel: " + da_reply);

    // CSI 16 t:单格像素尺寸(yazi 用它算图缩放)
    term::Vt ct(20, 5);
    ct.set_cell_size(8, 16);
    ct.feed("\033[16t", 5);
    std::string ct_reply;
    if(!ct.take_reply(ct_reply) || ct_reply != "\033[6;16;8t")
        return fail("vt cell size reply mismatch: " + ct_reply);

    // Kitty 图形探测是 APC(ESC _ ... ST),不能漏成普通文字
    term::Vt apc(40, 5);
    const std::string probe = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\X";
    apc.feed(probe.data(), probe.size());
    if(apc.row_text(0) != "X") return fail("vt apc leaked as text: " + apc.row_text(0));
    return 0;
}

static int test_term_theme() {
    const term::TermTheme &th = term::theme_current();
    if(th.palette[7] == 0) return fail("theme text palette missing");
    if(std::string(term::theme_by_id("no-such-theme").id) != "typix")
        return fail("theme unknown id did not fall back");
    return 0;
}

static int test_term_settings() {
    char templ[] = "/tmp/pjournal-term-XXXXXX";
    char *dir = mkdtemp(templ);
    if(!dir) return fail("term settings mkdtemp failed");
    const std::string path = std::string(dir) + "/terminal.conf";

    term::TermSettings &st = term::term_settings();
    st.font_path = "/tmp/font-a.ttf";
    st.font_size = 36;
    st.ui_font_size = 22;
    st.theme_id = "gruvbox";
    st.cursor_style = 2;
    st.cursor_blink = false;
    st.scrollback = 500;
    if(!term::settings_save(path)) return fail("term settings save failed");

    // 先改掉,load 必须把没写的键复位成默认值
    st.font_path.clear();
    st.font_size = 12;
    if(!term::settings_load(path)) return fail("term settings load failed");
    if(st.font_path != "/tmp/font-a.ttf") return fail("term settings font mismatch");
    if(st.font_size != 36) return fail("term settings size mismatch");
    if(st.ui_font_size != 22) return fail("term settings ui size mismatch");
    if(st.theme_id != "gruvbox") return fail("term settings theme mismatch");
    if(st.cursor_style != 2 || st.cursor_blink) return fail("term settings cursor mismatch");
    if(st.scrollback != 500) return fail("term settings scrollback mismatch");
    return 0;
}

static int test_term_keys() {
    if(term::bytes_for(8, false) != std::string(1, '\x7f')) return fail("key backspace mismatch");
    if(term::bytes_for(127, false) != "\033[3~") return fail("key delete mismatch");
    if(term::bytes_for('\n', false) != "\r") return fail("key enter mismatch");
    if(term::bytes_for(KEY_UP, false) != "\033[A") return fail("key up mismatch");
    if(term::bytes_for(KEY_UP, true) != "\033OA") return fail("key app cursor up mismatch");
    if(!term::bytes_for(term::kKeyMenu, false).empty()) return fail("key F10 leaked to shell");
    if(term::bytes_for(term::kKeyF1, false) != "\033OP") return fail("key F1 mismatch");
    if(term::scroll_lines(KEY_SHIFT_UP, 20) != 1) return fail("scroll up mismatch");
    if(term::scroll_lines(KEY_SHIFT_DOWN, 20) != -1) return fail("scroll down mismatch");
    if(term::scroll_lines(KEY_UP, 20) != 0) return fail("scroll plain up mismatch");
    return 0;
}

static int test_term_sixel() {
    // 2x2 的红块:栅格声明 + 定义 0 号色 + 两个全亮的 sixel 字符
    const std::string data = "\"1;1;2;2#0;2;100;0;0~~";
    term::SixelBitmap bmp;
    if(!term::sixel_decode(data.data(), data.size(), 1024, 0, bmp)) return fail("sixel decode failed");
    if(bmp.w != 2 || bmp.h < 2) return fail("sixel size mismatch");
    if(bmp.argb[0] != 0xffff0000u) return fail("sixel pixel mismatch");

    // 没定义过的色号用 VT340 默认表(1 号是蓝 0,0,205)
    term::SixelBitmap def;
    const std::string blue = "#1~~";
    if(!term::sixel_decode(blue.data(), blue.size(), 1024, 0, def)) return fail("sixel default color failed");
    if(def.argb[0] != 0xff0000cdu) return fail("sixel vt340 palette mismatch");

    // 16..255 用 xterm 调色板(16 号是最深的黑)
    term::SixelBitmap xt;
    const std::string idx16 = "#16~~";
    if(!term::sixel_decode(idx16.data(), idx16.size(), 1024, 0, xt)) return fail("sixel xterm color failed");
    if(xt.argb[0] != 0xff000000u) return fail("sixel xterm palette mismatch");

    // 0 号色 = 当前前景色;没给前景就是 VT340 黑
    term::SixelBitmap fg;
    const std::string c0 = "#0~~";
    if(!term::sixel_decode(c0.data(), c0.size(), 1024, 0x112233u, fg)) return fail("sixel fg color failed");
    if(fg.argb[0] != 0xff112233u) return fail("sixel fg color mismatch");
    if(!term::sixel_decode(c0.data(), c0.size(), 1024, 0, fg)) return fail("sixel fg fallback failed");
    if(fg.argb[0] != 0xff000000u) return fail("sixel fg fallback mismatch");
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
    if(int rc = test_term_vt()) return rc;
    if(int rc = test_term_theme()) return rc;
    if(int rc = test_term_settings()) return rc;
    if(int rc = test_term_keys()) return rc;
    if(int rc = test_term_sixel()) return rc;
    return 0;
}
