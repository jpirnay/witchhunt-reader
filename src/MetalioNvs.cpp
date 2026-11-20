#include <BoardConfig.h>

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include <Logging.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <wolfssl/wolfcrypt/pwdbased.h>
#include <wolfssl/wolfcrypt/settings.h>

namespace {
constexpr char kWitchNvs[] = "witch_nvs";

struct WpaKeyJob {
  const uint8_t* password;
  int passwordLength;
  const uint8_t* ssid;
  int ssidLength;
  uint8_t* output;
  SemaphoreHandle_t done;
  int result = -1;
};

void deriveWpaKey(void* argument) {
  auto& job = *static_cast<WpaKeyJob*>(argument);
  job.result =
      wc_PBKDF2(job.output, job.password, job.passwordLength, job.ssid, job.ssidLength, 4096, 32, WC_HASH_TYPE_SHA);
  xSemaphoreGive(job.done);
  vTaskDelete(nullptr);
}
}  // namespace

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

extern "C" int __wrap_esp_fast_psk(const char* password, size_t passwordLength, const uint8_t* ssid, size_t ssidLength,
                                   size_t iterations, uint8_t* output, size_t outputLength) {
  // S31 faults on this board in both IDF SHA paths and in software SHA on the Wi-Fi task.
  // Keep the WPA parameters and run software derivation on a separate stack.
  if (!password || !ssid || !output || iterations != 4096 || outputLength != 32 || passwordLength > 63 ||
      ssidLength > 32) {
    return -1;
  }
  StaticSemaphore_t doneStorage;
  WpaKeyJob job{reinterpret_cast<const uint8_t*>(password),
                static_cast<int>(passwordLength),
                ssid,
                static_cast<int>(ssidLength),
                output,
                xSemaphoreCreateBinaryStatic(&doneStorage)};
  constexpr uint32_t stackBytes = 6144;
  if (xTaskCreate(deriveWpaKey, "metalio-wpa", stackBytes, &job, tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    LOG_ERR("WIFI", "Could not start Metalio WPA key derivation");
    vSemaphoreDelete(job.done);
    return -1;
  }
  xSemaphoreTake(job.done, portMAX_DELAY);
  const int result = job.result;
  vSemaphoreDelete(job.done);
  if (result != 0) LOG_ERR("WIFI", "Metalio WPA key derivation failed: %d", result);
  return result == 0 ? 0 : -1;
}
#endif
