#include "DeviceSecret.h"

#include <BssMutex.h>
#include <Logging.h>
#include <esp_random.h>
#include <nvs.h>

#include <cstring>
#include <mutex>

namespace {
// The web server's task and the reader can both ask first. witchhunt: a .bss mutex where upstream
// has a function-local std::mutex, which ESP-IDF backs with heap it takes at the first lock.
BssMutex g_secretLock;
uint8_t g_secret[32];
bool g_loaded = false;
}  // namespace

bool deviceSecret(uint8_t (&out)[32]) {
  const std::lock_guard<BssMutex> guard(g_secretLock);

  // A failure is not cached: the next call retries.
  if (!g_loaded) {
    nvs_handle_t handle;
    if (nvs_open("devid", NVS_READWRITE, &handle) == ESP_OK) {
      size_t stored = sizeof(g_secret);
      const esp_err_t err = nvs_get_blob(handle, "secret", g_secret, &stored);
      if (err == ESP_OK) {
        g_loaded = stored == sizeof(g_secret);
      } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Only a missing secret is created. Any other read error leaves the stored one alone:
        // replacing it would orphan every wrapped book key.
        esp_fill_random(g_secret, sizeof(g_secret));
        g_loaded = nvs_set_blob(handle, "secret", g_secret, sizeof(g_secret)) == ESP_OK && nvs_commit(handle) == ESP_OK;
      }
      nvs_close(handle);
    }
    if (!g_loaded) {
      LOG_ERR("DSEC", "Device secret unavailable");
      return false;
    }
  }
  memcpy(out, g_secret, sizeof(out));
  return true;
}
