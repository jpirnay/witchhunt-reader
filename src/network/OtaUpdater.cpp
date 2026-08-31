#include "OtaUpdater.h"

#include <Arduino.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>

#include <cstdio>
#include <cstring>

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before the ESP-IDF headers below (esp_wifi.h and
// friends transitively include lwip). Pin this order; clang-format would
// otherwise sort the local headers last and break the build.
#include "CrossPointSettings.h"
#include "HttpDownloader.h"
#include <bootloader_common.h>
#include <esp_flash_partitions.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_wifi.h>
// clang-format on

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases/latest";
constexpr char releaseListUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases?per_page=1";
constexpr int otaHttpMaxAttempts = 3;
constexpr unsigned long otaInitialRetryDelayMs = 1000;
constexpr size_t releaseMetadataMaxBytes = 128 * 1024;

const char* getReleaseApiUrl() { return SETTINGS.includeBetaUpdates ? releaseListUrl : latestReleaseUrl; }

void delayBeforeRetry(const char* operation, int attempt) {
  const unsigned long delayMs = otaInitialRetryDelayMs << static_cast<unsigned int>(attempt - 1);
  LOG_ERR("OTA", "%s failed on attempt %d/%d, retrying in %lu ms", operation, attempt, otaHttpMaxAttempts, delayMs);
  delay(delayMs);
}

struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~WifiPowerSaveGuard() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  render = false;

  const char* releaseApiUrl = getReleaseApiUrl();

  // Keep WiFi out of modem-sleep while doing release metadata HTTPS I/O.
  // This mirrors installUpdate() and reduces intermittent TLS read stalls.
  WifiPowerSaveGuard wifiPowerSaveGuard;

  // Use one streaming parser path for both stable and beta update checks to
  // avoid holding full GitHub release JSON in memory.
  // Adapted from crosspoint-reader/crosspoint-reader (MIT),
  // PR #1810 by znelson and contributors.
  LOG_DBG("OTA", "Checking for %s update at %s", SETTINGS.includeBetaUpdates ? "beta" : "stable", releaseApiUrl);
  for (int attempt = 1; attempt <= otaHttpMaxAttempts; ++attempt) {
    ReleaseJsonParser releaseParser;
    size_t bytesSeen = 0;
    // Fail closed like the firmware download itself: this response names the URL
    // installUpdate() then fetches, so an unverified answer chooses the binary.
    const bool ok = HttpDownloader::fetchUrlVerified(
        releaseApiUrl,
        [&releaseParser, &bytesSeen](const uint8_t* data, size_t len) {
          bytesSeen += len;
          if (bytesSeen > releaseMetadataMaxBytes) {
            return false;
          }
          releaseParser.feed(reinterpret_cast<const char*>(data), len);
          // For OTA metadata we only need tag_name and firmware.bin fields.
          // Stop early once both are found to avoid fragile tail reads.
          if (releaseParser.foundTag() && releaseParser.foundFirmware()) {
            return false;
          }
          return true;
        },
        true);

    if (!ok) {
      if (bytesSeen > releaseMetadataMaxBytes) {
        LOG_ERR("OTA", "Release metadata too large after %zu bytes", bytesSeen);
        return METADATA_TOO_LARGE_ERROR;
      }

      LOG_ERR("OTA", "Release metadata stream failed on attempt %d/%d", attempt, otaHttpMaxAttempts);
      if (attempt < otaHttpMaxAttempts) {
        delayBeforeRetry("Release metadata stream", attempt);
        continue;
      }
      return HTTP_ERROR;
    }

    if (!releaseParser.foundTag()) {
      LOG_ERR("OTA", "No tag_name found in release metadata");
      return JSON_PARSE_ERROR;
    }

    if (!releaseParser.foundFirmware()) {
      LOG_ERR("OTA", "No firmware.bin asset found");
      return NO_UPDATE;
    }

    latestVersion = releaseParser.getTagName();
    otaUrl = releaseParser.getFirmwareUrl();
    otaSize = releaseParser.getFirmwareSize();
    totalSize = otaSize;
    updateAvailable = true;

    LOG_DBG("OTA", "Found %s update: %s", SETTINGS.includeBetaUpdates ? "beta" : "stable", latestVersion.c_str());
    return OK;
  }

  return HTTP_ERROR;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor = 0, currentMinor = 0, currentPatch = 0, currentBetaRelease = 0, currentBetaBuild = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0, latestBetaRelease = 0, latestBetaBuild = 0;

  const auto currentVersion = CROSSPOINT_VERSION;
  const bool currentIsBeta = strstr(currentVersion, "-rc.") != nullptr;
  const bool latestIsBeta = latestVersion.find("-rc.") != std::string::npos;

  // Semantic version check with optional RC suffix. `sscanf()` will stop when
  // it reaches part of the input string that doesn't match the format, so this
  // format string works for versions like "1.31", "1.34.2", "1.35.0-rc.1", and
  // "1.36.0-rc.2.5".
  // This does not handle versions using the old "rc.<hash>" format, but
  // considering that people will need to manually install this release or later
  // to get this functionality anyway that should be fine.
  sscanf(latestVersion.c_str(), "%d.%d.%d-rc.%d.%d", &latestMajor, &latestMinor, &latestPatch, &latestBetaRelease,
         &latestBetaBuild);
  sscanf(currentVersion, "%d.%d.%d-rc.%d.%d", &currentMajor, &currentMinor, &currentPatch, &currentBetaRelease,
         &currentBetaBuild);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  /*
   * If we reach here, the stable version segments are equal. A stable release
   * is newer than an RC with the same version.
   */
  if (!latestIsBeta && currentIsBeta) {
    return true;
  }

  if (latestIsBeta && !currentIsBeta) {
    return false;
  }

  /*
   * If both versions are RCs, compare their RC release and build numbers.
   */
  if (latestIsBeta && currentIsBeta) {
    if (latestBetaRelease != currentBetaRelease) {
      return latestBetaRelease > currentBetaRelease;
    }
    if (latestBetaBuild != currentBetaBuild) {
      return latestBetaBuild > currentBetaBuild;
    }
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

void OtaUpdater::cleanupUpdate() {
  if (otaWriteHandle) {
    const esp_err_t err = esp_ota_abort(reinterpret_cast<esp_ota_handle_t>(otaWriteHandle));
    if (err != ESP_OK) {
      LOG_ERR("OTA", "esp_ota_abort on cleanup: %s", esp_err_to_name(err));
    }
    otaWriteHandle = nullptr;
  }
  cancelRequested = false;
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void OtaUpdater::cancelUpdate() {
  // If a streaming install is mid-flight, the DataCallback observes
  // cancelRequested and aborts; the step then cleans up. If not started yet,
  // just set the flag so the next step returns UPDATE_CANCELLED.
  cancelRequested = true;
}

OtaUpdater::OtaUpdaterError OtaUpdater::beginInstallUpdate() {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  cleanupUpdate();
  render = false;
  cancelRequested = false;
  installDone = false;
  installResult = OK;
  processedSize = 0;

  // Open the next OTA partition for streaming writes. The actual firmware
  // download runs in performInstallUpdateStep() via the wolfSSL HttpDownloader,
  // so no mbedtls esp_https_ota is involved.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    LOG_ERR("OTA", "no next OTA partition");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_ota_handle_t handle = 0;
  const esp_err_t err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &handle);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(err));
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return INTERNAL_UPDATE_ERROR;
  }
  otaWriteHandle = reinterpret_cast<void*>(handle);
  LOG_INF("OTA", "esp_ota_begin OK on %s; streaming from %s", updatePartition->label, otaUrl.c_str());
  return UPDATE_IN_PROGRESS;
}

