// yong 引擎的 C 侧桥接。src/yong_ime.cpp 是纯 C++ 驱动,碰引擎的地方全在这里。
//
// 引擎本体(mb/face.c 里那个 EXTRA_IM EIM)是自包含的 C,但它要的平台回调都写在
// common/ 里(common/im.c 的 InitExtraIM 是模板),而那些文件拖着 glib 编不进来。
// 这里把驱动侧真正要用到的回调照抄一遍,只依赖 llib —— 和 common/common.c 里
// y_im_get_path / y_im_open_file / y_im_str_to_key 的语义对齐。
//
// 编码边界(照 yong 来的):
//   yong.ini 的值、arg、overlay、OpenFile 收到的路径  → UTF-8
//   EIM.CodeInput / EIM.StringGet / EIM.CandTable     → GBK
//   本文件对外(cpp 侧)一律给 UTF-8。

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "llib.h"
#include "yong.h"

// 设备上的 yong 数据目录。x86 本机试的时候用 PJOURNAL_YONG_DATA 环境变量顶掉。
#ifndef PJOURNAL_YONG_DATA
#define PJOURNAL_YONG_DATA "/root/yong"
#endif

// mb/face.c: L_EXPORT(EXTRA_IM EIM)={...}
extern EXTRA_IM EIM;

// src/yong_s2t.c
extern const char *yong_s2t(const char *gbk);

// common/bihua.c:拆字(笔画)引擎。它要的 YongGetPunc / y_im_* 这些驱动侧符号由
// src/yong_bihua_compat.c 补上。
void *y_bihua_eim(void);
int y_bihua_good(void);

// src/yong_bihua_compat.c
void yb_compat_set_im_eim(void *e);

#define YB_CAND_MAX 10
#define YB_IM_MAX 64
#define YB_PATH_MAX 768

// 引擎自己用 CandWordMax 分页,这个值就是我们每页给几个候选。9 和内置输入法的
// 候选条(竖排固定 9 行)对齐。
#define YB_CAND_PER_PAGE 9

struct yb_im {
    int index;                        // [IM] 里的编号
    char section[64];                 // 那个编号指向的节名,如 pinyin
    char name[128];                   // [<section>] name,UTF-8
    char arg[256];                    // [<section>] arg,UTF-8
    char overlay[256];                // [<section>] overlay,UTF-8
};

static EXTRA_IM *const eim = &EIM;

// 当前真正在跑的那个引擎:平时是 mb 方案引擎,进了拆字模式指向 bihua 引擎。
// 下面所有对外的取状态 / 喂键都走 g_active,驱动侧不用关心现在是谁。
static EXTRA_IM *g_active = &EIM;
// 拆字引擎(common/bihua.c 里那个 EXTRA_IM),第一次进拆字时惰性挂回调 + Init
static EXTRA_IM *g_bihua = NULL;
static int g_bihua_inited = 0;
static int g_key_bihua = '`';

// 拆字的 CodeInput 是「拆字键 + 每个键的笔画名」,一个字最多 3 字节,能把
// MAX_CODE_LEN 撑出三四倍,所以这个缓冲比 mb 引擎自己用的那个宽。
static char g_code_input[MAX_CODE_LEN * 4 + 8];
static char g_string_get[MAX_CAND_LEN + 2];
static char g_cand_table[YB_CAND_MAX][MAX_CAND_LEN + 1];
static char g_code_tips[YB_CAND_MAX][MAX_TIPS_LEN + 1];

static char g_home[YB_PATH_MAX];
static char g_data[YB_PATH_MAX];

static LKeyFile *g_main_cfg = NULL;
static LKeyFile *g_sub_cfg = NULL;

static struct yb_im g_ims[YB_IM_MAX];
static int g_im_count = 0;
static int g_im_pos = 0;

static int g_ready = 0;
static int g_loaded = 0;

// SendString 攒下来的上屏串(GBK)。yong 用它在「候选是 $SPACE 之类命令」时先把
// 编码对应的字发出去、再把命令留在 StringGet 里;驱动侧得先收着,等 IMR_COMMIT
// 一起吐。
static char g_pending[MAX_CAND_LEN * 4 + 4];
static int g_pending_len = 0;

