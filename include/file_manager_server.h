#pragma once

#include <cstdint>

bool file_manager_server_start(uint16_t port = 8080);
void file_manager_server_stop();
bool file_manager_server_running();
uint16_t file_manager_server_get_port();
