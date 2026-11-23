#pragma once

// Host stand-in for ESP-IDF's address-range checks: no buffer counts as RAM, so this target's writes into an
// enciphered folder take the copying path; the in-place one is exercised by test/cache_cipher and the pipeline shim.

inline bool esp_ptr_in_dram(const void*) { return false; }
inline bool esp_ptr_external_ram(const void*) { return false; }
