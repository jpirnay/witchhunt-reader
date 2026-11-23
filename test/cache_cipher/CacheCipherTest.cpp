// Host tests for CacheCipher — docs/future_work/protected-content.md (D2).
//
// Two kinds of check, and both matter:
//   * the RFC 8439 vectors, which prove this is really ChaCha20 and not merely a
//     self-consistent scrambler;
//   * the random-access properties the reader depends on — the page path deciphers one page
//     record at its own absolute offset and must get exactly what a whole-file pass would.
//
// These cover the portable core. The device build uses wolfSSL's ChaCha20 instead (2.5x the
// throughput, already linked for TLS), which this toolchain cannot build; bench_crypto checks
// that path against the same RFC keystream vector.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "CacheCipher.h"

namespace {

// RFC 8439 uses key 00..1f throughout its examples.
void fillSequentialKey(uint8_t key[CacheCipher::kKeyBytes]) {
  for (size_t i = 0; i < CacheCipher::kKeyBytes; i++) {
    key[i] = static_cast<uint8_t>(i);
  }
}

// A deterministic, structure-free filler. Not random: a failure has to be reproducible.
std::vector<uint8_t> patternBuffer(const size_t len) {
  std::vector<uint8_t> out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    out.push_back(static_cast<uint8_t>((i * 31u + (i >> 3)) & 0xFF));
  }
  return out;
}

}  // namespace

// RFC 8439 §2.3.2: keystream for counter 1, nonce 00:00:00:09 00:00:00:4a 00:00:00:00.
// Our API addresses the keystream by file offset, so counter 1 is offset 64.
TEST(CacheCipherTest, MatchesRfc8439KeystreamVector) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonce[CacheCipher::kNonceBytes] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
                                                   0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
  const uint8_t expected[64] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3,
                                0x20, 0x71, 0xc4, 0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22,
                                0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e, 0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa,
                                0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2, 0xb5, 0x12, 0x9c, 0xd1,
                                0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};

  // Enciphering zeroes yields the raw keystream.
  uint8_t buf[64] = {0};
  EXPECT_TRUE(CacheCipher(key, nonce).apply(buf, sizeof(buf), 64));

  EXPECT_EQ(0, memcmp(buf, expected, sizeof(expected)));
}

// RFC 8439 §2.4.2: the "Ladies and Gentlemen" plaintext at counter 1.
TEST(CacheCipherTest, MatchesRfc8439EncryptionVector) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonce[CacheCipher::kNonceBytes] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                   0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
  const std::string plaintext =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
      "future, sunscreen would be it.";
  const uint8_t expected[114] = {
      0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81, 0xe9, 0x7e, 0x7a,
      0xec, 0x1d, 0x43, 0x60, 0xc2, 0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b, 0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47,
      0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57, 0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f,
      0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8, 0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61, 0x56, 0xa3, 0x8e, 0x08,
      0x8a, 0x22, 0xb6, 0x5e, 0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37,
      0x36, 0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42, 0x87, 0x4d};
  ASSERT_EQ(sizeof(expected), plaintext.size());

  std::vector<uint8_t> buf(plaintext.begin(), plaintext.end());
  EXPECT_TRUE(CacheCipher(key, nonce).apply(buf.data(), buf.size(), 64));

  EXPECT_EQ(0, memcmp(buf.data(), expected, sizeof(expected)));
}

// Enciphering and deciphering are the same call — the whole read path depends on it.
TEST(CacheCipherTest, ApplyIsItsOwnInverse) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonce[CacheCipher::kNonceBytes] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const CacheCipher cipher(key, nonce);

  const std::vector<uint8_t> original = patternBuffer(5000);
  std::vector<uint8_t> buf = original;

  ASSERT_TRUE(cipher.apply(buf.data(), buf.size(), 1234));
  EXPECT_NE(original, buf);
  ASSERT_TRUE(cipher.apply(buf.data(), buf.size(), 1234));
  EXPECT_EQ(original, buf);
}

