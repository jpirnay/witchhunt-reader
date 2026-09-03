#pragma once

#include <Arduino.h>
#include <BoardConfig.h>
#include <FlashFontPartition.h>
#include <HalCapabilities.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "HalClock.h"
#include "HalGPIO.h"
#include "HalPowerManager.h"

// Human-readable name for the panel controller silicon actually resolved at
// boot (BoardConfig::ACTIVE.displayController) — reflects per-batch swaps
// like UC8253 -> UC8279d or SSD1677 -> UC8179 (see HalGPIO::begin() /
// freeink::applyXteinkDisplayController).
inline const char* displayControllerName(BoardConfig::DisplayController controller) {
  switch (controller) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LovyanGFX";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
  }
  return "Unknown";
}

// Display/hardware SDK identity, injected by scripts/git_branch.py from the
// EInkDisplay lib_dep (e.g. "FreeInk 61aa2aa"). Fallback keeps builds compiling
// if the pre-script could not resolve it.
#ifndef CROSSPOINT_DISPLAY_SDK
#define CROSSPOINT_DISPLAY_SDK "unknown"
#endif

// Snapshot of device system status, shared between the web server and the
// System Information activity so both surfaces show consistent data.
struct SystemStatus {
  const char* version;
  const char* displaySdk;  // display/hardware SDK name + version (CROSSPOINT_DISPLAY_SDK)
  const char* deviceType;  // display name of the active board, e.g. "X4", "X4 Pro", "T5 S3 Pro"
  // Exact BoardProfile selected at boot, e.g. "xteink_x4" / "xteink_x3" /
  // "xteink_x3_uc8279" / "xteink_x4_pro". deviceType alone is not enough to identify a
  // unit: each model ships in more than one silicon variant (per-batch panel swaps), and
  // two bug reports are only comparable once it is known whether they came from the same
  // one. It is also the only field that separates two boards sharing a display name.
  const char* boardProfile;
  const char* displayController;  // panel controller silicon resolved at boot, e.g. "UC8253"
  uint16_t displayWidth;          // Native panel width in pixels (long edge)
  uint16_t displayHeight;         // Native panel height in pixels (short edge)
  std::string chipVersion;
  uint32_t cpuFreqMHz;
  std::string ip;
  std::string wifiMode;  // "STA", "AP", or "Off"
  int rssi;              // dBm; 0 when not in STA mode
  std::string macAddress;
  uint32_t freeHeapBytes;
  uint32_t minFreeHeapBytes;
  uint32_t maxAllocHeapBytes;
  uint64_t flashBytes;             // Total flash chip size
  uint64_t flashAppPartitionSize;  // Size of the running OTA app partition
  uint16_t batteryPercent;
  bool charging;
  uint32_t uptimeSeconds;
  uint64_t sdTotalBytes;
  uint64_t sdUsedBytes;
  uint64_t sdFreeBytes;
  uint64_t fontCacheUsedBytes;   // bytes used in the flash font partition (0 if empty)
  uint64_t fontCacheTotalBytes;  // total size of the flash font partition
  // Power behaviour since boot. lightSleepSeconds is time the chip was actually
  // halted by the idle light-sleep path; lightSleepPercent is that as a share of
  // uptime, which is the readable proxy for average current. deepSleepSeconds is
  // the length of the sleep this boot woke from (0 when it cannot be established
  // — see HalClock::lastSleepSeconds).
  uint32_t lightSleepSeconds;
  uint8_t lightSleepPercent;
  uint32_t lightSleepSlices;
  uint32_t deepSleepSeconds;
  // Why light sleep did not happen. `attempts` separates the two failure modes a
  // bare 0% cannot: zero attempts means the idle branch was never reached at all
  // (input kept resetting the timer, or an activity held skipLoopDelay), whereas
  // attempts with a decline count names the guard that turned it down.
  HalPowerManager::LightSleepStats lightSleepRaw;

