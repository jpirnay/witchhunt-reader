#pragma once

// Cache-at-rest cipher for protected books — docs/design/cache-cipher.md (D2).
//
// A book read under LCP or Adobe DRM must not leave its plaintext on the SD card, but the
// reader's whole page-turn path is built on cached derivatives of that plaintext (the
// extracted XHTML, the laid-out pages, the converted images). This enciphers those files at
// rest so the card holds no readable copy, without changing a single byte offset in any of
// the formats that reference them.
//
// ChaCha20 (RFC 8439) as a pure keystream:
//
//   * **Length-preserving.** Ciphertext is exactly as long as plaintext, so every stored
//     offset — the page LUT, the patched section header, the anchor and page-break maps —
//     stays valid and no format changes.
//   * **Offset-addressable.** apply() derives the keystream position from the absolute file
//     offset, so a page turn deciphers only the page record it actually reads (a few KB)
//     rather than everything before it. A CBC-style mode would force a read from byte zero.
//   * **Cheap in software.** Our wolfSSL build has no hardware crypto (the Espressif port is
//     gated on WOLFSSL_ESPIDF, which never turns on under Arduino), and the accelerator would
//     not help here anyway: its single-block entry point takes a mutex and reloads the key per
//     16 bytes. See D2.
//
// Symmetric: apply() enciphers and deciphers. Freestanding C++17 — no Arduino, no allocation,
// no file knowledge — so it is exercised by test/cache_cipher against the RFC vectors.
//
// NOT the same thing as lib/Serialization/ObfuscationUtils, which XORs short credential
// strings with the 6-byte hardware MAC. That is casual-read protection for a handful of
// bytes; against kilobytes of known-structure book text it is not protection at all. Do not
// merge the two.
//
// **Contract the caller owns:** a (key, nonce) pair must never encipher two different
// plaintexts. Reusing one across two files, or across two generations of the same file, lets
// anyone holding both ciphertexts recover their XOR. Deriving a distinct nonce per cache file
// is the key-management layer's job, not this class's.

#include <cstddef>
#include <cstdint>

// Backend selection. On device wolfSSL's ChaCha20 does the work — it is already linked for
// TLS, and it measured 2.5x the throughput of the portable core (see CacheCipher.cpp). The
// portable core is what the host tests exercise, since wolfSSL is not built by that
// toolchain. Both are pinned to the same RFC 8439 vectors.
#if defined(FREEINK_NET_WOLFSSL)
#define CACHE_CIPHER_WOLFSSL 1
#endif

class CacheCipher {
 public:
  static constexpr size_t kKeyBytes = 32;
  static constexpr size_t kNonceBytes = 12;
  static constexpr size_t kBlockBytes = 64;

  // Both buffers are copied into the instance; neither needs to outlive the constructor.
  CacheCipher(const uint8_t key[kKeyBytes], const uint8_t nonce[kNonceBytes]);

  // XOR `len` bytes at `buf` with the keystream for absolute file offset `offset`.
  // Enciphering and deciphering are the same operation. `offset` need not be block-aligned;
  // a partial leading block costs one extra block computation, nothing more.
  //
  // Cost is ceil((offset % 64 + len) / 64) ChaCha20 block functions and no allocation, so a
  // caller may split a file into whatever chunks suit it as long as each chunk is applied at
  // its own absolute offset.
  //
  // **Returns false with the buffer untouched**, which a caller on a write path must treat as
  // fatal and abort on: the buffer then still holds plaintext, and writing it would put a
  // protected book in the clear on the SD card — the one outcome this class exists to
  // prevent. A read path may report a corrupt cache and rebuild.
  //
  // The RFC 8439 block counter is 32-bit, so the keystream repeats after 256 GiB. FAT32 caps
  // a file at 4 GiB, so no cache file can reach that; offsets beyond kMaxOffset are outside
  // the contract and are rejected rather than silently repeating the keystream. A zero-length
  // call is not a failure.
  [[nodiscard]] bool apply(uint8_t* buf, size_t len, uint64_t offset) const;

  static constexpr uint64_t kMaxOffset = (static_cast<uint64_t>(UINT32_MAX) + 1) * kBlockBytes - 1;

 private:
#if defined(CACHE_CIPHER_WOLFSSL)
  // The key and nonce, kept for the per-call wolfSSL context. A context is built on the stack
  // inside apply() rather than held as a member, so the call stays const and reentrant: SD
  // reads are serialised by the HAL mutex but the callers around them are not one task, and a
  // shared cipher context would be a race with no visible symptom beyond wrong bytes. Keying
  // costs a handful of word loads against a block function per 64 bytes.
  uint8_t key_[kKeyBytes];
  uint8_t nonce_[kNonceBytes];
#else
  // Keystream for one 64-byte block. `counter` is the RFC 8439 block counter.
  void keystreamBlock(uint32_t counter, uint8_t out[kBlockBytes]) const;

  // The 16-word ChaCha20 state with word 12 (the counter) left zero; filled per block.
  // A fixed-size member, not an allocation: 64 bytes, constructed once per protected book.
  uint32_t state_[16];
#endif
};
