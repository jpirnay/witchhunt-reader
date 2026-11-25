#pragma once

// Host stand-in for ESP-IDF's hardware RNG: random enough for the cache cipher's salts and nonces in a test, and not
// OpenSSL's, which this target does not link.

#include <cstddef>
#include <cstdint>
#include <random>

inline void esp_fill_random(void* buf, const size_t len) {
  static std::mt19937 engine{std::random_device{}()};
  auto* out = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(engine());
}
