#include "SystemInformationActivity.h"

#include <FlashLangPartition.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SystemStatus.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

// Set to 1 to add a "Declined" row breaking down why idle light sleep did not
// happen: the attempt count, then per-guard counters (lock / wifi / usb / front /
// dbnc / idf). A diagnostic for the idle-power path rather than something a reader
// needs, so it is off in normal builds; the counters behind it are collected
// either way in HalPowerManager::LightSleepStats, at ~44 bytes of .bss. Worth
// keeping switchable: this row is what identified the BQ27220 FC false positive
// that was silently disabling light sleep on X3 (see HalGPIO::isUsbElectricalConnected).
#define SHOW_SLEEP_DIAGNOSTICS 0

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

// The flash area, as technical values like the rest of this screen: "spiffs 3.4 MB" on the partition; otherwise
// where it is instead ("PSRAM", or "-" for none) and what the partition table holds ("spiffs 80.0 KB", too small for
// the language slot, or "spiffs -" for no `spiffs` partition at all).
static std::string flashAreaValue(const SystemStatus& status) {
  const std::string partition =
      "spiffs " + (status.flashPartitionBytes > 0 ? formatBytes(status.flashPartitionBytes) : std::string("-"));
  switch (status.flashArea) {
    case FlashArea::Backend::Partition:
      return partition;
    case FlashArea::Backend::PsramImage:
      return "PSRAM (" + partition + ")";
    case FlashArea::Backend::None:
      return "- (" + partition + ")";
  }
  return partition;
}

// The interface language's code, and when its pack is not in use, why: a code for a report rather than prose.
static std::string languageValue() {
  std::string value = I18N.getLanguageCode(I18N.getLanguage());
  const char* why = nullptr;
  switch (I18N.packState()) {
    case I18n::PackState::English:
    case I18n::PackState::Active:
      break;
    case I18n::PackState::NoPack:
      why = "no pack";
      break;
    case I18n::PackState::BadPack:
      why = "bad pack";
      break;
    case I18n::PackState::SlotFailed:
      switch (FlashLangPartition::lastFailure()) {
        case FlashLangPartition::Failure::NoArea:
          why = "no flash area";
          break;
        case FlashLangPartition::Failure::SlotTooSmall:
          why = "area too small";
          break;
        case FlashLangPartition::Failure::Build:
          why = "build";
          break;
        case FlashLangPartition::Failure::Map:
          why = "mmap";
          break;
        case FlashLangPartition::Failure::Corrupt:
          why = "CRC";
          break;
        case FlashLangPartition::Failure::None:
          why = "?";
          break;
      }
      break;
  }
  if (why) value = value + ": " + why;
  return value;
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
  page_ = 0;
  pageCount_ = 1;
  requestUpdate();
}

void SystemInformationActivity::onExit() { Activity::onExit(); }

void SystemInformationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (pageCount_ > 1) {
    const bool next = mappedInput.wasPressed(MappedInputManager::frontStripNext()) ||
                      mappedInput.wasPressed(MappedInputManager::Button::PageForward);
    const bool prev = mappedInput.wasPressed(MappedInputManager::frontStripPrevious()) ||
                      mappedInput.wasPressed(MappedInputManager::Button::PageBack);
    if (next || prev) {
      page_ = (page_ + (next ? 1 : pageCount_ - 1)) % pageCount_;
      requestUpdate();
      return;
    }
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

  auto drawHeader = [&](const char* subtitle) {
    GUI.drawHeader(renderer,
                   Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                   tr(STR_SYSTEM_INFO), subtitle);
  };

  // Two-column layout with interleaved section headers (drawn via the theme's
  // subheader so the full-width underline is consistent with the rest of the
  // UI). Data rows use a bold label on the left and the value at the column
  // midpoint.
  //
  // The rows are laid out as one continuous flow that breaks onto a new page
  // whenever the next section would run past the bottom, so the page count
  // follows the orientation and the rows present rather than a hand-kept split.
  // Every page is laid out on every render; only the rows of page_ are drawn.
  const int leftX = contentRect.x + metrics.verticalSpacing * 3;
  const int valueX = contentRect.x + contentRect.width / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowStep = lineH + 2;
  const int subHeaderHeight = lineH + 6;
  // The header always has a subtitle (the version, or "1 / 2"), so a page stops above its line.
  const Rect body = UITheme::getBodyRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/false, CROSSPOINT_VERSION);
  const int pageTop = body.y;
  const int pageBottom = body.y + body.height - metrics.verticalSpacing;
  int y = pageTop;
  int layoutPage = 0;

  auto breakIfShort = [&](int needed) {
    if (y + needed > pageBottom && y > pageTop) {
      ++layoutPage;
      y = pageTop;
    }
  };
  // `rows` keeps a section's header together with its rows; a section taller
  // than a page still breaks row by row in drawRow.
  auto drawSection = [&](const char* title, int rows) {
    breakIfShort(subHeaderHeight + 2 + rows * rowStep);
    if (layoutPage == page_) {
      GUI.drawSubHeader(renderer, Rect{contentRect.x, y, contentRect.width, subHeaderHeight}, title);
    }
    y += subHeaderHeight + 2;
  };
  auto drawRow = [&](const char* label, const std::string& value) {
    breakIfShort(rowStep);
    if (layoutPage == page_) {
      const int labelWidth = valueX - leftX - 4;
      const int valueWidth = contentRect.x + contentRect.width - valueX;
      renderer.drawText(UI_10_FONT_ID, leftX, y,
                        renderer.truncatedText(UI_10_FONT_ID, label, labelWidth, EpdFontFamily::BOLD).c_str(), true,
                        EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, valueX, y,
                        renderer.truncatedText(UI_10_FONT_ID, value.c_str(), valueWidth).c_str());
    }
    y += rowStep;
  };

  if (!status_.has_value()) {
    // Stats not yet collected — show a placeholder so the screen updates immediately
    pageCount_ = 1;
    drawHeader(CROSSPOINT_VERSION);
    drawRow(tr(STR_FW_VERSION), CROSSPOINT_VERSION);
    y += rowStep;
    drawRow("", tr(STR_GATHERING_DATA));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto& status = *status_;

  drawSection(tr(STR_SEC_VERSION), 5);
  drawRow(tr(STR_FW_VERSION), status.version);
  drawRow(tr(STR_DISPLAY_SDK), status.displaySdk);
  drawRow(tr(STR_DEVICE), std::string(status.deviceType) + " (" + std::to_string(status.displayWidth) + " x " +
                              std::to_string(status.displayHeight) + " px)");
  drawRow(tr(STR_DISPLAY_CONTROLLER), status.displayController);
  // The profile, not just "X3"/"X4": the per-batch panel variants select different
  // sibling profiles, and that distinction is what makes two field reports comparable.
  drawRow(tr(STR_DIAG_BOARD_PROFILE), status.boardProfile);

  drawSection(tr(STR_SEC_CHIP), 2);
  drawRow(tr(STR_CHIP), status.chipVersion);
  drawRow(tr(STR_CPU), std::to_string(status.cpuFreqMHz) + " " + tr(STR_MHZ));

  drawSection(tr(STR_SEC_MEMORY), 1);
  drawRow(tr(STR_MEM_COMBINED),
          formatBytesTriple(status.freeHeapBytes, status.minFreeHeapBytes, status.maxAllocHeapBytes));

  const bool hasArea = status.flashArea != FlashArea::Backend::None;
  drawSection(tr(STR_SEC_FLASH), hasArea ? 5 : 4);
  drawRow(tr(STR_APP_PARTITION), formatBytes(status.flashAppPartitionSize));
  drawRow(tr(STR_FLASH_TOTAL), formatBytes(status.flashBytes));
  // What a report of an English interface (with its pack on the card) or of TrueType fonts that do not load needs
  // first: where the flash area is, the partition table's `spiffs` partition, and why the language pack is not in use.
  drawRow(tr(STR_FLASH_AREA), flashAreaValue(status));
  if (hasArea) {
    drawRow(tr(STR_FONT_CACHE), (status.fontCacheUsedBytes > 0 ? formatBytes(status.fontCacheUsedBytes) : "-") +
                                    std::string(" / ") + formatBytes(status.fontCacheTotalBytes));
  }
  drawRow(tr(STR_LANGUAGE), languageValue());

  // Uptime, light sleep, battery, and deep sleep when known.
  drawSection(tr(STR_SEC_RUNTIME), status.deepSleepSeconds > 0 ? 4 : 3);
  const uint32_t h = status.uptimeSeconds / 3600;
  const uint32_t m = (status.uptimeSeconds % 3600) / 60;
  const uint32_t s = status.uptimeSeconds % 60;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%uh %02um %02us", h, m, s);
  drawRow(tr(STR_UPTIME), uptimeBuf);

  // Power behaviour. Light sleep is the share of uptime the chip was actually
  // halted between input polls — the readable stand-in for average current, since
  // the CDC guard means it can never be observed over a serial monitor. Deep sleep
  // is the length of the sleep this boot woke from, blank when unknowable.
  char sleepBuf[32];
  snprintf(sleepBuf, sizeof(sleepBuf), "%um %02us (%u%%)", status.lightSleepSeconds / 60, status.lightSleepSeconds % 60,
           status.lightSleepPercent);
  drawRow(tr(STR_LIGHT_SLEEP), sleepBuf);

#if SHOW_SLEEP_DIAGNOSTICS
  // Why it did not sleep. Only non-zero reasons are listed, so a healthy device
  // shows a short line rather than a wall of zeroes. "0 tries" is the meaningful
  // distinct case: the idle branch was never reached, so no guard is to blame.
  {
    const auto& raw = status.lightSleepRaw;
    char buf[24];
    snprintf(buf, sizeof(buf), "%u tries", raw.attempts);
    drawRow(tr(STR_SLEEP_DECLINED), buf);

    // Reasons go on their own continuation rows (blank label), wrapped to the
    // buffer width: the value column is only half the screen, so packing them
    // onto the row above truncated the list exactly when several guards had
    // fired and the detail mattered most. Healthy devices list nothing here.
    const struct {
      const char* name;
      uint32_t count;
    } reasons[] = {{"lock", raw.rejLock},        {"wifi", raw.rejWifi},     {"usb", raw.rejUsb},
                   {"front", raw.rejFrontlight}, {"dbnc", raw.rejDebounce}, {"idf", raw.rejIdf}};
    int n = 0;
    buf[0] = '\0';
    for (const auto& r : reasons) {
      if (r.count == 0) {
        continue;
      }
      char item[16];
      const int len = snprintf(item, sizeof(item), "%s %u", r.name, r.count);
      if (n > 0 && n + 2 + len >= static_cast<int>(sizeof(buf))) {
        drawRow("", buf);  // flush the full line and start the next
        n = 0;
        buf[0] = '\0';
      }
      n += snprintf(buf + n, sizeof(buf) - n, "%s%s", n > 0 ? ", " : "", item);
    }
    if (n > 0) {
      drawRow("", buf);
    }
  }
#endif  // SHOW_SLEEP_DIAGNOSTICS

  if (status.deepSleepSeconds > 0) {
    const uint32_t dh = status.deepSleepSeconds / 3600;
    const uint32_t dm = (status.deepSleepSeconds % 3600) / 60;
    const uint32_t ds = status.deepSleepSeconds % 60;
    snprintf(sleepBuf, sizeof(sleepBuf), "%uh %02um %02us", dh, dm, ds);
    drawRow(tr(STR_DEEP_SLEEP), sleepBuf);
  }

  std::string batteryLabel = std::to_string(status.batteryPercent) + "%";
  if (status.charging) {
    batteryLabel += " (";
    batteryLabel += tr(STR_CHARGING);
    batteryLabel += ")";
  }
  drawRow(tr(STR_BATTERY), batteryLabel);

  drawSection(tr(STR_SEC_STORAGE), 1);
  if (!sdStatusReady_) {
    const char* sdMessage = sdLoadRequested_ ? tr(STR_READING) : tr(STR_SD_UPDATE_PROMPT);
    drawRow(tr(STR_SD_CARD), sdMessage);
  } else if (status.sdTotalBytes > 0) {
    drawRow(tr(STR_SD_CARD), formatBytes(status.sdUsedBytes) + " / " + formatBytes(status.sdTotalBytes));
  } else {
    drawRow(tr(STR_SD_CARD), tr(STR_NOT_SET));
  }

  pageCount_ = layoutPage + 1;
  const bool paged = pageCount_ > 1;
  // With more than one page the header's subtitle says "1 / 2" instead of the
  // version, which is the first row anyway: a reader has to know there IS a
  // second page before they will look for the way to reach it.
  const std::string pageOfPages = std::to_string(page_ + 1) + " / " + std::to_string(pageCount_);
  drawHeader(paged ? pageOfPages.c_str() : CROSSPOINT_VERSION);

  // Draw logo centered horizontally, vertically centered in the space between
  // the last data row and the button hints -- on the last page only, where y is
  // still the cursor of the page on screen. drawImage handles coordinate
  // transformation internally so plain content-rect coordinates are used here.
  constexpr int kLogoSize = 120;
  const int hintsTop = contentRect.y + contentRect.height - metrics.buttonHintsHeight;
  const int logoY = y + (hintsTop - y - kLogoSize) / 2;
  const int logoX = contentRect.x + (contentRect.width - kLogoSize) / 2;
  if (layoutPage == page_ && logoY >= 0 && logoY + kLogoSize <= hintsTop) {
    renderer.drawImage(Logo120, logoX, logoY, kLogoSize, kLogoSize);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), sdStatusReady_ ? "" : tr(STR_UPDATE),
                                            paged ? tr(STR_PREV) : "", paged ? tr(STR_NEXT) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
