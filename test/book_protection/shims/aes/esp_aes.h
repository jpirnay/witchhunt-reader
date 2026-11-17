#pragma once

// Host stand-in for ESP-IDF's AES driver (aes/esp_aes.h) over OpenSSL: the calls lib/BookProtection
// makes, with the driver's contract -- the CBC chain carried in `iv` from call to call, and input
// and output allowed to be the same buffer.

#include <openssl/evp.h>

#include <cstddef>
#include <cstring>

#define ESP_AES_ENCRYPT 1
#define ESP_AES_DECRYPT 0

typedef struct {
  unsigned char key[32];
  unsigned int key_bytes;
} esp_aes_context;

inline void esp_aes_init(esp_aes_context* ctx) { memset(ctx, 0, sizeof(*ctx)); }
inline void esp_aes_free(esp_aes_context* ctx) { memset(ctx, 0, sizeof(*ctx)); }

inline int esp_aes_setkey(esp_aes_context* ctx, const unsigned char* key, const unsigned int keybits) {
  if (keybits != 128 && keybits != 192 && keybits != 256) return -1;
  memcpy(ctx->key, key, keybits / 8);
  ctx->key_bytes = keybits / 8;
  return 0;
}

inline int esp_aes_crypt_cbc(esp_aes_context* ctx, const int mode, const size_t length, unsigned char iv[16],
                             const unsigned char* input, unsigned char* output) {
  if (length == 0 || length % 16 != 0) return -1;
  const EVP_CIPHER* cipher = ctx->key_bytes == 16   ? EVP_aes_128_cbc()
                             : ctx->key_bytes == 24 ? EVP_aes_192_cbc()
                                                    : EVP_aes_256_cbc();
  unsigned char next[16];
  if (mode == ESP_AES_DECRYPT) memcpy(next, input + length - 16, 16);  // before an in-place call overwrites it
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int produced = 0;
  const bool ok = c != nullptr && EVP_CipherInit_ex(c, cipher, nullptr, ctx->key, iv, mode == ESP_AES_ENCRYPT) == 1 &&
                  EVP_CIPHER_CTX_set_padding(c, 0) == 1 &&
                  EVP_CipherUpdate(c, output, &produced, input, static_cast<int>(length)) == 1 &&
                  produced == static_cast<int>(length);
  EVP_CIPHER_CTX_free(c);
  if (!ok) return -1;
  if (mode == ESP_AES_ENCRYPT) memcpy(next, output + length - 16, 16);
  memcpy(iv, next, 16);
  return 0;
}
