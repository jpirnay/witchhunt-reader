#pragma once

// Host stand-in for wolfCrypt's AES-GCM, over OpenSSL: the calls the book-key wrap makes, with
// wolfCrypt's argument order and return codes (0 is success).

#include <openssl/evp.h>

#include <cstdint>
#include <cstring>

#define INVALID_DEVID -2

struct Aes {
  uint8_t key[32];
  uint32_t keyLen;
};

inline int wc_AesInit(Aes* aes, void*, int) {
  memset(aes, 0, sizeof(*aes));
  return 0;
}
inline void wc_AesFree(Aes* aes) { memset(aes, 0, sizeof(*aes)); }
inline int wc_AesGcmSetKey(Aes* aes, const uint8_t* key, const uint32_t len) {
  if (len != 16 && len != 24 && len != 32) return -1;
  memcpy(aes->key, key, len);
  aes->keyLen = len;
  return 0;
}

namespace host_gcm {
inline const EVP_CIPHER* cipher(const Aes* aes) {
  return aes->keyLen == 16 ? EVP_aes_128_gcm() : aes->keyLen == 24 ? EVP_aes_192_gcm() : EVP_aes_256_gcm();
}
}  // namespace host_gcm

inline int wc_AesGcmEncrypt(Aes* aes, uint8_t* out, const uint8_t* in, const uint32_t sz, const uint8_t* iv,
                            const uint32_t ivSz, uint8_t* authTag, const uint32_t authTagSz, const uint8_t* authIn,
                            const uint32_t authInSz) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int n = 0;
  bool ok = c != nullptr && EVP_EncryptInit_ex(c, host_gcm::cipher(aes), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(ivSz), nullptr) == 1 &&
            EVP_EncryptInit_ex(c, nullptr, nullptr, aes->key, iv) == 1 &&
            EVP_EncryptUpdate(c, nullptr, &n, authIn, static_cast<int>(authInSz)) == 1 &&
            EVP_EncryptUpdate(c, out, &n, in, static_cast<int>(sz)) == 1 && EVP_EncryptFinal_ex(c, out + n, &n) == 1 &&
            EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, static_cast<int>(authTagSz), authTag) == 1;
  EVP_CIPHER_CTX_free(c);
  return ok ? 0 : -1;
}

inline int wc_AesGcmDecrypt(Aes* aes, uint8_t* out, const uint8_t* in, const uint32_t sz, const uint8_t* iv,
                            const uint32_t ivSz, const uint8_t* authTag, const uint32_t authTagSz,
                            const uint8_t* authIn, const uint32_t authInSz) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int n = 0;
  uint8_t tag[16];
  memcpy(tag, authTag, authTagSz);
  bool ok = c != nullptr && EVP_DecryptInit_ex(c, host_gcm::cipher(aes), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(ivSz), nullptr) == 1 &&
            EVP_DecryptInit_ex(c, nullptr, nullptr, aes->key, iv) == 1 &&
            EVP_DecryptUpdate(c, nullptr, &n, authIn, static_cast<int>(authInSz)) == 1 &&
            EVP_DecryptUpdate(c, out, &n, in, static_cast<int>(sz)) == 1 &&
            EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, static_cast<int>(authTagSz), tag) == 1 &&
            EVP_DecryptFinal_ex(c, out + n, &n) == 1;
  EVP_CIPHER_CTX_free(c);
  return ok ? 0 : -1;
}
