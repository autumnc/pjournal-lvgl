// common/bihua.c 的驱动侧依赖。yong 里这些由 common/common.c 和 main.c 提供,但那些文件
// 拖着整套 xim/glib,编不进来(见 src/yong_s2t.c 和 src/yong_bridge.c 的说明)。这里只补
// bihua.c 真正用到的几个,数值和字符串一律转发回 src/yong_bridge.c。
//
// 编译这个文件要带上 -DCFG_NO_GLIB(common.h 靠它绕开 glib.h)。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h" // IM / EXTRA_IM / y_im_* 的声明

extern const char *yb_compat_path(const char *type);
extern char *yb_compat_config_string(const char *group, const char *key);
extern int yb_compat_key(const char *s);

// common/im.h: extern IM im;(yong 里定义在 main.c)
IM im;

const char *y_im_get_path(const char *type) { return yb_compat_path(type); }

char *y_im_get_config_string(const char *group, const char *key) {
    return yb_compat_config_string(group, key);
}

int y_im_get_config_int(const char *group, const char *key) {
    char *s = yb_compat_config_string(group, key);
    int v = s ? atoi(s) : 0;
    l_free(s);
    return v;
}

// common/common.c:y_im_get_key 的裁剪版(bihua 只用到 pos==-1,pos 那支顺带留着)
int y_im_get_key(const char *name, int pos, int def) {
    char *tmp = yb_compat_config_string("key", name);
    int ret = -1;

    if(!tmp) return def;
    if(!tmp[0]) {
        l_free(tmp);
        return def;
    }
    if(pos == -1) {
        ret = yb_compat_key(tmp);
    } else {
        char **list = l_strsplit(tmp, ' ');
        if(list) {
            int i;
            for(i = 0; i < pos && list[i]; i++) {}
            if(i == pos && list[i]) ret = yb_compat_key(list[i]);
            l_strfreev(list);
        }
    }
    if(ret < 0) ret = def;
    l_free(tmp);
    return ret;
}

// 设备上没有标点表(bd.txt),yong 那边 YongGetPunc 也会退化成源码里几个默认值;
// 我们这边的标点统一交给驱动侧的内置转换,所以这里就是「查不到」。
const char *YongGetPunc(int key, int bd, int peek) {
    (void)key;
    (void)bd;
    (void)peek;
    return NULL;
}

// 拆字引擎的 BihuaDoInput 会问 im.eim->Bihua(父引擎的笔画键表)
void yb_compat_set_im_eim(void *e) { im.eim = (EXTRA_IM *)e; }
