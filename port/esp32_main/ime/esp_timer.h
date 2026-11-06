#pragma once

// ESP-IDF esp_timer 的最小替身: IME 只用 esp_timer_get_time() 取单调微秒时钟。
#include <ctime>
#include <cstdint>

static inline int64_t esp_timer_get_time() {
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}
