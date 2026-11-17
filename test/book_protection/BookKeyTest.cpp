#include <DeviceSecret.h>
#include <gtest/gtest.h>
#include <nvs.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "BookKey.h"

// "<book>.key": a content key wrapped to this device (AES-256-GCM under SHA-256("book-key:" ||
// the NVS device secret)), upstream's format byte for byte.

namespace {

std::vector<uint8_t> fileBytes(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  uint8_t buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* f = fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  fwrite(bytes.data(), 1, bytes.size(), f);
  fclose(f);
}

class BookKeyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = ::testing::TempDir() + "book_key_" + std::to_string(getpid()) + "/";
    std::filesystem::create_directories(dir_);
    book_ = dir_ + "loan.epub";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string dir_, book_;
};

}  // namespace

TEST_F(BookKeyTest, RoundTrips) {
  for (const size_t len : {size_t{16}, size_t{32}}) {
    uint8_t key[32];
    for (size_t i = 0; i < len; ++i) key[i] = static_cast<uint8_t>(i * 7 + len);
    ASSERT_TRUE(bookkey::write(book_, key, len, 1893456000));
    EXPECT_EQ(fileBytes(book_ + ".key").size(), 14 + 12 + len + 16);
    uint8_t out[bookkey::MAX_KEY_LEN] = {};
    int64_t expires = 0;
    ASSERT_EQ(bookkey::read(book_, out, &expires), len);
    EXPECT_EQ(memcmp(out, key, len), 0);
    EXPECT_EQ(expires, 1893456000);
  }
  EXPECT_FALSE(std::filesystem::exists(book_ + ".key.tmp"));
  uint8_t key[16] = {};
  EXPECT_FALSE(bookkey::write(book_, key, 24, 0));
}

TEST_F(BookKeyTest, RefusesWhatThisDeviceDidNotWrap) {
  const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  uint8_t out[bookkey::MAX_KEY_LEN];
  int64_t expires = 0;
  EXPECT_EQ(bookkey::read(book_, out, &expires), 0u);  // no key file

  ASSERT_TRUE(bookkey::write(book_, key, sizeof(key), 0));
  // The due date is in the authenticated header: moving it breaks the tag.
  std::vector<uint8_t> bytes = fileBytes(book_ + ".key");
  bytes[6] ^= 0x01;
  writeBytes(book_ + ".key", bytes);
  EXPECT_EQ(bookkey::read(book_, out, &expires), 0u);

  ASSERT_TRUE(bookkey::write(book_, key, sizeof(key), 0));
  ASSERT_EQ(bookkey::read(book_, out, &expires), 16u);
  bytes = fileBytes(book_ + ".key");
  bytes.pop_back();  // cut short
  writeBytes(book_ + ".key", bytes);
  EXPECT_EQ(bookkey::read(book_, out, &expires), 0u);
}

TEST_F(BookKeyTest, OtherDeviceCannotRead) {
  const uint8_t key[16] = {9};
  ASSERT_TRUE(bookkey::write(book_, key, sizeof(key), 0));
  // DeviceSecret keeps the secret it loaded: a key this process wrapped stays readable here.
  // What another device would make of the file is a different KEK; shown by unwrapping with one.
  const std::vector<uint8_t> bytes = fileBytes(book_ + ".key");
  uint8_t kek[32];
  const char input[] = "book-key:another device's secret, 32 bytes long....";
  SHA256(reinterpret_cast<const uint8_t*>(input), sizeof(input) - 1, kek);
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int n = 0;
  uint8_t plain[16];
  uint8_t tag[16];
  memcpy(tag, bytes.data() + 14 + 12 + 16, 16);
  ASSERT_EQ(EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), 1);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
  EVP_DecryptInit_ex(c, nullptr, nullptr, kek, bytes.data() + 14);
  EVP_DecryptUpdate(c, nullptr, &n, bytes.data(), 14);
  EVP_DecryptUpdate(c, plain, &n, bytes.data() + 26, 16);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag);
  EXPECT_NE(EVP_DecryptFinal_ex(c, plain + n, &n), 1);
  EVP_CIPHER_CTX_free(c);
}

// Version 1 (no key-length byte, 16-byte keys) still reads.
TEST_F(BookKeyTest, ReadsVersionOne) {
  uint8_t secret[32];
  ASSERT_TRUE(deviceSecret(secret));
  uint8_t kekInput[9 + 32];
  memcpy(kekInput, "book-key:", 9);
  memcpy(kekInput + 9, secret, 32);
  uint8_t kek[32];
  SHA256(kekInput, sizeof(kekInput), kek);

  const uint8_t key[16] = {0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  std::vector<uint8_t> file = {'F', 'I', 'B', 'K', 1};
  const uint64_t expires = 1700000000;
  for (int i = 0; i < 8; ++i) file.push_back(static_cast<uint8_t>(expires >> (8 * i)));
  const uint8_t iv[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  file.insert(file.end(), iv, iv + 12);
  uint8_t wrapped[16];
  uint8_t tag[16];
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int n = 0;
  ASSERT_EQ(EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), 1);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
  EVP_EncryptInit_ex(c, nullptr, nullptr, kek, iv);
  EVP_EncryptUpdate(c, nullptr, &n, file.data(), 13);
  EVP_EncryptUpdate(c, wrapped, &n, key, 16);
  EVP_EncryptFinal_ex(c, wrapped + n, &n);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag);
  EVP_CIPHER_CTX_free(c);
  file.insert(file.end(), wrapped, wrapped + 16);
  file.insert(file.end(), tag, tag + 16);
  writeBytes(book_ + ".key", file);

  uint8_t out[bookkey::MAX_KEY_LEN] = {};
  int64_t got = 0;
  ASSERT_EQ(bookkey::read(book_, out, &got), 16u);
  EXPECT_EQ(memcmp(out, key, 16), 0);
  EXPECT_EQ(got, 1700000000);
}

// The secret is made once, kept in NVS where upstream keeps it, and the same on every call.
TEST_F(BookKeyTest, DeviceSecretIsStable) {
  uint8_t a[32], b[32];
  ASSERT_TRUE(deviceSecret(a));
  ASSERT_TRUE(deviceSecret(b));
  EXPECT_EQ(memcmp(a, b, 32), 0);
  const auto stored = host_nvs::blobs().find("devid/secret");
  ASSERT_NE(stored, host_nvs::blobs().end());
  ASSERT_EQ(stored->second.size(), 32u);
  EXPECT_EQ(memcmp(stored->second.data(), a, 32), 0);
}