// 当前 IM 的 [key] 配置。引擎拿不到这些(翻页/选字/临时英文在驱动侧),所以这边自己存
// 一份。注意 overlay 是按输入法覆盖 [key] 的(mb/pinyin.ini 里 tEN=v),所以这份要在
// yb_load_im 里、l_key_file_set_overlay 之后读。
static char g_select_n[16] = "1234567890";
static int g_key_pageup = '-';
static int g_key_pagedown = '=';
static int g_key_commit = YK_SPACE;
static int g_key_temp_english = YK_NONE;

int yb_load_im(int pos);
void yb_bihua_leave(void);

/* ---------------------------------------------------------------- 路径 */

static int yb_mkdir_p(const char *path, int mode) {
    char temp[YB_PATH_MAX];
    size_t len = strlen(path);
    if(len == 0 || len >= sizeof temp) return -1;
    memcpy(temp, path, len + 1);
    for(char *p = temp + 1; *p; ++p) {
        if(*p != '/') continue;
        *p = 0;
        mkdir(temp, mode);
        *p = '/';
    }
    return mkdir(temp, mode);
}

// yong 的 y_im_get_path 在裸 Linux 上把 DATA 退化成 ".."(指望着 cwd 就是数据目录),
// 这里不回退,cwd 跟数据目录没关系。
static void yb_resolve_paths(void) {
    const char *home = getenv("HOME");
    const char *cfg = getenv("XDG_CONFIG_HOME");
    char xdg[YB_PATH_MAX];

    if(cfg && *cfg) snprintf(xdg, sizeof xdg, "%s", cfg);
    else if(home && *home) snprintf(xdg, sizeof xdg, "%s/.config", home);
    else snprintf(xdg, sizeof xdg, ".");

    snprintf(g_home, sizeof g_home, "%s/yong", xdg);
    if(!l_file_exists(g_home)) {
        char legacy[YB_PATH_MAX];
        snprintf(legacy, sizeof legacy, "%s/.yong", home ? home : ".");
        if(l_file_exists(legacy)) snprintf(g_home, sizeof g_home, "%s", legacy);
        else yb_mkdir_p(g_home, 0700);
    }
    snprintf(g_data, sizeof g_data, "%s", PJOURNAL_YONG_DATA);
    {
        const char *override = getenv("PJOURNAL_YONG_DATA");
        if(override && *override) snprintf(g_data, sizeof g_data, "%s", override);
    }
}

/* -------------------------------------------------------------- 配置层 */

static char g_cfg_ret[512];

static char *yb_get_config(const char *section, const char *key) {
    char *tmp;
    if(!g_main_cfg || !key) return NULL;
    // section==NULL 是「当前输入法那一节」(如 [pinyin]),不是字面量 NULL 组。
    if(!section) {
        if(g_im_pos < 0 || g_im_pos >= g_im_count) return NULL;
        section = g_ims[g_im_pos].section;
    }
    tmp = l_key_file_get_string(g_main_cfg, section, key);
    if(!tmp) return NULL;
    if(strlen(tmp) >= sizeof g_cfg_ret - 1) {
        l_free(tmp);
        return NULL;
    }
    strcpy(g_cfg_ret, tmp);
    l_free(tmp);
    return g_cfg_ret;
}

static const char *yb_get_path(const char *type) {
    static char lib[YB_PATH_MAX];
    if(!type) return g_data;
    if(!strcmp(type, "HOME")) return g_home;
    if(!strcmp(type, "LIB")) {
        snprintf(lib, sizeof lib, "%s/l32", g_data);
        if(l_file_exists(lib)) return lib;
        return ".";
    }
    return g_data;
}