// D2's rule, the property the page LUT relies on: deciphering an arbitrary slice at its own
// absolute offset must equal what a single whole-buffer pass produced there. Slice sizes are
// deliberately not block multiples, and one of them is smaller than a block.
TEST(CacheCipherTest, SliceAtOffsetMatchesWholeBufferPass) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonce[CacheCipher::kNonceBytes] = {0xaa, 0xbb, 0xcc, 0xdd, 0, 0, 0, 0, 0, 0, 0, 1};
  const CacheCipher cipher(key, nonce);

  const std::vector<uint8_t> plain = patternBuffer(10 * 1024);
  std::vector<uint8_t> whole = plain;
  ASSERT_TRUE(cipher.apply(whole.data(), whole.size(), 0));

  const size_t offsets[] = {0, 1, 63, 64, 65, 127, 4096, 9000};
  const size_t lengths[] = {1, 7, 63, 64, 65, 200, 1024, 1000};

  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    const size_t off = offsets[i];
    const size_t len = lengths[i];
    ASSERT_LE(off + len, whole.size());

    std::vector<uint8_t> slice(whole.begin() + static_cast<long>(off), whole.begin() + static_cast<long>(off + len));
    ASSERT_TRUE(cipher.apply(slice.data(), slice.size(), off));

    const std::vector<uint8_t> expected(plain.begin() + static_cast<long>(off),
                                        plain.begin() + static_cast<long>(off + len));
    EXPECT_EQ(expected, slice) << "offset " << off << " length " << len;
  }
}

// Writers hand over whatever chunk sizes suit them; chunking must not change the ciphertext.
TEST(CacheCipherTest, ChunkedWriteMatchesSinglePass) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonce[CacheCipher::kNonceBytes] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0};
  const CacheCipher cipher(key, nonce);

  const std::vector<uint8_t> plain = patternBuffer(3000);
  std::vector<uint8_t> oneShot = plain;
  ASSERT_TRUE(cipher.apply(oneShot.data(), oneShot.size(), 0));

  std::vector<uint8_t> chunked = plain;
  const size_t chunks[] = {1, 63, 64, 130, 512, 1000};
  size_t pos = 0;
  for (const size_t chunk : chunks) {
    const size_t take = chunk < chunked.size() - pos ? chunk : chunked.size() - pos;
    ASSERT_TRUE(cipher.apply(chunked.data() + pos, take, pos));
    pos += take;
  }
  if (pos < chunked.size()) {
    ASSERT_TRUE(cipher.apply(chunked.data() + pos, chunked.size() - pos, pos));
  }

  EXPECT_EQ(oneShot, chunked);
}

// Different nonces must not produce the same keystream — this is what keeps two cache files
// from being XOR-comparable once the key-derivation layer gives each its own nonce.
TEST(CacheCipherTest, DifferentNoncesDiverge) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonceA[CacheCipher::kNonceBytes] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  const uint8_t nonceB[CacheCipher::kNonceBytes] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

  std::vector<uint8_t> a(256, 0);
  std::vector<uint8_t> b(256, 0);
  ASSERT_TRUE(CacheCipher(key, nonceA).apply(a.data(), a.size(), 0));
  ASSERT_TRUE(CacheCipher(key, nonceB).apply(b.data(), b.size(), 0));

  EXPECT_NE(a, b);
}

// A refusal must be visible, because a caller that writes the buffer anyway would put
// plaintext on the card. Nothing asked for is not a refusal.
TEST(CacheCipherTest, EmptyIsSuccessAndNullIsRefused) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonce[CacheCipher::kNonceBytes] = {0};
  const CacheCipher cipher(key, nonce);

  std::vector<uint8_t> buf = patternBuffer(16);
  const std::vector<uint8_t> untouched = buf;

  EXPECT_TRUE(cipher.apply(buf.data(), 0, 0));
  EXPECT_FALSE(cipher.apply(nullptr, 16, 0));

  EXPECT_EQ(untouched, buf);
}

// Past the 32-bit block counter the keystream would repeat, which is worse than refusing.
// No FAT32 file can reach this; the guard exists so a bad offset fails loudly rather than
// quietly weakening the cipher on a device.
TEST(CacheCipherTest, RefusesOffsetsBeyondTheCounterRange) {
  uint8_t key[CacheCipher::kKeyBytes];
  fillSequentialKey(key);
  const uint8_t nonce[CacheCipher::kNonceBytes] = {0};
  const CacheCipher cipher(key, nonce);

  std::vector<uint8_t> buf = patternBuffer(64);
  const std::vector<uint8_t> untouched = buf;

  EXPECT_FALSE(cipher.apply(buf.data(), buf.size(), CacheCipher::kMaxOffset + 1));
  EXPECT_EQ(untouched, buf);

  // Straddling the end is refused too, rather than half-applied.
  EXPECT_FALSE(cipher.apply(buf.data(), buf.size(), CacheCipher::kMaxOffset - 8));
  EXPECT_EQ(untouched, buf);
}
