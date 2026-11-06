#pragma once

// IME.cpp 原样 include ESP32 的 pjournal_app.h, 只用到下面这些键码。
// 修饰键"单击"事件码必须与 include/key_codes.h 的 AppKey 一致(0xB0..0xB5)。

#define KEY_MOD_TAP_LCTRL 0xB0
#define KEY_MOD_TAP_LGUI 0xB1
#define KEY_MOD_TAP_LALT 0xB2
#define KEY_MOD_TAP_RALT 0xB3
#define KEY_MOD_TAP_RGUI 0xB4
#define KEY_MOD_TAP_RCTRL 0xB5

#ifndef KEY_PAGE_UP
#define KEY_PAGE_UP 0xA0
#endif
#ifndef KEY_PAGE_DOWN
#define KEY_PAGE_DOWN 0xA1
#endif