static void *yb_open_file(const char *fn, const char *mode) {
    if(!fn || !fn[0] || !mode) return NULL;
    if(fn[0] == '/') return l_file_open(fn, mode, NULL);
    if(strchr(mode, 'w')) {
        // 引擎写的是用户词库(如 pinyin.usr),落 HOME;HOME 下没有子目录就先建出来。
        char path[YB_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", g_home, fn);
        char *slash = strrchr(path, '/');
        if(slash) {
            *slash = 0;
            if(!l_file_exists(path)) yb_mkdir_p(path, 0700);
        }
    }
    return l_file_open(fn, mode, g_home, g_data, NULL);
}

/* ------------------------------------------------------------ 按键名解析 */

// common/common.c:str_key_map 的裁剪版(去掉了 repeat/超时重复那套,驱动侧不用)。
static const struct {
    const char *name;
    int key;
} YB_KEY_MAP[] = {
    {"NONE", 0},          {"LCTRL", YK_LCTRL},   {"RCTRL", YK_RCTRL},
    {"LSHIFT", YK_LSHIFT}, {"RSHIFT", YK_RSHIFT}, {"LALT", YK_LALT},
    {"RALT", YK_RALT},    {"LWIN", YK_LWIN},     {"RWIN", YK_RWIN},
    {"TAB", YK_TAB},      {"ESC", YK_ESC},       {"ENTER", YK_ENTER},
    {"BACKSPACE", YK_BACKSPACE}, {"SPACE", YK_SPACE}, {"DEL", YK_DELETE},
    {"HOME", YK_HOME},    {"LEFT", YK_LEFT},     {"UP", YK_UP},
    {"DOWN", YK_DOWN},    {"RIGHT", YK_RIGHT},   {"PGUP", YK_PGUP},
    {"PGDN", YK_PGDN},    {"PAGEUP", YK_PGUP},   {"PAGEDOWN", YK_PGDN},
    {"END", YK_END},      {"INSERT", YK_INSERT}, {"CAPSLOCK", YK_CAPSLOCK},
    {"COMMA", ','},       {"BACK", YK_BACK},     {NULL, 0},
};

// common/common.c:y_im_str_to_key(s,NULL)
static int yb_str_to_key(const char *s) {
    char tmp[16];
    const char *p = s;
    int key = 0;

    if(!s || !s[0] || s[0] == '_') return -1;
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s + 2, NULL, 16);
    while(p[0]) {
        int i;
        for(i = 0; i < 15; i++) {
            tmp[i] = *p;
            if(!tmp[i] || tmp[i] == ' ') break;
            p++;
            if(i > 0 && tmp[i] == '_') break;
        }
        tmp[i] = 0;
        if(i == 0) return -2;
        if(i == 1) {
            if(!isgraph((unsigned char)tmp[0])) return -4;
            key |= (unsigned char)tmp[0];
            if(p[0] && p[0] != ' ') return -3;
            break;
        }
        if(!strcmp(tmp, "CTRL")) key |= KEYM_CTRL;
        else if(!strcmp(tmp, "SHIFT")) key |= KEYM_SHIFT;
        else if(!strcmp(tmp, "ALT")) key |= KEYM_ALT;
        else if(!strcmp(tmp, "WIN")) key |= KEYM_SUPER;
        else {
            for(i = 0; YB_KEY_MAP[i].name; i++) {
                if(!strcmp(YB_KEY_MAP[i].name, tmp)) {
                    key |= YB_KEY_MAP[i].key;
                    break;
                }
            }
            if(!YB_KEY_MAP[i].name) return -9;
            if(p[0] == ' ') break;
        }
    }
    return key;
}

static int yb_get_key(const char *s) { return yb_str_to_key(s); }

/* --------------------------------------------------------------- 选字键 */

// common/main.c:y_im_check_select 的裁剪版。key_select[] / key_select1[](设备上是
// LSHIFT/RSHIFT)在我们这条路上永远撞不上 ASCII,所以只剩 commit(空格)和
// key_select_n(数字)两支。
static int yb_check_select(int key, int flags) {
    if((flags & 0x01) && key == g_key_commit) return 0;
    if(flags & 0x04) {
        const char *p;
        if(key & KEYM_MASK) return -1;
        p = strchr(g_select_n, key);
        if(p && *p) return (int)(p - g_select_n);
    }
    if(flags & 0x08) {
        int i = key - YK_VIRT_SELECT;
        if(i >= 0 && i < 10) return i;
    }
    return -1;
}

