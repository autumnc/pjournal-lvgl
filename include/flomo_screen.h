#pragma once

#include "flomo_model.h"

#include <string>

// 浮墨屏。app_ui 只负责把按键/文字转发进来,所有 LVGL 对象都在这里创建和销毁。
//
// 界面尺寸的基准字号是 34(dm250 1024×600),纵向写死的像素一律过 ui_px() 换算。
namespace flomo {

// 进入浮墨屏:加载本地库、建根对象、清屏。重复调用无副作用。
void create();

// 每帧重绘当前视图。
void render();

// 键盘按键(LVGL 的 LV_KEY_* 或 ASCII)。本屏吞掉所有键,包括背光/输入法切换。
void handle_key(int key);

// 主循环 tick:取后台同步/今日回顾结果,落盘,重绘。
void tick();

// 输入法上屏文字。
void insert_utf8(const std::string &text);

// 离开浮墨屏:关输入法、保存本地库、释放对象。
void leave();

}  // namespace flomo
