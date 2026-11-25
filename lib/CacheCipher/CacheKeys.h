#pragma once

// Key and nonce derivation for the protected-book cache — docs/design/cache-cipher.md (D2).
// Pure functions: no storage, no NVS, no Arduino, so the derivation is pinned by host
// tests. The plumbing that supplies the device key and the per-book salt lives on the device
// side; this file only says how the bytes are computed.
//
// The shape:
//
//   device key   32 random bytes, generated once, kept in NVS (internal flash, never on the
//                removable card). Pull the SD card and the caches on it are noise.
//   salt         12 random bytes per cache directory, written when that directory is created
//                and stored inside it. Cheap to regenerate: deleting the cache regenerates it.
//   book key     = ChaCha20 keystream(device key, salt)[0..32]
//   file nonce   = a 96-bit hash of the file's name within the cache directory
//
// Why a salt rather than hashing the directory name: the cache directory is keyed by a hash
// of the book's path, so the same book at the same path would derive the same key forever.
// A salt regenerated with the directory means a rebuilt cache is enciphered differently from
// the one it replaced, which is what stops a rebuild from reusing an old keystream wholesale.
//
// Why a plain hash for the nonce: a nonce must be *unique* under a given key, not secret and
// not unpredictable — it is stored in the clear in any design. A 96-bit name hash makes a
// collision between two names in one cache directory negligible (a large book has a few
// thousand cache files; the birthday bound is around 2^48).
//
// **Known limit, deliberately accepted.** Writing two different contents to the *same* name
// under the same salt reuses that keystream, and anyone holding both ciphertexts learns their
// XOR. Two things keep this narrow: section caches embed a property hash in the filename
// (Section::getSectionFilePath), so a settings change writes a different file rather than
// rewriting one; and a fresh salt accompanies every new cache directory. What remains is a
// rebuild under identical settings — an interrupted build restarted, say — where the new
// content overwrites the old in place. An attacker would need a snapshot of the card from
// before *and* after that rebuild. If that is ever judged too weak, the fix is at the writer:
// rebuild under a new name rather than in place.

#include <cstddef>
#include <cstdint>

namespace cachekeys {

constexpr size_t kDeviceKeyBytes = 32;
constexpr size_t kSaltBytes = 12;
constexpr size_t kBookKeyBytes = 32;
constexpr size_t kNonceBytes = 12;

// bookKey = the first 32 bytes of the ChaCha20 keystream under (device key, salt).
// ChaCha20 as a PRF rather than a hash function: it is already the one primitive this library
// carries on both host and device, so the derivation needs no second algorithm.
void deriveBookKey(const uint8_t deviceKey[kDeviceKeyBytes], const uint8_t salt[kSaltBytes],
                   uint8_t out[kBookKeyBytes]);

// A per-file nonce from the file's name as it appears inside the cache directory. Pass the
// path relative to that directory ("sections/12_a1b2c3d4.bin"), not an absolute path: the
// absolute form contains the directory name, which is the same for every file in the book and
// would only add constant bytes.
void deriveFileNonce(const char* relativeName, uint8_t out[kNonceBytes]);

}  // namespace cachekeys
