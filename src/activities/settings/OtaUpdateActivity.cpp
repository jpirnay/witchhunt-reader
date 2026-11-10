#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "FontDownloadActivity.h"
#include "FontInstaller.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"
#include "util/FontCatalog.h"
#include "util/FontDownloadRules.h"

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  // Re-trim: the status screens rendered since onEnter can have repopulated the
  // font cache. Idempotent - the secondary buffer is already gone by now.
  releaseMemoryForDownload(renderer, "OTA");

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      failureReason = res;
      state = FAILED;
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available (latest=%s, current=%s)", updater.getLatestVersion().c_str(),
            CROSSPOINT_VERSION);
    checkPacks();
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
  askToUpdate();
}

void OtaUpdateActivity::askToUpdate() {
  ConfirmationActivity::Question question;
  // With a release title, that is the headline and "New update" its caption; without one, the
  // dialog reads as it always has.
  if (updater.getReleaseName().empty()) {
    question.headline = tr(STR_NEW_UPDATE);
  } else {
    question.title = tr(STR_NEW_UPDATE);
    question.headline = updater.getReleaseName();
    question.headlineMaxLines = 2;
  }
  // Several lines in one slot: FUI's layoutText() breaks on \n explicitly and keeps a blank
  // line (FreeInkUICore.h), so the two versions and, below a gap, the opening lines of the
  // release notes each start their own line without needing more text slots.
  question.message = std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION + "\n" +
                     std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion();
  if (!updater.getReleaseNotes().empty()) {
    question.message += "\n\n" + updater.getReleaseNotes();
  }
  // Two version lines, the gap, and up to five lines of notes; ConfirmDialog takes lines back
  // if the panel would not fit the screen.
  question.messageMaxLines = 8;
  question.acceptLabel = StrId::STR_UPDATE;

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, std::move(question)),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             firmwareDeclined = true;
                             checkPacks();
                             return;
                           }
                           startUpdate();
                         });
}

void OtaUpdateActivity::checkPacks() {
  {
    RenderLock lock(*this);
    state = NO_UPDATE;
    packCheck = PackCheck::Checking;
  }
  requestUpdateAndWait();

  // One TLS session at a time, and the heap for this one: the release check's session closed with its call, and what
  // only an install would have read goes now. The screens drew since the last trim.
  updater.releaseCheckDetails();
  releaseMemoryForDownload(renderer, "OTA");

  // The Font Manager's manifest load and comparison. The catalog lives for the count only: it is freed before anything
  // is drawn or asked, and before the Font Manager loads its own.
  size_t updates = 0;
  const char* error = nullptr;
  bool loaded = false;
  {
    FontCatalog catalog;
    std::string baseUrl;
    const FontInstaller installer(sdFontSystem.registry());
    loaded = FontDownloadActivity::loadCatalog(installer, catalog, baseUrl, error);
    if (loaded) updates = FontDownloadRules::packUpdates(catalog);
  }
  if (loaded) {
    LOG_INF("OTA", "Language and hyphenation packs: %u with an update", static_cast<unsigned>(updates));
  } else {
    LOG_ERR("OTA", "Pack check failed: %s", error);
  }

  {
    RenderLock lock(*this);
    packUpdates = static_cast<uint16_t>(std::min<size_t>(updates, UINT16_MAX));
    packCheck = !loaded ? PackCheck::Failed : updates == 0 ? PackCheck::UpToDate : PackCheck::Updates;
  }
  if (packCheck == PackCheck::Updates) {
    askToUpdatePacks();
    return;
  }
  requestUpdate();
}

void OtaUpdateActivity::askToUpdatePacks() {
  ConfirmationActivity::Question question;
  char line[128];
  question.headline = packLine(line, sizeof(line));
  question.acceptLabel = StrId::STR_UPDATE;
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, std::move(question));
  if (!confirmation) {
    // The line under the versions gives the count; Back leaves.
    LOG_ERR("OTA", "OOM: pack update question (%zu bytes)", sizeof(ConfirmationActivity));
    requestUpdate();
    return;
  }
  // Cancel: the line keeps the count, and Back leaves.
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (!result.isCancelled) updatePacks();
  });
}

void OtaUpdateActivity::updatePacks() {
  FontDownloadActivity::Wanted wanted;
  wanted.packUpdates = true;
  // In this screen's place, as a pack offer opens it (PackOffer::ask): the downloads' TLS session needs the heap. Left,
  // the Font Manager restarts into Settings, where Check for updates was.
  auto download =
      makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInput, wanted, FontDownloadActivity::ReturnTo::Settings);
  if (!download) {
    LOG_ERR("OTA", "OOM: Font Manager for the pack updates");
    return;
  }
  LOG_INF("OTA", "Updating %u packs in the Font Manager", static_cast<unsigned>(packUpdates));
  handingOver = true;
  activityManager.replaceActivity(std::move(download));
}

