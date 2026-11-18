#pragma once

// Host stand-in for ESP-IDF's NVS: the blob calls DeviceSecret makes, over a map a test can clear
// (a full flash erase: a new device secret) or make fail.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using esp_err_t = int;
using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
#ifndef ESP_OK
#define ESP_OK 0
#endif
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102

namespace host_nvs {
inline std::map<std::string, std::vector<uint8_t>>& blobs() {
  static std::map<std::string, std::vector<uint8_t>> store;
  return store;
}
inline bool& failOpen() {
  static bool fail = false;
  return fail;
}
inline std::string& openNamespace() {
  static std::string ns;
  return ns;
}
}  // namespace host_nvs

inline esp_err_t nvs_open(const char* ns, nvs_open_mode_t, nvs_handle_t* handle) {
  if (host_nvs::failOpen()) return ESP_FAIL;
  host_nvs::openNamespace() = ns;
  *handle = 1;
  return ESP_OK;
}
inline esp_err_t nvs_get_blob(nvs_handle_t, const char* key, void* out, size_t* length) {
  const auto it = host_nvs::blobs().find(host_nvs::openNamespace() + "/" + key);
  if (it == host_nvs::blobs().end()) return ESP_ERR_NVS_NOT_FOUND;
  if (*length < it->second.size()) return ESP_FAIL;
  memcpy(out, it->second.data(), it->second.size());
  *length = it->second.size();
  return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t, const char* key, const void* value, size_t length) {
  const auto* p = static_cast<const uint8_t*>(value);
  host_nvs::blobs()[host_nvs::openNamespace() + "/" + key].assign(p, p + length);
  return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
inline void nvs_close(nvs_handle_t) {}
