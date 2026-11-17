#pragma once

// Host stand-in for wolfCrypt's one-shot SHA-256, over OpenSSL.

#include <openssl/sha.h>

#include <cstdint>

inline int wc_Sha256Hash(const uint8_t* data, const uint32_t len, uint8_t* hash) {
  return SHA256(data, len, hash) != nullptr ? 0 : -1;
}
