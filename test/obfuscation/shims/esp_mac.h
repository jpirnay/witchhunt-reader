#pragma once
// Host stand-in for ESP-IDF's eFuse MAC read. The reader under test is the LilyGo T5S3 of the
// device check (dc:b4:d9:2b:ee:d4); ObfuscationUtils reads it once and keeps it.

#include <cstdint>
#include <cstring>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;

inline constexpr uint8_t kTestReaderMac[6] = {0xdc, 0xb4, 0xd9, 0x2b, 0xee, 0xd4};

inline esp_err_t esp_efuse_mac_get_default(uint8_t* mac) {
  std::memcpy(mac, kTestReaderMac, sizeof(kTestReaderMac));
  return ESP_OK;
}