/* Writes the otadata entry to boot from the most recently flashed OTA partition,
 * bypassing esp_ota_set_boot_partition()'s image_validate() call.
 * Used when esp_ota_end()/esp_ota_set_boot_partition() returns
 * ESP_ERR_OTA_VALIDATE_FAILED on unsigned Arduino builds (boot_comm efuse
 * revision check false-positive). */
int OtaUpdater::forceSetOtaBootPartition() {
  const esp_partition_t* newPartition = esp_ota_get_next_update_partition(nullptr);
  if (newPartition == nullptr) {
    LOG_ERR("OTA", "force boot partition: next update partition not found");
    return ESP_ERR_NOT_FOUND;
  }
  LOG_INF("OTA", "force boot next partition=%s subtype=0x%x offset=0x%lx size=0x%lx", newPartition->label,
          newPartition->subtype, static_cast<unsigned long>(newPartition->address),
          static_cast<unsigned long>(newPartition->size));

  const esp_partition_t* otaDataPartition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (otaDataPartition == nullptr) {
    LOG_ERR("OTA", "force boot partition: otadata partition not found");
    return ESP_ERR_NOT_FOUND;
  }
  esp_ota_select_entry_t otadata[2];
  esp_err_t err = esp_partition_read(otaDataPartition, 0, &otadata[0], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: read otadata[0] failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_partition_read(otaDataPartition, otaDataPartition->erase_size, &otadata[1], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: read otadata[1] failed: %s", esp_err_to_name(err));
    return err;
  }

  int activeSlot = bootloader_common_get_active_otadata(otadata);
  int nextSlot = (activeSlot == -1) ? 0 : (~activeSlot & 1);

  uint8_t otaAppCount = 0;
  while (esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + otaAppCount),
                                  nullptr) != nullptr) {
    otaAppCount++;
  }
  if (otaAppCount == 0) {
    LOG_ERR("OTA", "force boot select: no OTA app partitions found");
    return ESP_ERR_NOT_FOUND;
  }

  const uint8_t subTypeId = newPartition->subtype & 0x0F;
  uint32_t newSeq;
  if (activeSlot == -1) {
    newSeq = subTypeId + 1;
  } else {
    uint32_t currentSeq = otadata[activeSlot].ota_seq;
    newSeq = currentSeq;
    // ESP-IDF's bootloader maps ota_seq to an OTA app slot with
    // (ota_seq - 1) % ota_app_count. Match that mapping here so the forced
    // otadata entry selects the partition the streaming install just wrote.
    while ((newSeq - 1) % otaAppCount != static_cast<uint32_t>(subTypeId)) {
      newSeq++;
    }
    if (newSeq == currentSeq) newSeq += otaAppCount;
  }

  otadata[nextSlot].ota_seq = newSeq;
  otadata[nextSlot].ota_state = ESP_OTA_IMG_VALID;
  otadata[nextSlot].crc = bootloader_common_ota_select_crc(&otadata[nextSlot]);

  err = esp_partition_erase_range(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                                  otaDataPartition->erase_size);
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: erase otadata[%d] failed: %s", nextSlot, esp_err_to_name(err));
    return err;
  }

  err = esp_partition_write(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                            &otadata[nextSlot], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) {
    LOG_ERR("OTA", "force boot: write otadata[%d] failed: %s", nextSlot, esp_err_to_name(err));
  }
  return err;
}