/* ------------------------------------------------------------- 上屏输出 */

static void yb_send_string(const char *s, int flag) {
    int len;
    (void)flag;
    if(!s || !s[0]) return;
    len = (int)strlen(s);
    if(g_pending_len + len >= (int)sizeof g_pending) return;
    memcpy(g_pending + g_pending_len, s, len);
    g_pending_len += len;
    g_pending[g_pending_len] = 0;
}

static int yb_query_history(const char *pre, char out[][MAX_CAND_LEN + 1], int max) {
    (void)pre;
    (void)out;
    (void)max;
    return 0;
}

static const char *yb_get_last(int len) {
    (void)len;
    return NULL;
}

static void yb_show_tip(const char *fmt, ...) { (void)fmt; }

static const char *yb_translate(const char *s) { return s; }

static void yb_log(const char *fmt, ...) { (void)fmt; }

// common/im.c:eim_callback + common/async.c 的同步版
static int yb_callback(int index, ...) {
    va_list ap;
    int res = -1;

    va_start(ap, index);
    switch(index) {
    case EIM_CALLBACK_ASYNC_WRITE_FILE: {
        const char *file = va_arg(ap, const char *);
        LString *data = va_arg(ap, LString *);
        int backup = va_arg(ap, int);
        char orig[YB_PATH_MAX];
        char dest[YB_PATH_MAX];
        FILE *fp;
        if(!file || !data) {
            l_string_free(data);
            break;
        }
        if(backup) {
            // 先备份旧的,再覆盖:写坏了还能从 .bak 里捞回来
            snprintf(orig, sizeof orig, "%s/%s", g_home, file);
            snprintf(dest, sizeof dest, "%s.bak", orig);
            l_remove(dest);
            rename(orig, dest);
        }
        fp = (FILE *)yb_open_file(file, "wb");
        if(fp) {
            if(data->len > 0) fwrite(data->str, (size_t)data->len, 1, fp);
            fclose(fp);
            res = 0;
        }
        l_string_free(data);
        break;
    }
    case EIM_CALLBACK_SELECT_KEY: {
        int key = va_arg(ap, int);
        if(!key) {
            const char *keys = va_arg(ap, const char *);
            if(keys) {
                for(int i = 0; keys[i]; i++) {
                    int pos = yb_check_select(keys[i], 0xf);
                    if(pos >= 0) {
                        res = pos + 1;
                        break;
                    }
                }
            }
        } else {
            int pos = yb_check_select(key, 0xf);
            if(pos >= 0) res = pos + 1;
        }
        break;
    }
    case EIM_CALLBACK_SET_ASSIST_CODE:
        va_arg(ap, const char *);
        break;
    default:
        break;
    }
    va_end(ap);
    return res;
}

/* ------------------------------------------------------------ 引擎挂钩 */

// 把平台回调挂到引擎上。mb 方案引擎和拆字引擎用的是同一套回调,所以两边都调这个。
static void yb_hook(EXTRA_IM *e) {
    e->CodeInput = g_code_input;
    e->StringGet = g_string_get;
    e->CandTable = g_cand_table;
    e->CodeTips = g_code_tips;
    e->CandWordMax = YB_CAND_PER_PAGE;
    e->CandWordMaxReal = YB_CAND_PER_PAGE;
    e->CaretPos = -1;
    e->GetSelect = NULL;
    e->GetPath = yb_get_path;
    e->GetConfig = yb_get_config;
    e->GetKey = yb_get_key;
    e->OpenFile = (void *)yb_open_file;
    e->SendString = yb_send_string;
    e->Beep = NULL;
    e->QueryHistory = yb_query_history;
    e->GetLast = yb_get_last;
    e->ShowTip = yb_show_tip;
    e->Translate = yb_translate;
    e->Log = yb_log;
    e->Request = NULL;
    e->Callback = yb_callback;
}

/* ---------------------------------------------------------- IM 列表扫描 */

