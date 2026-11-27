#include "HalBoardSupport.h"

#include <BoardConfig.h>
#include <HalCapabilities.h>
#include <HalI2cBus.h>
#include <Logging.h>

#if BOARD_SUPPORT_OWNS_BUSES
#include <BoardT5S3.h>
#endif

#if FREEINK_CAP_HAPTIC
#include <HapticManager.h>
#endif

#if FREEINK_DEVICE_METALIO_EINK4_PLUS
#include <I2cBusLock.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <wolfssl/wolfcrypt/pwdbased.h>
#include <wolfssl/wolfcrypt/settings.h>

namespace {
constexpr char kWitchNvs[] = "witch_nvs";
RTC_NOINIT_ATTR uint32_t metalioRejectedWakes;

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

void sleepOnExpanderKey() {
  constexpr gpio_num_t wakePin = freeink::metalioplus::EXPANDER_INT_GPIO;
  if (esp_sleep_is_valid_wakeup_gpio(wakePin) && freeink::metalioplus::prepareKeyWake()) {
    // The open-drain INT needs its RTC pull-up powered for the entire sleep.
    freeink::PowerManager::armWakeOnPins(1ULL << wakePin, /*wakeLow=*/true);
    LOG_INF("PWR", "Metalio standby: expander wake armed");
    freeink::PowerManager::deepSleep();
  }
  LOG_INF("PWR", "Metalio standby: polling power key for wake");
  freeink::metalioplus::waitForPowerKeyThenRestart();
}
}  // namespace

extern "C" esp_err_t __wrap_nvs_flash_init() {
  esp_err_t result = nvs_flash_init_partition(kWitchNvs);
  // Arduino handles these two errors by erasing the default "nvs" partition.
  // Erase only our private partition so the other firmware's NVS stays untouched.
  if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    if (nvs_flash_erase_partition(kWitchNvs) == ESP_OK) {
      result = nvs_flash_init_partition(kWitchNvs);
    }
  }
  return result;
}

extern "C" esp_err_t __wrap_nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle) {
  return nvs_open_from_partition(kWitchNvs, name, mode, handle);
}

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);

extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  // WitchHunt saves credentials on SD. Keep the Wi-Fi driver's settings in RAM
  // so initialization works without another firmware's NVS partition.
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

namespace HalBoardSupport {

void earlyBoot() {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && !freeink::metalioplus::probePowerKeyHeldEarly()) {
    ++metalioRejectedWakes;
    sleepOnExpanderKey();
  }
  WiFi.persistent(false);
#endif
}

void begin() {
#if BOARD_SUPPORT_OWNS_BUSES
  // LilyGo T5S3 board-support owns the shared I2C bring-up, SD SPI bus CS deselects,
  // and the PCA9535 expander button hook. Must run before HalGPIO::begin().
  BoardT5S3::begin();
  HalI2cBus::begin();
  HalI2cBus::adoptMutex(BoardT5S3::i2cMutexHandle());
#elif FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Share one recursive I2C mutex across HalI2cBus (touch/RTC/gauge) and the SDK's
  // freeink::I2cBusLock (TPS65185 PMIC, TCA9555 expander, and haptic motor).
  HalI2cBus::begin();
  freeink::enableI2cBusLock();
  HalI2cBus::adoptMutex(freeink::i2cBusMutex());
#endif
}

void onSerialReady() {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    LOG_INF("PWR", "Metalio deep sleep: ignored %lu shared interrupts",
            static_cast<unsigned long>(metalioRejectedWakes));
  }
#endif
}

void prepareForDeepSleep() {
#if FREEINK_CAP_HAPTIC
  if (HalCapabilities::hasHaptics()) {
    HapticManager::getInstance().stop();
  }
#endif
}

bool allowsLightSleep() {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // The S31 core in this toolchain can corrupt PSRAM on light-sleep wake.
  return false;
#else
  return true;
#endif
}

bool enterDeepSleep() {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  // Keep the board powered so the expander can wake it on a Power press.
  metalioRejectedWakes = 0;
  sleepOnExpanderKey();
  return true;
#else
  return false;
#endif
}

bool overrideWakeupReason(HalGPIO::WakeupReason& out) {
#if FREEINK_DEVICE_METALIO_EINK4_PLUS
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_DEEPSLEEP) {
    out = HalGPIO::WakeupReason::PowerButton;
    return true;
  }
#else
  (void)out;
#endif
  return false;
}

}  // namespace HalBoardSupport
