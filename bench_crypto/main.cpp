// ESP32-C3/S3 cache-cipher benchmark — docs/future_work/protected-content.md (D2).
//
// Decides two things with numbers instead of assumptions:
//
//   1. Is the cache cipher cheap enough to meet the performance contract in D2
//      (< 2 ms per page turn, < 10% on a section build)? A page turn deciphers one page
//      record — single-digit KB — so the 2 KB and 4 KB rows are the ones that matter; the
//      64 KB row stands in for an image or a whole section pass.
//   2. Is our own ChaCha20 worth carrying when wolfSSL already ships one, and how does AES
//      compare — both as an alternative for the cache and as the mandated cipher for the
//      book's own entries (AES-128-CBC for ADEPT, AES-256-CBC for LCP), which is what the
//      first-open budget pays.
//
// Crypto here is 100% software: the Espressif hardware port is gated on WOLFSSL_ESPIDF,
// which never turns on under Arduino. See D2 for what enabling it would and would
// not buy.
//
// Build & flash:  pio run -e bench_crypto -t upload
// Monitor:        pio device monitor -e bench_crypto

#include <Arduino.h>
#include <esp_timer.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/chacha.h>

#include "CacheCipher.h"

namespace {

// Static, not heap: the benchmark owns the whole device and a 64 KB run must not depend on
// how fragmented the heap happens to be.
constexpr size_t kMaxBuffer = 64 * 1024;
uint8_t buffer[kMaxBuffer];

// Sizes that map to something real: a page record, a generous page record, a whole image or
// section pass.
constexpr size_t kSizes[] = {2 * 1024, 4 * 1024, 64 * 1024};

// Enough repetitions that esp_timer's 1 µs resolution is noise, without making a run tedious.
constexpr int kReps = 64;

uint8_t key32[32];
uint8_t nonce12[12];
uint8_t iv16[16];

void seedInputs() {
  for (size_t i = 0; i < sizeof(key32); i++) key32[i] = static_cast<uint8_t>(i * 7 + 1);
  for (size_t i = 0; i < sizeof(nonce12); i++) nonce12[i] = static_cast<uint8_t>(i * 13 + 3);
  for (size_t i = 0; i < sizeof(iv16); i++) iv16[i] = static_cast<uint8_t>(i * 5 + 11);
  for (size_t i = 0; i < kMaxBuffer; i++) buffer[i] = static_cast<uint8_t>(i * 31 + (i >> 5));
}

// One row of the report. `us` is the total for kReps passes over `len` bytes.
void report(const char* what, const size_t len, const int64_t us) {
  const double perCallUs = static_cast<double>(us) / kReps;
  const double mbPerSec = (static_cast<double>(len) * kReps) / static_cast<double>(us);
  Serial.printf("%-28s %6u B  %8.1f us/call  %7.2f MB/s\n", what, static_cast<unsigned>(len), perCallUs, mbPerSec);
}

void benchCacheCipher(const size_t len) {
  const CacheCipher cipher(key32, nonce12);
  // Offset 4096 rather than 0: the real caller always deciphers at a file offset, and an
  // unaligned start costs one extra block. Keep the measurement honest about that.
  const int64_t start = esp_timer_get_time();
  for (int i = 0; i < kReps; i++) {
    if (!cipher.apply(buffer, len, 4096 + static_cast<uint64_t>(i))) {
      Serial.println("CacheCipher::apply refused - benchmark invalid");
      return;
    }
  }
  report("CacheCipher (device path)", len, esp_timer_get_time() - start);
}

void benchWolfChacha(const size_t len) {
  ChaCha ctx;
  if (wc_Chacha_SetKey(&ctx, key32, sizeof(key32)) != 0) {
    Serial.println("wc_Chacha_SetKey failed");
    return;
  }
  const int64_t start = esp_timer_get_time();
  for (int i = 0; i < kReps; i++) {
    // Counter set per call, as random access into a file would require.
    wc_Chacha_SetIV(&ctx, nonce12, static_cast<word32>(64 + i));
    wc_Chacha_Process(&ctx, buffer, buffer, static_cast<word32>(len));
  }
  report("wolfSSL ChaCha20", len, esp_timer_get_time() - start);
}

void benchAesCbcDecrypt(const size_t len, const int keyBits) {
  Aes aes;
  if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) {
    Serial.println("wc_AesInit failed");
    return;
  }
  if (wc_AesSetKey(&aes, key32, keyBits / 8, iv16, AES_DECRYPTION) != 0) {
    Serial.printf("wc_AesSetKey(%d) failed - mode not compiled in?\n", keyBits);
    wc_AesFree(&aes);
    return;
  }
  const int64_t start = esp_timer_get_time();
  for (int i = 0; i < kReps; i++) {
    wc_AesCbcDecrypt(&aes, buffer, buffer, static_cast<word32>(len));
  }
  const int64_t elapsed = esp_timer_get_time() - start;
  wc_AesFree(&aes);
  char label[32];
  snprintf(label, sizeof(label), "wolfSSL AES-%d-CBC dec", keyBits);
  report(label, len, elapsed);
}

