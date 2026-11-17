#pragma once

// Host stand-in for ESP-IDF's hardware RNG: OpenSSL's.

#include <openssl/rand.h>

#include <cstddef>

inline void esp_fill_random(void* buf, const size_t len) {
  RAND_bytes(static_cast<unsigned char*>(buf), static_cast<int>(len));
}