static void yb_scan_ims(void) {
    char key[16];
    g_im_count = 0;
    for(int i = 0; i < YB_IM_MAX; i++) {
        const char *section;
        char *engine;
        struct yb_im *im;

        snprintf(key, sizeof key, "%d", i);
        section = l_key_file_get_data(g_main_cfg, "IM", key);
        if(!section) break;
        // 只收引擎是我们编进来的 mb 方案的(libgbk.so 那种内码方案没编)
        engine = l_key_file_get_string(g_main_cfg, section, "engine");
        if(!engine) continue;
        int usable = !strcmp(engine, "libmb.so");
        l_free(engine);
        if(!usable) continue;

        im = &g_ims[g_im_count++];
        im->index = i;
        snprintf(im->section, sizeof im->section, "%s", section);
        {
            char *v = l_key_file_get_string(g_main_cfg, section, "name");
            snprintf(im->name, sizeof im->name, "%s", v && v[0] ? v : section);
            l_free(v);
            v = l_key_file_get_string(g_main_cfg, section, "arg");
            snprintf(im->arg, sizeof im->arg, "%s", v ? v : "");
            l_free(v);
            v = l_key_file_get_string(g_main_cfg, section, "overlay");
            snprintf(im->overlay, sizeof im->overlay, "%s", v ? v : "");
            l_free(v);
        }
    }
}

static void yb_read_key_config(void) {
    char *tmp;
    tmp = l_key_file_get_string(g_main_cfg, "key", "select_n");
    if(tmp) {
        if(tmp[0]) snprintf(g_select_n, sizeof g_select_n, "%s", tmp);
        l_free(tmp);
    }
    tmp = l_key_file_get_string(g_main_cfg, "key", "commit");
    if(tmp) {
        int k = yb_str_to_key(tmp);
        if(k > 0) g_key_commit = k;
        l_free(tmp);
    }
    // 临时英文键([key] tEN,如拼音的 v):空码时按它进英文,由驱动侧代理给内置词库
    g_key_temp_english = YK_NONE;
    tmp = l_key_file_get_string(g_main_cfg, "key", "tEN");
    if(tmp) {
        int k = yb_str_to_key(tmp);
        if(k > 0) g_key_temp_english = k >= 'A' && k <= 'Z' ? k - 'A' + 'a' : k;
        l_free(tmp);
    }
    tmp = l_key_file_get_string(g_main_cfg, "key", "page");
    if(tmp) {
        char **list = l_strsplit(tmp, ' ');
        if(list && list[0] && list[0][0]) g_key_pageup = (unsigned char)list[0][0];
        if(list && list[0] && list[1] && list[1][0]) g_key_pagedown = (unsigned char)list[1][0];
        l_strfreev(list);
        l_free(tmp);
    }
    // 拆字键([key] bihua,设备上拼音的 overlay 里配成 u):空码时按它进拆字。
    // 没配是 yong 的默认键 '`';配成 NONE 就是关掉;>=0x80 的当没配(和 main.c 一致)。
    g_key_bihua = '`';
    tmp = l_key_file_get_string(g_main_cfg, "key", "bihua");
    if(tmp) {
        if(tmp[0]) {
            int k = yb_str_to_key(tmp);
            if(k >= 0) g_key_bihua = k;
        }
        l_free(tmp);
    }
    if(g_key_bihua >= 0x80) g_key_bihua = YK_NONE;
}

/* -------------------------------------------------------------- 对外 API */

int yb_begin(void) {
    int def;

    if(g_ready) return 1;
    yb_resolve_paths();

    g_main_cfg = l_key_file_open("yong.ini", 0, g_home, g_data, NULL);
    if(!g_main_cfg) {
        fprintf(stderr, "yong: 找不到 yong.ini(%s 或 %s)\n", g_home, g_data);
        return 0;
    }
    yb_scan_ims();
    if(g_im_count == 0) {
        fprintf(stderr, "yong: [IM] 里没有可用的 libmb 方案\n");
        return 0;
    }
    yb_hook(&EIM);

    def = l_key_file_get_int(g_main_cfg, "IM", "default");
    g_im_pos = 0;
    for(int i = 0; i < g_im_count; i++) {
        if(g_ims[i].index == def) {
            g_im_pos = i;
            break;
        }
    }
    if(!yb_load_im(g_im_pos)) {
        fprintf(stderr, "yong: %s 初始化失败\n", g_ims[g_im_pos].section);
        return 0;
    }
    g_ready = 1;
    return 1;
}