OtaUpdater::OtaUpdaterError OtaUpdater::performInstallUpdateStep() {
  if (cancelRequested) {
    cleanupUpdate();
    return UPDATE_CANCELLED;
  }
  if (!otaWriteHandle) {
    return INTERNAL_UPDATE_ERROR;
  }
  // The whole streaming download happens in this single call; the DataCallback
  // drives progress/cancel via installProgressCb so the UI stays responsive.
  // Idempotent guard: once done, don't re-enter.
  if (installDone) {
    return installResult;
  }

  const esp_ota_handle_t handle = reinterpret_cast<esp_ota_handle_t>(otaWriteHandle);
  bool writeOk = true;
  bool userCancelled = false;

  // Verify-only (fail closed): a MITM must not be able to downgrade the firmware
  // download to an unverified connection by presenting a bad cert.
  const bool fetchOk = HttpDownloader::fetchUrlVerified(
      otaUrl,
      [&](const uint8_t* data, size_t len) -> bool {
        if (cancelRequested) {
          userCancelled = true;
          return false;
        }
        if (esp_ota_write(handle, data, len) != ESP_OK) {
          writeOk = false;
          return false;  // abort
        }
        processedSize += len;
        render = true;
        // Let the host redraw progress and poll for Back; abort if it returns false.
        if (installProgressCb && !installProgressCb(processedSize, totalSize)) {
          userCancelled = true;
          return false;
        }
        return true;
      },
      /*treatAbortAsSuccess=*/false);

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  installDone = true;

  if (userCancelled || cancelRequested) {
    LOG_INF("OTA", "Install cancelled by user");
    cleanupUpdate();
    installResult = UPDATE_CANCELLED;
    return installResult;
  }
  if (!writeOk) {
    LOG_ERR("OTA", "esp_ota_write failed");
    cleanupUpdate();
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }
  if (!fetchOk) {
    LOG_ERR("OTA", "firmware download failed");
    cleanupUpdate();
    installResult = HTTP_ERROR;
    return installResult;
  }

  esp_err_t finish_err = esp_ota_end(handle);
  otaWriteHandle = nullptr;
  if (finish_err != ESP_OK && finish_err != ESP_ERR_OTA_VALIDATE_FAILED) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(finish_err));
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }

  // Set the new partition as boot. Arduino unsigned builds fail image_validate()
  // in esp_ota_set_boot_partition (and esp_ota_end returns VALIDATE_FAILED), so
  // fall back to writing otadata directly — same bypass as before.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  esp_err_t boot_err = (finish_err == ESP_ERR_OTA_VALIDATE_FAILED) ? ESP_ERR_OTA_VALIDATE_FAILED
                                                                   : esp_ota_set_boot_partition(updatePartition);
  if (boot_err == ESP_ERR_OTA_VALIDATE_FAILED) {
    LOG_INF("OTA", "Validation failed (expected for unsigned Arduino builds) - forcing boot partition");
    boot_err = static_cast<esp_err_t>(forceSetOtaBootPartition());
    if (boot_err != ESP_OK) {
      LOG_ERR("OTA", "forceSetOtaBootPartition failed: %s", esp_err_to_name(boot_err));
      installResult = VALIDATE_FAILED;
      return installResult;
    }
  } else if (boot_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(boot_err));
    installResult = INTERNAL_UPDATE_ERROR;
    return installResult;
  }

  LOG_INF("OTA", "Update completed (%zu bytes)", processedSize);
  installResult = OK;
  return installResult;
}
