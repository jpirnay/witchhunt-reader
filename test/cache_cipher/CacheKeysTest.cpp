// Host tests for the cache key/nonce derivation — docs/future_work/protected-content.md (D2).
//
// The properties that matter are separation (nothing shares a keystream that shouldn't) and
// stability (the derivation cannot drift, because a change silently invalidates every cache
// on every card).

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "CacheKeys.h"

namespace {

std::vector<uint8_t> bookKey(const uint8_t deviceKeyByte, const uint8_t saltByte) {
  uint8_t deviceKey[cachekeys::kDeviceKeyBytes];
  uint8_t salt[cachekeys::kSaltBytes];
  memset(deviceKey, deviceKeyByte, sizeof(deviceKey));
  memset(salt, saltByte, sizeof(salt));

  std::vector<uint8_t> out(cachekeys::kBookKeyBytes);
  cachekeys::deriveBookKey(deviceKey, salt, out.data());
  return out;
}

std::vector<uint8_t> fileNonce(const char* name) {
  std::vector<uint8_t> out(cachekeys::kNonceBytes);
  cachekeys::deriveFileNonce(name, out.data());
  return out;
}

}  // namespace

TEST(CacheKeysTest, BookKeyIsDeterministic) { EXPECT_EQ(bookKey(0x11, 0x22), bookKey(0x11, 0x22)); }

// A different device must not derive the same book key from the same card.
TEST(CacheKeysTest, BookKeyDependsOnDeviceKey) { EXPECT_NE(bookKey(0x11, 0x22), bookKey(0x12, 0x22)); }

// The salt is what makes a rebuilt cache differ from the one it replaced.
TEST(CacheKeysTest, BookKeyDependsOnSalt) { EXPECT_NE(bookKey(0x11, 0x22), bookKey(0x11, 0x23)); }

TEST(CacheKeysTest, BookKeyIsNotTrivial) {
  const std::vector<uint8_t> key = bookKey(0x11, 0x22);
  const std::vector<uint8_t> zeroes(cachekeys::kBookKeyBytes, 0);
  EXPECT_NE(zeroes, key);

  // Not merely a copy of the inputs either.
  const std::vector<uint8_t> deviceKeyBytes(cachekeys::kBookKeyBytes, 0x11);
  EXPECT_NE(deviceKeyBytes, key);
}

TEST(CacheKeysTest, FileNonceIsDeterministic) {
  EXPECT_EQ(fileNonce("sections/12_a1b2c3d4.bin"), fileNonce("sections/12_a1b2c3d4.bin"));
}

// Every file in a cache directory shares one key, so distinct names must give distinct
// nonces — this is the property that keeps two cache files from being XOR-comparable.
TEST(CacheKeysTest, FileNoncesAreDistinctAcrossRealisticNames) {
  std::set<std::vector<uint8_t>> seen;
  size_t count = 0;

  for (int spine = 0; spine < 400; spine++) {
    for (const char* suffix : {"a1b2c3d4", "00000000", "ffffffff"}) {
      char name[64];
      snprintf(name, sizeof(name), "sections/%d_%s.bin", spine, suffix);
      seen.insert(fileNonce(name));
      count++;
    }
    char html[64];
    snprintf(html, sizeof(html), "sections/html_%d.bin", spine);
    seen.insert(fileNonce(html));
    count++;

    char img[64];
    snprintf(img, sizeof(img), "img_%d.bmp", spine);
    seen.insert(fileNonce(img));
    count++;
  }
  seen.insert(fileNonce("pagelist.bin"));
  count++;

  EXPECT_EQ(count, seen.size()) << "two cache file names collided into one nonce";
}

// Names differing only in ways a truncated hash would miss.
TEST(CacheKeysTest, FileNoncesSeparateNearIdenticalNames) {
  EXPECT_NE(fileNonce("sections/1_aaaaaaaa.bin"), fileNonce("sections/1_aaaaaaab.bin"));
  EXPECT_NE(fileNonce("sections/1.bin"), fileNonce("sections/10.bin"));
  EXPECT_NE(fileNonce("a"), fileNonce("aa"));
  EXPECT_NE(fileNonce("ab"), fileNonce("ba"));
  EXPECT_NE(fileNonce(""), fileNonce("a"));
}

TEST(CacheKeysTest, FileNonceHandlesNullName) {
  std::vector<uint8_t> out(cachekeys::kNonceBytes, 0xEE);
  cachekeys::deriveFileNonce(nullptr, out.data());
  EXPECT_EQ(fileNonce(""), out);  // treated as the empty name, not left as garbage
}

// Known answers, pinned deliberately. These are not from a standard — they are this
// derivation's own output, captured so that changing it has to be a decision: a silent change
// makes every cache on every card undecipherable, and the symptom would be a corrupt-looking
// book rather than an obvious error.
TEST(CacheKeysTest, DerivationIsPinned) {
  const std::vector<uint8_t> key = bookKey(0x11, 0x22);
  const uint8_t expectedKey[cachekeys::kBookKeyBytes] = {
      0xd8, 0x1d, 0x93, 0x33, 0x68, 0x18, 0xd1, 0x33, 0x7d, 0x97, 0xc4, 0x5b, 0x0f, 0x3a, 0x76, 0xa6,
      0xd5, 0x76, 0xb7, 0x22, 0x6a, 0xe8, 0xc0, 0x8d, 0xe3, 0x38, 0x8b, 0x9b, 0x18, 0x25, 0x79, 0xfc};
  EXPECT_EQ(0, memcmp(key.data(), expectedKey, sizeof(expectedKey)));

  const std::vector<uint8_t> nonce = fileNonce("sections/12_a1b2c3d4.bin");
  const uint8_t expectedNonce[cachekeys::kNonceBytes] = {0xdb, 0x62, 0x88, 0xdf, 0x74, 0x66,
                                                         0x9d, 0x4e, 0x77, 0xfe, 0xb5, 0x6f};
  EXPECT_EQ(0, memcmp(nonce.data(), expectedNonce, sizeof(expectedNonce)));
}
