#pragma once

#include "flomo_model.h"

#include <string>

// 本地库放哪儿:~/.pjournal-lvgl/flomo/memos.json。和 pjournal 的设置目录同源,
// 但单独一个子目录,笔记数据不跟配置混在一起。
std::string flomo_db_dir();

// memos.json 的读写。旧版 ~/.flomo-typix/memos.json 会自动迁移过来。
bool flomo_db_load(Store &store);
bool flomo_db_save(const Store &store);
