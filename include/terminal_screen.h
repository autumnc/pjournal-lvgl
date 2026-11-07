#pragma once

#include <string>

// 终端屏。app_ui 只负责转发按键/文字和切换,所有 LVGL 对象都在这里建和销毁。
//
// 终端自己画整屏(term_ui 的自绘画布),不参与 app_ui 的 render/clear_root 流程 ——
// 所以 render() 是 no-op,进屏时 create() 清 root,离屏时 leave() 再清一次。
namespace terminal {

// 进入终端屏:读设置、载字体、建画布、起 shell。重复调用无副作用(先 leave)。
void create();

// no-op:画布靠 VT 的脏行重绘,不整屏重建。
void render();

// 键盘按键(AppKey 或 0x01-0x7f 字节)。本屏吞掉所有键。
void handle_key(int key);

// 主循环 tick:读 pty、喂 VT、刷新候选条/角标。
void tick();

// 输入法上屏文字:直接写进 pty。
void insert_utf8(const std::string &text);

// 离开终端屏:关输入法、关闭 shell 会话、释放对象。可重复调用。
void leave();

}  // namespace terminal