void yb_end(void) {
    if(!g_ready) return;
    if(g_loaded) eim->Destroy();
    g_loaded = 0;
    g_ready = 0;
}

int yb_ready(void) { return g_ready; }

int yb_load_im(int pos) {
    if(pos < 0 || pos >= g_im_count) return 0;
    g_active = eim;   // 换方案先退出拆字
    if(g_loaded) {
        eim->Destroy();
        g_loaded = 0;
    }
    // overlay 要挂在 Init 之前:Engine 在 TableInitReal 里就把 [key]/[IM]/[<im>] 全读完了
    if(g_sub_cfg) {
        l_key_file_free(g_sub_cfg);
        g_sub_cfg = NULL;
    }
    if(g_ims[pos].overlay[0])
        g_sub_cfg = l_key_file_open(g_ims[pos].overlay, 0, g_home, g_data, NULL);
    l_key_file_set_overlay(g_main_cfg, g_sub_cfg);
    yb_read_key_config();   // overlay 会覆盖 [key],必须在 set_overlay 之后读

    g_im_pos = pos;
    g_code_input[0] = 0;
    g_string_get[0] = 0;
    g_pending_len = 0;
    g_pending[0] = 0;
    memset(g_cand_table, 0, sizeof g_cand_table);
    memset(g_code_tips, 0, sizeof g_code_tips);
    eim->Flag = 0;
    eim->WorkMode = EIM_WM_NORMAL;

    if(eim->Init(g_ims[pos].arg) != 0) return 0;
    eim->Reset();
    g_loaded = 1;
    return 1;
}

int yb_im_count(void) { return g_im_count; }

const char *yb_im_name(int i) {
    if(i < 0 || i >= g_im_count) return "";
    return g_ims[i].name;
}

int yb_cur_im(void) { return g_im_pos; }

void yb_reset(void) {
    // 拆字也在这里退出(照 yong 的 YongResetIM_:reset 一并把 BihuaMode 清掉)
    yb_bihua_leave();
    if(g_loaded) eim->Reset();
}

int yb_do_input(int key) {
    if(!g_loaded) return IMR_NEXT;
    return g_active->DoInput(key);
}

int yb_get_cand_words(int mode) {
    if(!g_loaded) return IMR_NEXT;
    return g_active->GetCandWords(mode);
}

int yb_get_cand_word(int index) {
    if(!g_loaded) return 0;
    return g_active->GetCandWord(index) != NULL;
}

int yb_code_len(void) { return g_loaded ? g_active->CodeLen : 0; }
int yb_cand_count(void) { return g_loaded ? g_active->CandWordCount : 0; }
int yb_cand_page_count(void) { return g_loaded ? g_active->CandPageCount : 0; }
int yb_cur_cand_page(void) { return g_loaded ? g_active->CurCandPage : 0; }
int yb_select_index(void) { return g_loaded ? g_active->SelectIndex : 0; }
void yb_set_select_index(int i) {
    if(g_loaded) g_active->SelectIndex = i;
}

int yb_cand_word_max(void) { return g_loaded ? g_active->CandWordMax : 0; }

// 引擎按 CandWordMax 分页(TableGetCandWords 里 max 就是它),驱动侧每页只画得下几个
// 就得在这儿同步过去,否则一页里放不下的那几个候选既看不见、又还能被数字键选中。
// 引擎在 DoInput 里重建候选表,所以下一次喂键之前设好就够。
void yb_set_cand_word_max(int n) {
    if(!g_loaded) return;
    if(n < 1) n = 1;
    if(n > YB_CAND_PER_PAGE) n = YB_CAND_PER_PAGE;
    g_active->CandWordMax = n;
    g_active->CandWordMaxReal = n;
}

