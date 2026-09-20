#pragma once

// src/yong_bridge.c 对外的 C 接口。yong 引擎(mb/face.c 里那个 EXTRA_IM EIM)和它拖着的
// llib 一起编在 pjournal_yong 里,这一层是唯一能碰它们的地方 —— llib.h 用了 C23 的
// defer/auto 和 <stdatomic.h>,没有 extern "C",C++ 侧编不进来。
//
// 编码约定:.c 里自己处理 GBK,这里对外的 char* 一律 UTF-8。
// 路径:设备上是 PJOURNAL_YONG_DATA(默认 /root/yong),可用同名环境变量顶掉(HOME 底下
// 那份用户配置照 XDG 规则找)。

#ifdef __cplusplus
extern "C" {
#endif

// 引擎就绪(找到 yong.ini、扫到 libmb 方案、Init 成功)。不可用时下面全部安全退化。
int yb_begin(void);
void yb_end(void);
int yb_ready(void);

// [IM] 里 engine=libmb.so 的方案列表(设备上是 永码/五笔/两分/二笔/拼音,跳过内码的 gbk)
int yb_im_count(void);
const char *yb_im_name(int i);   // UTF-8,拿不到返回 ""
int yb_cur_im(void);             // 当前方案在列表里的下标
int yb_load_im(int pos);         // 切方案(内部 Destroy + 重新 Init)

void yb_reset(void);

// 按键喂给引擎,返回 IMR_*(见 yong.h)。
int yb_do_input(int key);
// 翻页,mode 是 PAGE_FIRST / PAGE_NEXT / PAGE_PREV,返回 IMR_*。
int yb_get_cand_words(int mode);
// 取第 index 个候选(0 起)当上屏串,非 0 = 成功;之后要 yb_string_get_utf8 取走。
int yb_get_cand_word(int index);

int yb_code_len(void);           // 编码长度(字节数,GBK)
int yb_cand_count(void);         // 本页候选个数
int yb_cand_page_count(void);    // 总页数
int yb_cur_cand_page(void);      // 当前页,0 起
int yb_select_index(void);       // 引擎记的高亮位置
void yb_set_select_index(int i);

// 每页候选个数:引擎用它分页,驱动侧每页画得下几个就设几个,免得一页里放不下的候选
// 看不见却还能被数字键选中。数值夹在 1..9。
int yb_cand_word_max(void);
void yb_set_cand_word_max(int n);

// [key] 配置里驱动侧要用的那几个([key] page / commit / select_n / tEN)
int yb_key_pageup(void);
int yb_key_pagedown(void);
int yb_is_commit_key(int key);
// 临时英文键([key] tEN,如拼音的 v);没配返回 0(YK_NONE)
int yb_key_temp_english(void);
// 数字选字:'1'..'9' 对应的位置(0 起),不是选字键返回 -1
int yb_select_pos(int key);

// 拆字(笔画)辅助引擎:空码时按 [key] bihua(设备上拼音是 u)进去,之后所有键都归它,
// 候选/编码/上屏照旧从上面那些 yb_* 取(桥接内部把「当前引擎」切过去了)。
// yb_reset 会顺带退出来,所以驱动侧的 reset_engine 不用额外处理。
int yb_bihua_key(void);      // 没配返回 YK_NONE(YK_NONE 是 0)
int yb_bihua_enter(void);    // 引擎不可用(bihua.bin 没读到)返回 0,这时别进
void yb_bihua_leave(void);
int yb_bihua_active(void);

// EIM.StringGet 是引擎的「另一个上屏串」缓冲,读走后驱动侧要自己清
void yb_clear_string_get(void);

int yb_code_input_utf8(char *out, int size);
int yb_string_get_utf8(char *out, int size, int trad);
int yb_cand_utf8(int i, char *out, int size, int trad);
// 取走 SendString 攒下来的上屏串(候选是 $SPACE 之类命令时引擎会先发字再发命令),
// 返回写进 out 的字节数,取走后清空。
int yb_take_output(char *out, int size, int trad);
int yb_pending_len(void);

#ifdef __cplusplus
}
#endif
