#pragma once

void app_ui_create();
void app_ui_reload_theme();
void app_ui_reload_font();
bool app_ui_should_quit();
void app_ui_tick();

// 供浮墨屏等子模块返回主面板。内部走 goto_screen(Main),会触发子屏的收尾逻辑。
void app_ui_return_to_main();
