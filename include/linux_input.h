#pragma once

#include <string>

bool linux_input_start(const std::string &device);
bool linux_input_start_console_modifiers();
bool linux_input_start_console();