const char* OtaUpdateActivity::packLine(char* buf, const size_t size) const {
  switch (packCheck) {
    case PackCheck::NotRun:
      return nullptr;
    case PackCheck::Checking:
      return tr(STR_CHECKING_UPDATE);
    case PackCheck::UpToDate:
      return tr(STR_PACKS_UP_TO_DATE);
    case PackCheck::Updates:
      snprintf(buf, size, tr(STR_PACK_UPDATES), static_cast<int>(packUpdates));
      return buf;
    case PackCheck::Failed:
      return tr(STR_PACK_CHECK_FAILED);
  }
  return nullptr;
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Free the heap the WiFi stack needs before it is brought up, not after -
  // association itself is the allocation-heavy step, well ahead of TLS. That
  // includes the per-category SettingInfo vectors of the SettingsActivity below
  // us, which fragment the heap; onExit() reboots, so they are never needed again.
  // WifiSelectionActivity sets WIFI_STA itself, so no radio work happens here.
  releaseMemoryForDownload(renderer, "OTA");

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Handed over to the Font Manager: the radio is its now, and it restarts when left.
  if (handingOver) return;
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  // Nothing of our own while asking: the question is the ConfirmationActivity above.
  if (state == WAITING_CONFIRMATION) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = contentRect.y + (contentRect.height - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    y = GUI.drawProgressBar(renderer,
                            Rect{contentRect.x + metrics.contentSidePadding, y,
                                 contentRect.width - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                            static_cast<int>(updaterProgress * 100), 100) +
        metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, firmwareDeclined ? tr(STR_NEW_UPDATE) : tr(STR_NO_UPDATE), true,
                              EpdFontFamily::BOLD);
    const int textWidth = contentRect.width - metrics.contentSidePadding * 2;
    const std::string current = std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION;
    const std::string latest = std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion();
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      renderer.truncatedText(UI_10_FONT_ID, current.c_str(), textWidth).c_str());
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding,
                      top + height * 2 + metrics.verticalSpacing * 2,
                      renderer.truncatedText(UI_10_FONT_ID, latest.c_str(), textWidth).c_str());
    // The packs' line, below a gap; a translation may take two lines.
    char line[128];
    if (const char* packs = packLine(line, sizeof(line))) {
      int y = top + height * 3 + metrics.verticalSpacing * 4;
      for (const auto& wrapped : renderer.wrappedText(UI_10_FONT_ID, packs, textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, y, wrapped.c_str());
        y += height;
      }
    }
    // Nothing to press while the packs are checked: the check holds the loop.
    if (packCheck != PackCheck::Checking) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    const char* reason = "";
    switch (failureReason) {
      case OtaUpdater::HTTP_ERROR:
        reason = "Network error (HTTP request failed)";
        break;
      case OtaUpdater::JSON_PARSE_ERROR:
        reason = "Could not parse release info";
        break;
      case OtaUpdater::UPDATE_OLDER_ERROR:
        reason = "Available version is not newer";
        break;
      case OtaUpdater::OOM_ERROR:
        reason = "Out of memory";
        break;
      case OtaUpdater::METADATA_TOO_LARGE_ERROR:
        reason = tr(STR_RELEASE_METADATA_TOO_LARGE);
        break;
      case OtaUpdater::INTERNAL_UPDATE_ERROR:
        reason = "Internal update error";
        break;
      case OtaUpdater::NO_UPDATE:
        reason = "No firmware asset found";
        break;
      case OtaUpdater::VALIDATE_FAILED:
        reason = "Bootloader incompatible - reflash via USB with PlatformIO";
        break;
      case OtaUpdater::WRONG_DEVICE_ERROR:
        reason = tr(STR_FIRMWARE_WRONG_DEVICE);
        break;
      case OtaUpdater::CHECKSUM_ERROR:
        reason = "Download corrupted (checksum mismatch)";
        break;
      default:
        break;
    }
    if (reason[0] != '\0') {
      renderer.drawCenteredText(SMALL_FONT_ID, top + height + metrics.verticalSpacing, reason);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    const int hintWidth = contentRect.width - 2 * metrics.contentSidePadding;
    const auto hintLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_POWER_ON_HINT), hintWidth, 4);
    int hintY = top + height + metrics.verticalSpacing;
    for (const auto& line : hintLines) {
      renderer.drawCenteredText(UI_10_FONT_ID, hintY, line.c_str());
      hintY += height;
    }
  }

  renderer.displayBuffer();

  if (state == FINISHED) {
    state = SHUTTING_DOWN;
  }
}

void OtaUpdateActivity::startUpdate() {
  LOG_DBG("OTA", "New update available, starting download...");
  // The question drew since the check: give its glyph caches back before the transfer.
  releaseMemoryForDownload(renderer, "OTA");
  // The install now streams in one blocking call; drive the progress bar and
  // Back-to-cancel from inside the download via this callback. Throttle state
  // is a member (the callback outlives this stack frame).
  lastOtaDrawMs = 0;
  updater.setInstallProgressCallback([this](size_t, size_t) -> bool {
    mappedInput.update();
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      return false;  // abort the install
    }
    const uint32_t now = millis();
    if (now - lastOtaDrawMs >= 1000) {  // throttle e-ink redraws
      lastOtaDrawMs = now;
      requestUpdate(true);
    }
    return true;
  });
  const auto beginResult = updater.beginInstallUpdate();
  if (beginResult != OtaUpdater::UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update begin failed: %d", beginResult);
    {
      RenderLock lock(*this);
      failureReason = beginResult;
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPDATE_IN_PROGRESS;
  }
  requestUpdate();
  return;
}

void OtaUpdateActivity::loop() {
  // TODO @ngxson : refactor this logic later
  if (updater.getRender()) {
    requestUpdate();
    updater.clearRender();
  }

  if (state == UPDATE_IN_PROGRESS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      updater.cancelUpdate();
      finish();
      return;
    }

    const auto res = updater.performInstallUpdateStep();
    if (res == OtaUpdater::UPDATE_IN_PROGRESS) {
      if (updater.getRender()) {
        requestUpdate();
        updater.clearRender();
      }
      return;
    }

    if (res == OtaUpdater::OK) {
      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      requestUpdate();
      return;
    }

    if (res == OtaUpdater::UPDATE_CANCELLED) {
      finish();
      return;
    }

    LOG_DBG("OTA", "Update failed: %d", res);
    {
      RenderLock lock(*this);
      failureReason = res;
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
