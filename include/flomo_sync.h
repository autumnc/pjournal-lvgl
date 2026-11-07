#pragma once

#include "flomo_model.h"

#include <string>
#include <vector>

// 后台同步:先把本地 dirty 的改动推上去,再按游标把远端增量拉下来。
//
// 这件事必须离开主线程 —— pjournal 的 webdav_sync 是主线程阻塞式的(70s 超时),
// 在 Flomo 这里「编辑到一半卡住不动」是不可接受的,所以整段跑在 std::thread 上,
// 而且同步线程**绝不碰任何 LVGL 对象**。
//
// 结果整份 Store 通过 mutex 交回主线程,由 flomo_ui_tick() 取走落盘 + 重绘。
// 同一时刻只允许一个同步在跑。
bool sync_start(const Store &snapshot);   // false = 已经有一个在跑了
bool sync_busy();

// 取结果。有新结果返回 true 并填好 out_store / out_status(状态栏那句话);
// 没有新结果返回 false,不动 out_*。
bool sync_take(Store &out_store, std::string &out_status);

// 今日回顾:后台取 memo/notify_of_today/,再逐条拉正文(404 跳过,最多 20 条)。
bool review_start(const std::string &token);
bool review_busy();
bool review_take(std::vector<Memo> &out_memos, std::string &out_status);