int yb_key_pageup(void) { return g_key_pageup; }
int yb_key_pagedown(void) { return g_key_pagedown; }
int yb_is_commit_key(int key) { return key == g_key_commit; }
int yb_key_temp_english(void) { return g_key_temp_english; }

// 驱动侧选字:位置 0 起(和 y_im_check_select 一致)
int yb_select_pos(int key) { return yb_check_select(key, 0xe); }

// 引擎把 StringGet 当「另一个上屏串」用(IMR_COMMIT_DISPLAY / IMR_PUNC 前后),
// 驱动侧读走后自己清,免得下一轮重复拿到同一串。
void yb_clear_string_get(void) { g_string_get[0] = 0; }

/* ------------------------------------------------------------- 文本输出 */

int yb_code_input_utf8(char *out, int size) {
    if(out && size > 0) out[0] = 0;
    if(!g_loaded || !g_code_input[0] || !out || size <= 0) return 0;
    l_gb_to_utf8(g_code_input, out, size);
    return (int)strlen(out);
}

int yb_string_get_utf8(char *out, int size, int trad) {
    const char *src = g_string_get;
    if(out && size > 0) out[0] = 0;
    if(!g_loaded || !g_string_get[0] || !out || size <= 0) return 0;
    if(trad) src = yong_s2t(g_string_get);
    l_gb_to_utf8(src, out, size);
    return (int)strlen(out);
}

int yb_cand_utf8(int i, char *out, int size, int trad) {
    const char *src;
    if(out && size > 0) out[0] = 0;
    if(!g_loaded || i < 0 || i >= YB_CAND_MAX || !out || size <= 0) return 0;
    src = g_cand_table[i];
    if(!src[0]) return 0;
    if(trad) src = yong_s2t(src);
    l_gb_to_utf8(src, out, size);
    return (int)strlen(out);
}

int yb_take_output(char *out, int size, int trad) {
    const char *src = g_pending;
    if(out && size > 0) out[0] = 0;
    if(g_pending_len == 0) return 0;
    if(trad) src = yong_s2t(g_pending);
    l_gb_to_utf8(src, out, size);
    g_pending_len = 0;
    g_pending[0] = 0;
    return (int)strlen(out);
}

int yb_pending_len(void) { return g_pending_len; }

/* ------------------------------------------------------------ 拆字(笔画) */

// [key] bihua(设备上拼音 overlay 里是 u),没配返回 YK_NONE
int yb_bihua_key(void) { return g_key_bihua; }

int yb_bihua_active(void) { return g_bihua != NULL && g_active == g_bihua; }

// 进拆字模式。引擎第一次用到才 Init —— 没碰拆字就不去读那份 bihua.bin。
int yb_bihua_enter(void) {
    if(!g_loaded) return 0;
    if(!g_bihua) {
        g_bihua = (EXTRA_IM *)y_bihua_eim();
        if(!g_bihua) return 0;
        yb_hook(g_bihua);
    }
    if(!g_bihua_inited) {
        g_bihua->Flag = 0;
        g_bihua->WorkMode = EIM_WM_NORMAL;
        // 拆字用的笔画键表挂在「父」引擎上,照 common/main.c 的 InitExtraIM 传进去
        yb_compat_set_im_eim(eim);
        if(g_bihua->Init((const char *)eim->Bihua) != 0) return 0;
        g_bihua_inited = 1;
    }
    if(!y_bihua_good()) return 0;
    g_bihua->Reset();
    g_active = g_bihua;
    return 1;
}

void yb_bihua_leave(void) {
    if(!g_bihua || g_active != g_bihua) return;
    g_bihua->Reset();
    g_active = eim;
}

/* ---------------------------- 拆字兼容层(src/yong_bihua_compat.c)要的驱动侧查询 */

const char *yb_compat_path(const char *type) { return yb_get_path(type); }

char *yb_compat_config_string(const char *group, const char *key) {
    if(!g_main_cfg || !group || !key) return NULL;
    return l_key_file_get_string(g_main_cfg, group, key);
}

int yb_compat_key(const char *s) { return yb_str_to_key(s); }