#ifdef WOLFSSL_AES_COUNTER
void benchAesCtr(const size_t len, const int keyBits) {
  Aes aes;
  if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) {
    Serial.println("wc_AesInit failed");
    return;
  }
  // wc_AesCtrSetKey, not wc_AesSetKeyDirect: the latter needs WOLFSSL_AES_DIRECT, which our
  // TLS profile does not define, while this one ships with WOLFSSL_AES_COUNTER itself.
  if (wc_AesCtrSetKey(&aes, key32, keyBits / 8, iv16, AES_ENCRYPTION) != 0) {
    Serial.printf("wc_AesCtrSetKey(%d) failed\n", keyBits);
    wc_AesFree(&aes);
    return;
  }
  const int64_t start = esp_timer_get_time();
  for (int i = 0; i < kReps; i++) {
    wc_AesCtrEncrypt(&aes, buffer, buffer, static_cast<word32>(len));
  }
  const int64_t elapsed = esp_timer_get_time() - start;
  wc_AesFree(&aes);
  char label[32];
  snprintf(label, sizeof(label), "wolfSSL AES-%d-CTR", keyBits);
  report(label, len, elapsed);
}
#endif

// The device build uses wolfSSL's ChaCha20 while the host tests exercise the portable core,
// so the RFC 8439 keystream vector is checked on BOTH sides rather than assumed to carry
// across. Same vector as test/cache_cipher: §2.3.2, counter 1, which our offset-addressed API
// reaches at offset 64.
void verifyRfcVector() {
  uint8_t key[32];
  for (size_t i = 0; i < sizeof(key); i++) key[i] = static_cast<uint8_t>(i);
  const uint8_t nonce[12] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
  const uint8_t expected[64] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3,
                                0x20, 0x71, 0xc4, 0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22,
                                0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e, 0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa,
                                0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2, 0xb5, 0x12, 0x9c, 0xd1,
                                0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};

  uint8_t block[64] = {0};
  const bool ok = CacheCipher(key, nonce).apply(block, sizeof(block), 64);
  Serial.printf("CacheCipher RFC 8439 vector: %s\n",
                (ok && memcmp(block, expected, sizeof(expected)) == 0) ? "PASS" : "FAIL");
}

// Correctness before speed: a fast cipher that deciphers to the wrong bytes is worthless, and
// the host tests cannot prove the RISC-V build behaves like the mingw one.
void verifyRoundTrip() {
  const CacheCipher cipher(key32, nonce12);
  constexpr size_t kLen = 1024;
  constexpr uint64_t kOffset = 7777;  // deliberately not block-aligned
  uint8_t original[kLen];
  memcpy(original, buffer, kLen);

  bool ok = cipher.apply(buffer, kLen, kOffset);
  const bool changed = memcmp(original, buffer, kLen) != 0;
  ok = cipher.apply(buffer, kLen, kOffset) && ok;
  const bool restored = memcmp(original, buffer, kLen) == 0;

  Serial.printf("CacheCipher round-trip at unaligned offset: %s\n", (ok && changed && restored) ? "PASS" : "FAIL");

  // And a slice at its own offset must match the whole-buffer pass - the property the page
  // LUT depends on (D2).
  uint8_t slice[200];
  ok = cipher.apply(buffer, kLen, kOffset);  // buffer is ciphertext again
  memcpy(slice, buffer + 64, sizeof(slice));
  ok = cipher.apply(slice, sizeof(slice), kOffset + 64) && ok;
  Serial.printf("CacheCipher slice-at-offset: %s\n",
                (ok && memcmp(slice, original + 64, sizeof(slice)) == 0) ? "PASS" : "FAIL");
  ok = cipher.apply(buffer, kLen, kOffset) && ok;  // leave the buffer as plaintext for the runs below
  if (!ok) Serial.println("CacheCipher::apply refused during verification");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(3000);  // USB-CDC enumeration; without it the first lines are lost

  Serial.println();
  Serial.println("=== cache cipher benchmark ===");
  Serial.printf("CPU %u MHz, %d reps per row, software crypto only\n", static_cast<unsigned>(ESP.getCpuFreqMHz()),
                kReps);
  Serial.println();

  seedInputs();
  verifyRfcVector();
  verifyRoundTrip();
  Serial.println();

  for (const size_t len : kSizes) {
    benchCacheCipher(len);
    benchWolfChacha(len);
    benchAesCbcDecrypt(len, 128);
    benchAesCbcDecrypt(len, 256);
#ifdef WOLFSSL_AES_COUNTER
    benchAesCtr(len, 256);
#else
    Serial.println("wolfSSL AES-CTR                 not compiled in (WOLFSSL_AES_COUNTER)");
#endif
    Serial.println();
  }

  Serial.println("Budgets (D2): page turn < 2 ms for a single-digit-KB record;");
  Serial.println("section build < 10%; cached image < 25 ms for ~48 KB.");
  Serial.println("=== done ===");
}

void loop() { delay(1000); }
