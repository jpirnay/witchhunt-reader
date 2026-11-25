#include "CacheCipher.h"

#include <cstring>

// Two backends, one behaviour. On device wolfSSL's ChaCha20 does the work: it is already
// linked for TLS, so it costs no flash, and it measured 4.6 MB/s against 1.8 MB/s for the
// portable core below on an ESP32-C3 (docs/design/cache-cipher.md, D2). At 1.8 MB/s a 4 KB
// page record costs 2.25 ms and a 48 KB image 26 ms, both over D2's budgets; at
// 4.6 MB/s they are 0.9 ms and 10 ms, both inside. The gap is the byte-at-a-time XOR here
// against wolfSSL's word-wise one — worth knowing before anyone "simplifies" this back to one
// implementation.
//
// The portable core stays for the host tests, which is where the RFC 8439 vectors are checked
// (wolfSSL is not built by the host toolchain). The two are pinned to the same vectors:
// bench_crypto checks the device path against the same RFC keystream that test/cache_cipher
// checks the portable one against, so "both are really ChaCha20" is asserted on both sides
// rather than assumed.
#if defined(CACHE_CIPHER_WOLFSSL)
#include <wolfssl/wolfcrypt/chacha.h>
#if !defined(HAVE_CHACHA)
#error "CacheCipher needs HAVE_CHACHA; scripts/patch_wolfssl.py must keep ChaCha20 enabled."
#endif
#endif

#if !defined(CACHE_CIPHER_WOLFSSL)
namespace {

// "expand 32-byte k" — the RFC 8439 constant, as four little-endian words.
constexpr uint32_t kSigma[4] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};

inline uint32_t rotl32(const uint32_t v, const int c) { return (v << c) | (v >> (32 - c)); }

inline uint32_t load32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void store32le(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

inline void quarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
  a += b;
  d = rotl32(d ^ a, 16);
  c += d;
  b = rotl32(b ^ c, 12);
  a += b;
  d = rotl32(d ^ a, 8);
  c += d;
  b = rotl32(b ^ c, 7);
}

}  // namespace
#endif  // !CACHE_CIPHER_WOLFSSL

CacheCipher::CacheCipher(const uint8_t key[kKeyBytes], const uint8_t nonce[kNonceBytes]) {
#if defined(CACHE_CIPHER_WOLFSSL)
  memcpy(key_, key, kKeyBytes);
  memcpy(nonce_, nonce, kNonceBytes);
#else
  state_[0] = kSigma[0];
  state_[1] = kSigma[1];
  state_[2] = kSigma[2];
  state_[3] = kSigma[3];
  for (size_t i = 0; i < 8; i++) {
    state_[4 + i] = load32le(key + i * 4);
  }
  state_[12] = 0;  // block counter, filled per block by keystreamBlock
  for (size_t i = 0; i < 3; i++) {
    state_[13 + i] = load32le(nonce + i * 4);
  }
#endif
}

#if !defined(CACHE_CIPHER_WOLFSSL)
void CacheCipher::keystreamBlock(const uint32_t counter, uint8_t out[kBlockBytes]) const {
  // 64 bytes of working state on the stack, per the allocation rules: bounded, short-lived,
  // and far under the stack budget.
  uint32_t working[16];
  memcpy(working, state_, sizeof(working));
  working[12] = counter;

  for (int i = 0; i < 10; i++) {  // 20 rounds = 10 column rounds + 10 diagonal rounds
    quarterRound(working[0], working[4], working[8], working[12]);
    quarterRound(working[1], working[5], working[9], working[13]);
    quarterRound(working[2], working[6], working[10], working[14]);
    quarterRound(working[3], working[7], working[11], working[15]);
    quarterRound(working[0], working[5], working[10], working[15]);
    quarterRound(working[1], working[6], working[11], working[12]);
    quarterRound(working[2], working[7], working[8], working[13]);
    quarterRound(working[3], working[4], working[9], working[14]);
  }

  for (size_t i = 0; i < 16; i++) {
    const uint32_t word = working[i] + (i == 12 ? counter : state_[i]);
    store32le(out + i * 4, word);
  }
}
#endif  // !CACHE_CIPHER_WOLFSSL

bool CacheCipher::apply(uint8_t* buf, const size_t len, const uint64_t offset) const {
  if (buf == nullptr) {
    return false;
  }
  if (len == 0) {
    return true;  // nothing asked for is not a failure
  }
  // Outside the contract (see the header): a wrapped counter would repeat the keystream,
  // which is worse than refusing. No cache file can reach this on FAT32.
  if (offset > kMaxOffset || len - 1 > kMaxOffset - offset) {
    return false;
  }

  uint64_t blockIndex = offset / kBlockBytes;
  size_t within = static_cast<size_t>(offset % kBlockBytes);

#if defined(CACHE_CIPHER_WOLFSSL)
  ChaCha ctx;
  if (wc_Chacha_SetKey(&ctx, key_, static_cast<word32>(kKeyBytes)) != 0) {
    return false;
  }

  size_t done = 0;
  if (within != 0) {
    // wolfSSL has no "start part-way into a block" entry point, so the leading partial block
    // is done by hand: enciphering zeroes yields that block's raw keystream.
    uint8_t keystream[kBlockBytes] = {0};
    if (wc_Chacha_SetIV(&ctx, nonce_, static_cast<word32>(blockIndex)) != 0 ||
        wc_Chacha_Process(&ctx, keystream, keystream, static_cast<word32>(kBlockBytes)) != 0) {
      return false;
    }
    const size_t inBlock = kBlockBytes - within;
    const size_t take = len < inBlock ? len : inBlock;
    for (size_t i = 0; i < take; i++) {
      buf[i] ^= keystream[within + i];
    }
    done = take;
    blockIndex++;
  }

  if (done < len) {
    // The rest is block-aligned; wolfSSL handles a short final block itself.
    if (wc_Chacha_SetIV(&ctx, nonce_, static_cast<word32>(blockIndex)) != 0 ||
        wc_Chacha_Process(&ctx, buf + done, buf + done, static_cast<word32>(len - done)) != 0) {
      return false;
    }
  }
  return true;
#else
  uint8_t keystream[kBlockBytes];
  size_t done = 0;
  while (done < len) {
    keystreamBlock(static_cast<uint32_t>(blockIndex), keystream);
    const size_t remaining = len - done;
    const size_t inBlock = kBlockBytes - within;
    const size_t take = remaining < inBlock ? remaining : inBlock;
    for (size_t i = 0; i < take; i++) {
      buf[done + i] ^= keystream[within + i];
    }
    done += take;
    within = 0;
    blockIndex++;
  }
  return true;
#endif
}
