#pragma once
// ESP-IDF's capability allocator, for the test of how FlashArea picks its backend: PSRAM from the host heap, or none
// when a test says the chip has none. Counts what was asked of the PSRAM.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#define MALLOC_CAP_8BIT (1 << 0)
#define MALLOC_CAP_SPIRAM (1 << 10)

inline bool g_hostHasPsram = true;
inline int g_psramAllocs = 0;

inline void* heap_caps_malloc(const size_t size, const uint32_t caps) {
  if (caps & MALLOC_CAP_SPIRAM) {
    ++g_psramAllocs;
    if (!g_hostHasPsram) return nullptr;
  }
  return std::malloc(size);
}
inline void heap_caps_free(void* ptr) { std::free(ptr); }
inline size_t heap_caps_get_free_size(uint32_t) { return 0; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 0; }