  static SystemStatus collectFast() {
    SystemStatus s;
    s.version = CROSSPOINT_VERSION;
    s.displaySdk = CROSSPOINT_DISPLAY_SDK;
    // From the active board profile, not a deviceIsX3() ternary. That ternary
    // reported every non-X3 board as "X4 (800 x 480)" -- correct on the C3 pair
    // it was written for, wrong on every S3 board, where deviceIsX3() is false by
    // construction. Observed on the T5S3, which is a 960x540 LilyGo.
    //
    // ACTIVE is the right source on the C3 too: HalGPIO::begin() runs the X3/X4
    // fingerprint and calls BoardConfig::selectDevice(), so the profile already
    // reflects the detected device (including the UC8279 X3 variant) by the time
    // this is read.
    s.deviceType = HalCapabilities::boardDisplayName(BoardConfig::ACTIVE.board);
    s.displayWidth = BoardConfig::ACTIVE.displayWidth;
    s.displayHeight = BoardConfig::ACTIVE.displayHeight;
    // The build slug beside the display name: two profiles can share a name (the X3's
    // UC8253 and UC8279 variants both read "X3") and only this tells them apart.
    s.boardProfile = BoardConfig::ACTIVE.name;
    s.displayController = displayControllerName(BoardConfig::ACTIVE.displayController);
    s.chipVersion = ESP.getChipModel();
    s.chipVersion += " rev ";
    s.chipVersion += std::to_string(ESP.getChipRevision());
    s.cpuFreqMHz = static_cast<uint32_t>(getCpuFrequencyMhz());
    s.freeHeapBytes = ESP.getFreeHeap();
    s.minFreeHeapBytes = ESP.getMinFreeHeap();
    s.maxAllocHeapBytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    s.flashBytes = static_cast<uint64_t>(ESP.getFlashChipSize());
    // ESP.getSketchSize() is unreliable on custom partition layouts (the
    // underlying esp_image_verify() call silently fails), so we report the
    // running OTA partition capacity instead — a firm, measurable number.
    const esp_partition_t* running = esp_ota_get_running_partition();
    s.flashAppPartitionSize = running ? static_cast<uint64_t>(running->size) : 0;
    s.batteryPercent = powerManager.getBatteryPercentage();
    // Route through the HAL rather than reading UART0_RXD directly: that pin is
    // the X4-only USB detect, and on X3 it is repurposed as I2C SDA, where the
    // bus pull-ups made this read HIGH (= "charging") permanently.
    s.charging = gpio.isUsbConnected();
    s.uptimeSeconds = millis() / 1000;
    const auto& sleepStats = powerManager.lightSleepStats();
    s.lightSleepRaw = sleepStats;
    s.lightSleepSeconds = sleepStats.sleptMs / 1000;
    s.lightSleepSlices = sleepStats.slept;
    // Against uptime, not against sleptMs+awakeMs: awakeMs only accumulates
    // between slices, so the ratio against it would ignore all the time the
    // device spent reading, rendering or below the idle threshold — and read as
    // a far higher sleep share than the battery actually sees.
    const uint32_t uptimeMs = millis();
    s.lightSleepPercent = uptimeMs > 0 ? static_cast<uint8_t>((sleepStats.sleptMs * 100ULL) / uptimeMs) : 0;
    s.deepSleepSeconds = HalClock::lastSleepSeconds();
    s.macAddress = WiFi.macAddress().c_str();
    s.sdTotalBytes = 0;
    s.sdUsedBytes = 0;
    s.sdFreeBytes = 0;

    // Flash font partition — size always available; used bytes from CPFC index
    // if a valid cache is present, otherwise 0.
    {
      const esp_partition_t* part =
          esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
      s.fontCacheTotalBytes = part ? static_cast<uint64_t>(part->size) : 0;
      s.fontCacheUsedBytes = 0;
      if (part && FlashFontPartition::hasValidIndex()) {
        // Read the index to sum up all entry data sizes.
        // Re-use the public query API to avoid duplicating partition read logic.
        // We map the CPFC header (8 bytes + up to 16 × 48-byte entries = 776 B).
        uint8_t buf[8 + 16 * 48];
        if (esp_partition_read(part, 0, buf, sizeof(buf)) == ESP_OK && buf[0] == 'C' && buf[1] == 'P' &&
            buf[2] == 'F' && buf[3] == 'C') {
          const uint8_t count = buf[4];
          uint64_t usedTotal = 8 + static_cast<uint64_t>(count) * 48;  // header + index
          for (uint8_t i = 0; i < count && i < 16; i++) {
            const uint8_t* entry = buf + 8 + i * 48;
            const uint32_t dataSize = static_cast<uint32_t>(entry[40]) | (static_cast<uint32_t>(entry[41]) << 8) |
                                      (static_cast<uint32_t>(entry[42]) << 16) |
                                      (static_cast<uint32_t>(entry[43]) << 24);
            usedTotal += dataSize;
          }
          s.fontCacheUsedBytes = usedTotal;
        }
      }
    }

    const wifi_mode_t mode = WiFi.getMode();
    const bool isAP = (mode == WIFI_MODE_AP) || (mode == WIFI_MODE_APSTA);

    if (isAP) {
      s.wifiMode = "AP";
      s.ip = WiFi.softAPIP().toString().c_str();
      s.rssi = 0;
    } else if (WiFi.status() == WL_CONNECTED) {
      s.wifiMode = "STA";
      s.ip = WiFi.localIP().toString().c_str();
      s.rssi = WiFi.RSSI();
    } else {
      s.wifiMode = "Off";
      s.ip = "-";
      s.rssi = 0;
    }

    return s;
  }

  static void fillSdStatus(SystemStatus& s) {
    uint32_t t0 = millis();
    s.sdTotalBytes = Storage.sdTotalBytes();
    s.sdUsedBytes = Storage.sdUsedBytes();
    s.sdFreeBytes = Storage.sdFreeBytes();
    LOG_DBG("SYSINFO", "Filled SD status in %u ms", millis() - t0);
  }

  static SystemStatus collect() {
    SystemStatus s = collectFast();
    fillSdStatus(s);
    return s;
  }
};
