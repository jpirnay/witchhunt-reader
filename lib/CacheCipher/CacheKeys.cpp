#include "CacheKeys.h"

#include <cstring>

#include "CacheCipher.h"

namespace {

// FNV-1a, the 64-bit and 32-bit parameters. Two independent walks give the 96 bits the nonce
// needs; see CacheKeys.h for why a non-cryptographic hash is the right tool here.
constexpr uint64_t kFnv64Offset = 1469598103934665603ull;
constexpr uint64_t kFnv64Prime = 1099511628211ull;
constexpr uint32_t kFnv32Offset = 2166136261u;
constexpr uint32_t kFnv32Prime = 16777619u;

uint64_t fnv1a64(const char* s, size_t len) {
  uint64_t h = kFnv64Offset;
  for (size_t i = 0; i < len; i++) {
    h ^= static_cast<uint8_t>(s[i]);
    h *= kFnv64Prime;
  }
  return h;
}

// Walked backwards and finished with the length, so it is not a truncation of the 64-bit walk
// above: two names that collided in one would have to collide independently in the other.
uint32_t fnv1a32Reverse(const char* s, size_t len) {
  uint32_t h = kFnv32Offset;
  for (size_t i = len; i > 0; i--) {
    h ^= static_cast<uint8_t>(s[i - 1]);
    h *= kFnv32Prime;
  }
  h ^= static_cast<uint32_t>(len);
  h *= kFnv32Prime;
  return h;
}

void store64le(uint8_t* p, const uint64_t v) {
  for (size_t i = 0; i < 8; i++) {
    p[i] = static_cast<uint8_t>(v >> (i * 8));
  }
}

void store32le(uint8_t* p, const uint32_t v) {
  for (size_t i = 0; i < 4; i++) {
    p[i] = static_cast<uint8_t>(v >> (i * 8));
  }
}

}  // namespace

namespace cachekeys {

void deriveBookKey(const uint8_t deviceKey[kDeviceKeyBytes], const uint8_t salt[kSaltBytes],
                   uint8_t out[kBookKeyBytes]) {
  // Enciphering zeroes yields raw keystream, so this is ChaCha20 used as a PRF.
  memset(out, 0, kBookKeyBytes);
  const CacheCipher prf(deviceKey, salt);
  if (!prf.apply(out, kBookKeyBytes, 0)) {
    // Only reachable on a null buffer, which cannot happen here; zeroing rather than leaving
    // the caller a half-derived key keeps a failure obvious instead of subtle.
    memset(out, 0, kBookKeyBytes);
  }
}

void deriveFileNonce(const char* relativeName, uint8_t out[kNonceBytes]) {
  const size_t len = relativeName != nullptr ? strlen(relativeName) : 0;
  store64le(out, fnv1a64(relativeName, len));
  store32le(out + 8, fnv1a32Reverse(relativeName, len));
}

}  // namespace cachekeys
