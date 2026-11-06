#pragma once

// ESP-IDF heap_caps 的最小替身。IME 用 PsramAllocator 把用户词索引放到 PSRAM;
// Linux 上没有分区堆, 直接退化成 malloc/free, 能力位仅用于源码兼容。
#include <cstdlib>
#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_INTERNAL (1u << 0)
#define MALLOC_CAP_SPIRAM (1u << 1)
#define MALLOC_CAP_8BIT (1u << 2)

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return std::malloc(size);
}

static inline void heap_caps_free(void *p) { std::free(p); }
