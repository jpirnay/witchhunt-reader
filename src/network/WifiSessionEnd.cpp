#include "WifiSessionEnd.h"

#include <HalCapabilities.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>

#include "HttpDownloader.h"
#include "SilentRestart.h"
#include "activities/reader/KOReaderAutoSync.h"
#if CROSSPOINT_KOREADER_AUTOSYNC
#include "activities/reader/KOReaderSyncWorker.h"
#endif

namespace wifi_session {
namespace {

constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
uint32_t bootFree = 0;
uint32_t bootLargest = 0;

// Off with the radio, unless it is off already: getMode() reads NULL only once WiFi.mode(WIFI_OFF) has run or when
// Wi-Fi never started (Arduino WiFiGeneric.cpp, getMode() and espWiFiStop()), so a bare esp_wifi_stop() still reads STA
// and is torn down here, and a radio already off skips wifiOff()'s 200 ms of delays.
void radioOff() {
  // SNTP whether or not the radio is still up: a site that turned the radio off some other way than wifiOff() would
  // leave the client running across the exit (HalClock::syncNtp()'s fallback stays enabled after a success).
  if (esp_sntp_enabled()) esp_sntp_stop();
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_NULL) return;
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) WiFi.softAPdisconnect(true);
  HalClock::wifiOff(/*skipNtpSync=*/true);  // SNTP stop, disconnect, WIFI_OFF (esp_wifi_deinit, netifs destroyed)
}

}  // namespace

void recordBootBaseline() {
  bootFree = heap_caps_get_free_size(kInternalCaps);
  bootLargest = heap_caps_get_largest_free_block(kInternalCaps);
}

Exit end(const char* tag) {
  if (deepSleepSupersedesRestart()) return Exit::Reboot;
#if CROSSPOINT_KOREADER_AUTOSYNC
  if (KOReaderSyncWorker::isBusy()) {
    LOG_INF("WIFI", "Session end [%s]: the background sync holds the radio; restarting", tag);
    return Exit::Reboot;
  }
#endif
  radioOff();
  HttpDownloader::forgetUnverifiableHosts();

  const uint32_t freeInternal = heap_caps_get_free_size(kInternalCaps);
  const uint32_t largestInternal = heap_caps_get_largest_free_block(kInternalCaps);
  const bool restartsAnyway = HalCapabilities::restartsAfterWifiSession();
  const Exit exit = verdict(restartsAnyway, freeInternal, largestInternal);
  if (exit == Exit::Reboot && !restartsAnyway) {
    LOG_ERR("WIFI",
            "Session end [%s]: internal free=%lu largest=%lu (boot %lu/%lu) under the minimums %lu/%lu; restarting",
            tag, static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(largestInternal),
            static_cast<unsigned long>(bootFree), static_cast<unsigned long>(bootLargest),
            static_cast<unsigned long>(kMinFreeInternalBytes), static_cast<unsigned long>(kMinLargestInternalBytes));
  } else {
    // Whether the secondary framebuffer is resident: the C3s gave it back to the heap before the session
    // (trimMemoryForNetworkSession()), so their free figure counts it (48,000 B on the X4, 52,272 B on the X3) where
    // the boot baseline does not.
    LOG_INF("WIFI", "Session end [%s]: internal free=%lu largest=%lu (boot %lu/%lu, min %lu/%lu, 2nd buffer %s) -> %s",
            tag, static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(largestInternal),
            static_cast<unsigned long>(bootFree), static_cast<unsigned long>(bootLargest),
            static_cast<unsigned long>(kMinFreeInternalBytes), static_cast<unsigned long>(kMinLargestInternalBytes),
            display.hasSecondaryBuffer() ? "held" : "not held", exit == Exit::CarryOn ? "carry on" : "restart");
  }
  return exit;
}

}  // namespace wifi_session
