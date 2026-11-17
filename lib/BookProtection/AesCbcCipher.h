#pragma once

#include <ZipFile.h>

// AES-CBC decryption on the chip's AES peripheral, through ESP-IDF's esp_aes driver, which the
// Wi-Fi stack links already (esp_aes_crypt_cbc is in the image without this): the cipher of a
// protected book's entries. Both chips the firmware runs on have the peripheral.
namespace book_protection {
const ZipFile::CipherOps& aesCbcCipher();
}
