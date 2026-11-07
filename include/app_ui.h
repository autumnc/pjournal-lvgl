#pragma once

void app_ui_create();
void app_ui_reload_theme();
void app_ui_reload_font();
bool app_ui_should_quit();
void app_ui_tick();

// 退出主循环让 main 重新 exec 自己。输入法(尤其 fcitx5)、字体、主题这些只在
// 启动时读一次的设置靠它生效。
void app_ui_request_reload();
bool app_ui_reload_requested();

// 供浮墨屏等子模块返回主面板。内部走 goto_screen(Main),会触发子屏的收尾逻辑。
void app_ui_return_to_main();
