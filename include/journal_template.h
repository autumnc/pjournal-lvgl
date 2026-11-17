#pragma once

#include <ctime>
#include <string>
#include <vector>

// 日记模板:~/.pjournal 之外的 ~/Templates 下的 Markdown 文件。
// 模板块里的 {{date}} / {{weekday}} 在套用时按当前时间展开;
// {{weather}} 由天气功能填入。
std::string journal_template_dir();

// 模板文件名列表(不含路径,按文件名排序)。
std::vector<std::string> journal_template_list();

// "晨间日记.md" -> "晨间日记";没有扩展名时原样返回。
std::string journal_template_display_name(const std::string &filename);

// 设置里保存的当前模板文件名(可能为空或已失效)。
std::string journal_template_selected();
void journal_template_select(const std::string &filename);

// 模板选择「无」时关闭模板日记:主界面入口与天气地区设置都会隐藏。
bool journal_template_enabled();
void journal_template_select_none();

// 展开 {{date}} / {{weekday}}(兼容 {{ date }} / {{ weekday }} 写法)。
std::string journal_template_expand(const std::string &text, time_t now);

// 按文件名读取模板并展开占位符。文件名必须是 Templates 目录下的裸文件名。
bool journal_template_load(const std::string &filename, std::string &out);

// 读取当前选中的模板;未选择时取列表中的第一个。失败时 error 为提示文本。
bool journal_template_load_selected(std::string &out, std::string &error);

// 首次启动(Templates 目录不存在)时创建该目录,并写入固件内置的默认模板。
void journal_template_ensure_defaults();
