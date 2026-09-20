// yong 的简繁转换表薄包装。
//
// yong 自己的 common/s2t.c 编不进来(它 include common.h → glib.h,还调 xim/select
// 那套驱动函数),但表本身是纯数据:s2t_char.c 是单字表,s2t_phrase.c 是词语表。
// 两边存的都是 GBK 码对(不是 Unicode —— l_gb_to_char() 对 GBK 双字节返回的就是
// read_u16be,所以引擎侧和表侧一致),按第一列升序,bsearch 查。
//
// 这里只做 简→繁(s2t 方向)。繁→简(t2s)用不上,开关关了就是原样输出。
//
// 下面那个转换循环是照 common/s2t.c 的 s2t_conv() 抄的,去掉驱动侧分支
// (s2t_get_enable / im.SelectMode / s2t_biaodian),s2t_open 固定为「开」,
// l_read_u8(p) 换成 p[0]。

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lgb.h"
#include "lfuncs.h"

#include "s2t_char.c"
#include "s2t_phrase.c"

// 词语表的二分查找。格式(见 common/s2t.c 的注释):
//   data + s2t_phrase[h] 指向一条记录,记录第一个字节高 4 位是「改第几个字节」,
//   低 4 位是长度;后面是原文,再后面是替换内容。
static int replace_phrase(char *in, int len)
{
	if (len > 8)
		return 0;
	const char *p = NULL;
	{
		int b = 0, e = S2T_PHRASE_NUM;
		while (b < e)
		{
			int h = b + (e - b) / 2;
			const char *t = s2t_data + s2t_phrase[h];
			int r;
			int l2 = t[0] & 0xf;
			if (len < l2)
				r = -1;
			else if (len > l2)
				r = 1;
			else
				r = memcmp(in, t + 1, len);
			if (r > 0)
				b = h + 1;
			else if (r < 0)
				e = h;
			else
			{
				p = t;
				break;
			}
		}
	}
	if (!p)
		return 0;
	uint8_t offset = (uint8_t)p[0] >> 4;
	p += 1 + len;
	if (offset)
	{
		offset = (uint8_t)((offset - 1) * 2);
		in[offset] = p[0];
		in[offset + 1] = p[1];
	}
	else
	{
		memcpy(in, p, (size_t)len);
	}
	return 1;
}

static int adjust_phrase(char *in, int len)
{
	if (len <= 3)
		return 0;
	if (replace_phrase(in, len))
		return 1;
	if (len > 4)
	{
		// 先试前 4 字节(一个词),剩下的递归;不中再退到前 2 字节(一个字)
		if (adjust_phrase(in, 4))
			adjust_phrase(in + 4, len - 4);
		else
			adjust_phrase(in + 2, len - 2);
	}
	return 0;
}

// 简→繁。gbk 串进,gbk 静态缓冲出。没有变化时返回原指针。
const char *yong_s2t(const char *gbk)
{
	static char out[4096];
	const char *s = gbk;
	int pos = 0;

	if (!gbk || !gbk[0])
		return gbk;

	for (;;)
	{
		uint32_t code = l_gb_to_char(s);
		s = (const char *)l_gb_next_char(s);
		if (s == NULL)
			break;
		if (pos + 4 >= (int)sizeof(out))
			break;
		if (code < 0x10000)
		{
			const uint16_t *res = bsearch(&code, s2t, s2t_num, 4, l_uint16_equal);
			if (res)
				code = res[1];
			else
			{
				const uint32_t *r32 = bsearch(&code, s2te, s2te_num, 8, l_uint32_equal);
				if (r32)
					code = r32[1];
			}
		}
		else
		{
			const uint32_t *r32 = bsearch(&code, s2te, s2te_num, 8, l_uint32_equal);
			if (r32)
				code = r32[1];
		}
		pos += l_char_to_gb(code, out + pos);
	}
	out[pos] = 0;
	adjust_phrase(out, pos);

	if (!strcmp(out, gbk))
		return gbk;
	return out;
}
