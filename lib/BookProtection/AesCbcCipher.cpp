#include "AesCbcCipher.h"

#include <aes/esp_aes.h>

#include <cstring>

namespace {
struct AesCbcState {
  esp_aes_context aes;
  uint8_t iv[16];  // the chain: esp_aes_crypt_cbc leaves the last ciphertext block here
};

bool aesBegin(void* state, const uint8_t* key, const size_t keyLen, const uint8_t iv[16]) {
  auto* s = static_cast<AesCbcState*>(state);
  esp_aes_init(&s->aes);
  if (esp_aes_setkey(&s->aes, key, static_cast<unsigned int>(keyLen * 8)) != 0) {
    esp_aes_free(&s->aes);
    return false;
  }
  memcpy(s->iv, iv, sizeof(s->iv));
  return true;
}

bool aesDecrypt(void* state, uint8_t* data, const size_t len) {
  auto* s = static_cast<AesCbcState*>(state);
  return esp_aes_crypt_cbc(&s->aes, ESP_AES_DECRYPT, len, s->iv, data, data) == 0;
}

void aesEnd(void* state) {
  auto* s = static_cast<AesCbcState*>(state);
  esp_aes_free(&s->aes);  // zeroes the key
  volatile uint8_t* iv = s->iv;
  for (size_t i = 0; i < sizeof(s->iv); ++i) iv[i] = 0;
}

const ZipFile::CipherOps kAesCbc = {sizeof(AesCbcState), aesBegin, aesDecrypt, aesEnd};
}  // namespace

namespace book_protection {
const ZipFile::CipherOps& aesCbcCipher() { return kAesCbc; }
}  // namespace book_protection
