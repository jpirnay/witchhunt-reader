#include <BoardConfig.h>

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include <Logging.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

namespace {
constexpr char kWitchNvs[] = "witch_nvs";
}

extern "C" esp_err_t __wrap_nvs_flash_init() {
  const esp_err_t result = nvs_flash_init_partition(kWitchNvs);
  // Arduino handles these two errors by erasing the first NVS partition.
  // Always leave the other firmware's default partition alone.
  if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    return ESP_FAIL;
  }
  return result;
}

extern "C" esp_err_t __wrap_nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
  return nvs_open_from_partition(kWitchNvs, name, mode, handle);
}

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);

extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  // WitchHunt saves credentials on SD. Like CloudZao, keep the Wi-Fi driver's
  // settings in RAM so initialization works without the other slot's NVS.
  wifi_init_config_t inRam = *config;
  inRam.nvs_enable = false;
  const esp_err_t result = __real_esp_wifi_init(&inRam);
  if (result != ESP_OK) LOG_ERR("WIFI", "Metalio Wi-Fi initialization failed: %s", esp_err_to_name(result));
  return result;
}
#endif
