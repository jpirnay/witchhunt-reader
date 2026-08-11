#pragma once
// Host stub for <esp_heap_caps.h>. Returns a large constant so every heap gate
// in the pipeline passes deterministically — control flow must not depend on
// the host machine's actual memory state.
#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_8BIT (1 << 0)
#define MALLOC_CAP_DEFAULT (1 << 1)
#define MALLOC_CAP_INTERNAL (1 << 2)

inline size_t heap_caps_get_largest_free_block(uint32_t /*caps*/) { return 200 * 1024; }
inline size_t heap_caps_get_free_size(uint32_t /*caps*/) { return 300 * 1024; }

// Block-count probe. Constant like the rest of this stub: it feeds diagnostic logging only,
// never a decision, so the host must not vary on it. Field names match the ESP-IDF struct so
// the device and host compile the same call sites.
struct multi_heap_info_t {
  size_t total_free_bytes;
  size_t total_allocated_bytes;
  size_t largest_free_block;
  size_t minimum_free_bytes;
  size_t allocated_blocks;
  size_t free_blocks;
  size_t total_blocks;
};

inline void heap_caps_get_info(multi_heap_info_t* info, uint32_t /*caps*/) {
  if (info) *info = multi_heap_info_t{};
}
