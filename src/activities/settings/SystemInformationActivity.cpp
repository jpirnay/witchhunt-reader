#include "SystemInformationActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SystemStatus.h"
#include "components/UITheme.h"
#include "fontIds.h"

static const char* pickUnit(uint64_t maxBytes, double& outDivisor) {
  if (maxBytes >= 1024ULL * 1024 * 1024) {
    outDivisor = 1024.0 * 1024.0 * 1024.0;
    return "GB";
  }
  if (maxBytes >= 1024ULL * 1024) {
    outDivisor = 1024.0 * 1024.0;
    return "MB";
  }
  if (maxBytes >= 1024ULL) {
    outDivisor = 1024.0;
    return "KB";
  }
  outDivisor = 1.0;
  return "B";
}

static std::string formatBytes(uint64_t bytes) {
  double div;
  const char* unit = pickUnit(bytes, div);
  char buf[16];
  if (div == 1.0) {
    snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(bytes), unit);
  } else {
    snprintf(buf, sizeof(buf), "%.1f %s", bytes / div, unit);
  }
  return buf;
}

// Format three byte values on a single line sharing one trailing unit. The
// unit is chosen from the largest of the three so all values fit sensibly.
static std::string formatBytesTriple(uint64_t a, uint64_t b, uint64_t c) {
  double div;
  const char* unit = pickUnit(std::max({a, b, c}), div);
  char buf[48];
  if (div == 1.0) {
    snprintf(buf, sizeof(buf), "%llu / %llu / %llu %s", static_cast<unsigned long long>(a),
             static_cast<unsigned long long>(b), static_cast<unsigned long long>(c), unit);
  } else {
    snprintf(buf, sizeof(buf), "%.1f / %.1f / %.1f %s", a / div, b / div, c / div, unit);
  }
  return buf;
}

void SystemInformationActivity::onEnter() {
  Activity::onEnter();
  status_.reset();
  sdStatusReady_ = false;
  sdLoadRequested_ = false;
  requestUpdate();
}

void SystemInformationActivity::onExit() { Activity::onExit(); }

void SystemInformationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Collect fast fields first so this page appears immediately.
  if (!status_.has_value()) {
    status_ = SystemStatus::collectFast();
    requestUpdate();
    return;
  }

  // SD stats can be slower to compute on large cards. Load them only when the
  // user explicitly confirms.
  if (!sdStatusReady_) {
    if (sdLoadRequested_) {
      SystemStatus::fillSdStatus(*status_);
      sdStatusReady_ = true;
      sdLoadRequested_ = false;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      sdLoadRequested_ = true;
      requestUpdate();
    }
  }
}

void SystemInformationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false);

  renderer.clearScreen();

  GUI.drawHeader(renderer,
                 Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_SYSTEM_INFO), CROSSPOINT_VERSION);

  // Two-column layout with interleaved section headers (drawn via the theme's
  // subheader so the full-width underline is consistent with the rest of the
  // UI). Data rows use a bold label on the left and the value at the column
  // midpoint; row step is tightened so all sections fit on one screen.
  const int leftX = contentRect.x + metrics.verticalSpacing * 3;
  const int valueX = contentRect.x + contentRect.width / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowStep = lineH + 2;
  const int subHeaderHeight = lineH + 6;
  int y = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto drawSection = [&](const char* title) {
    GUI.drawSubHeader(renderer, Rect{contentRect.x, y, contentRect.width, subHeaderHeight}, title);
    y += subHeaderHeight + 2;
  };
  auto drawRow = [&](const char* label, const std::string& value) {
    renderer.drawText(UI_10_FONT_ID, leftX, y, label, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, valueX, y, value.c_str());
    y += rowStep;
  };

  if (!status_.has_value()) {
    // Stats not yet collected — show a placeholder so the screen updates immediately
    drawRow(tr(STR_FW_VERSION), CROSSPOINT_VERSION);
    y += rowStep;
    drawRow("", tr(STR_GATHERING_DATA));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto& status = *status_;

  drawSection(tr(STR_SEC_VERSION));
  drawRow(tr(STR_FW_VERSION), status.version);
  drawRow(tr(STR_DEVICE), std::string(status.deviceType) + " (" + std::to_string(status.displayWidth) + " x " +
                              std::to_string(status.displayHeight) + " px)");

  drawSection(tr(STR_SEC_CHIP));
  drawRow(tr(STR_CHIP), status.chipVersion);
  drawRow(tr(STR_CPU), std::to_string(status.cpuFreqMHz) + " " + tr(STR_MHZ));

  drawSection(tr(STR_SEC_MEMORY));
  drawRow(tr(STR_MEM_COMBINED),
          formatBytesTriple(status.freeHeapBytes, status.minFreeHeapBytes, status.maxAllocHeapBytes));

  drawSection(tr(STR_SEC_FLASH));
  drawRow(tr(STR_APP_PARTITION), formatBytes(status.flashAppPartitionSize));
  drawRow(tr(STR_FLASH_TOTAL), formatBytes(status.flashBytes));
  if (status.fontCacheTotalBytes > 0) {
    const std::string fontCacheValue = status.fontCacheUsedBytes > 0 ? formatBytes(status.fontCacheUsedBytes) + " / " +
                                                                           formatBytes(status.fontCacheTotalBytes)
                                                                     : "- / " + formatBytes(status.fontCacheTotalBytes);
    drawRow(tr(STR_FONT_CACHE), fontCacheValue);
  }

  drawSection(tr(STR_SEC_RUNTIME));
  const uint32_t h = status.uptimeSeconds / 3600;
  const uint32_t m = (status.uptimeSeconds % 3600) / 60;
  const uint32_t s = status.uptimeSeconds % 60;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%uh %02um %02us", h, m, s);
  drawRow(tr(STR_UPTIME), uptimeBuf);
  std::string batteryLabel = std::to_string(status.batteryPercent) + "%";
  if (status.charging) {
    batteryLabel += " (";
    batteryLabel += tr(STR_CHARGING);
    batteryLabel += ")";
  }
  drawRow(tr(STR_BATTERY), batteryLabel);

  drawSection(tr(STR_SEC_STORAGE));
  if (!sdStatusReady_) {
    const char* sdMessage = sdLoadRequested_ ? tr(STR_READING) : tr(STR_SD_UPDATE_PROMPT);
    drawRow(tr(STR_SD_CARD), sdMessage);
  } else if (status.sdTotalBytes > 0) {
    drawRow(tr(STR_SD_CARD), formatBytes(status.sdUsedBytes) + " / " + formatBytes(status.sdTotalBytes));
  } else {
    drawRow(tr(STR_SD_CARD), tr(STR_NOT_SET));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), sdStatusReady_ ? "" : tr(STR_UPDATE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
