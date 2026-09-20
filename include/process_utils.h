#pragma once

#include <string>
#include <vector>

namespace process {

std::string run_capture(const std::vector<std::string> &args);
bool command_exists(const char *name);

}
