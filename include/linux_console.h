#pragma once

#include <string>

bool linux_console_enter_graphics();
void linux_console_restore();
std::string linux_console_active_tty();
