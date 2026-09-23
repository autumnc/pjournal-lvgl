#pragma once

#include <string>

// dm250 的背光是 /sys/class/backlight/rk28_bl,type=raw:brightness 写的就是 PWM
// 占空比本身,0..max_brightness(254)。写 0 会让背光彻底灭掉,屏幕上什么都看不见,
// 而这台机器又没有独立的亮度键可以摸回来,所以下面所有接口都把值夹到 >=1。
//
// 机器上没有背光设备时(x86 上跑、或者别的机型),这些接口都安全地退化成
// false / 0 / no-op,调用方不用额外判断。
std::string backlight_device();  // 设备目录,没有则空串
bool backlight_available();
int backlight_max();             // max_brightness,不可用时 0
bool backlight_set_percent(int percent);  // 1..100,夹到 [1,max] 后写入
int backlight_current_percent();          // 当前亮度折成百分比,不可用时 0
