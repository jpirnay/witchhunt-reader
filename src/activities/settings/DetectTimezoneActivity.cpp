#include "DetectTimezoneActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <JsonText.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/WifiSessionEnd.h"
#include "util/TimezoneDetectMap.h"
#include "util/Timezones.h"

namespace {

using timezone_detect::mapIanaTimezone;

bool fetchTimezonePayload(const char* url, std::string& payload) {
  // Give DNS a moment after WiFi connect.
  delay(500);
  for (int attempt = 0; attempt < 2; ++attempt) {
    // Public IP/timezone lookup with no credentials. Unverified: see the
    // TlsPolicy notes in HttpDownloader.h. The answer only seeds the timezone
    // SETTING, which the user sees and can correct.
    if (HttpDownloader::fetchUrl(url, payload, "", "", HttpDownloader::TlsPolicy::Unverified)) {
      return true;
    }
    delay(300);
  }
  return false;
}

bool fetchPublicIp(std::string& ip) {
  std::string payload;
  if (!fetchTimezonePayload("https://api.ipify.org", payload)) {
    return false;
  }

  // Payload is plain text: IP address
  ip = payload;
  // Trim any whitespace
  while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' ' || ip.back() == '\t')) {
    ip.pop_back();
  }
  if (!ip.empty()) {
    LOG_DBG("CLK", "Public IP: %s", ip.c_str());
    return true;
  }
  return false;
}

bool detectTimezoneSetting(uint8_t& outSetting, std::string& outIana, bool& outDstKnown, bool& outDstActive) {
  std::string payload;
  std::string publicIp;
  if (fetchPublicIp(publicIp)) {
    std::string timeUrl = std::string("https://timeapi.io/api/Time/current/ip?ipAddress=") + publicIp;
    LOG_DBG("CLK", "Timezone detect via TimeAPI: %s", timeUrl.c_str());
    if (fetchTimezonePayload(timeUrl.c_str(), payload)) {
      // Continue to parse TimeAPI payload below.
    }
  }

  if (payload.empty() && !fetchTimezonePayload("http://ip-api.com/json/?fields=timezone,dst", payload)) {
    LOG_ERR("CLK", "Timezone detect failed: fetch error");
    return false;
  }

  JsonDocument doc;
  const auto err = JsonText::parse(doc, payload);
  if (err) {
    LOG_ERR("CLK", "Timezone detect failed: %s", err.c_str());
    return false;
  }

  const char* tz = doc["timezone"] | "";
  if (!tz || tz[0] == '\0') {
    tz = doc["timeZone"] | "";
  }
  if (!tz || tz[0] == '\0') {
    tz = doc["time_zone"] | "";
  }
  if (!tz || tz[0] == '\0') {
    LOG_ERR("CLK", "Timezone detect failed: missing timezone (payload: %s)", payload.c_str());
    return false;
  }

  outIana = tz;
  if (!doc["dst_active"].isNull()) {
    outDstKnown = true;
    outDstActive = doc["dst_active"].as<bool>();
  } else if (!doc["dstActive"].isNull()) {
    outDstKnown = true;
    outDstActive = doc["dstActive"].as<bool>();
  } else if (!doc["dst"].isNull()) {
    outDstKnown = true;
    outDstActive = doc["dst"].as<bool>();
  } else {
    outDstKnown = false;
    outDstActive = false;
  }

  if (!mapIanaTimezone(tz, outSetting)) {
    LOG_ERR("CLK", "Timezone detect unsupported: %s", tz);
    return false;
  }

  if (outDstKnown) {
    LOG_DBG("CLK", "Timezone detected: %s (dst=%d)", tz, outDstActive ? 1 : 0);
  } else {
    LOG_DBG("CLK", "Timezone detected: %s (dst=unknown)", tz);
  }
  return true;
}

}  // namespace

void DetectTimezoneActivity::onEnter() {
  Activity::onEnter();

  // Free the heap the WiFi stack needs before it is brought up, not after -
  // association itself is the allocation-heavy step, well ahead of TLS. Matters
  // most here because SettingsActivity is still on the stack below us with its
  // per-category SettingInfo vectors resident, fragmenting the heap.
  trimMemoryForNetworkSession(renderer, "CLK");

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onWifiSelectionCancelled();
                             return;
                           }
                           onWifiSelectionComplete(true);
                         });
}

void DetectTimezoneActivity::onExit() {
  Activity::onExit();
  HalClock::wifiOff();
  // The C3s reboot; the PSRAM boards return to Clock settings in this boot while the heap allows.
  if (wifiWasUsed_ && wifi_session::end("TZ") == wifi_session::Exit::Reboot) {
    silentRestartToClockSettings();
  }
}

void DetectTimezoneActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    state = FAILED;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = DETECTING;
  }
  requestUpdateAndWait();

  performDetect();
}

// The join screen brought the radio up (station mode, a scan) before it was backed out of. That
// is a WiFi session like any other, so onExit() restarts into Clock settings, where Back leads,
// instead of turning the radio off and carrying on.
void DetectTimezoneActivity::onWifiSelectionCancelled() {
  wifiWasUsed_ = true;
  finish();
}

void DetectTimezoneActivity::performDetect() {
  wifiWasUsed_ = true;
  // Re-trim: the status screens rendered since onEnter can have repopulated the
  // font cache. Idempotent - the secondary buffer is already gone by now.
  trimMemoryForNetworkSession(renderer, "CLK");

  uint8_t detected = SETTINGS.clockTimezone;
  detectedTimezone.clear();
  dstKnown = false;
  dstActive = false;
  if (detectTimezoneSetting(detected, detectedTimezone, dstKnown, dstActive)) {
    SETTINGS.clockTimezone = detected;
    timezones::applyToClock();
    SETTINGS.saveToFile();
    state = SUCCESS;
  } else {
    state = FAILED;
  }

  HalClock::wifiOff();
  requestUpdate();
}

void DetectTimezoneActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int headerBottom = contentRect.y + metrics.topPadding + metrics.headerHeight;
  const Rect bodyRect = Rect(contentRect.x, headerBottom, contentRect.width,
                             contentRect.height - (metrics.topPadding + metrics.headerHeight));

  renderer.clearScreen();
  GUI.drawHeader(renderer,
                 Rect(contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight),
                 tr(STR_DETECT_TIMEZONE));

  if (state == CONNECTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, bodyRect.y + bodyRect.height / 2, tr(STR_CONNECTING), true,
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == DETECTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, bodyRect.y + bodyRect.height / 2, tr(STR_DETECTING_TIMEZONE), true,
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, bodyRect.y + bodyRect.height / 2 - 20, tr(STR_TIMEZONE_DETECTED), true,
                              EpdFontFamily::BOLD);

    if (!detectedTimezone.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, bodyRect.y + bodyRect.height / 2 + 5, detectedTimezone.c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, bodyRect.y + bodyRect.height / 2 + 25, dstStatusLabel());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, bodyRect.y + bodyRect.height / 2, tr(STR_TIMEZONE_DETECT_FAILED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

const char* DetectTimezoneActivity::dstStatusLabel() const {
  if (!dstKnown) {
    return tr(STR_DST_UNKNOWN);
  }
  return dstActive ? tr(STR_DST_ACTIVE) : tr(STR_DST_INACTIVE);
}

void DetectTimezoneActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}
