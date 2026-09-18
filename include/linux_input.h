#pragma once

#include <string>

bool linux_input_start(const std::string &device);
bool linux_input_start_modifier_taps(const std::string &device);
bool linux_input_start_console();
