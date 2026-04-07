#include "KOReaderSyncActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

bool KOReaderSyncActivity::ensureEpubLoaded() {
  if (epub) {
    return true;
  }

  auto loadedEpub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  loadedEpub->setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
  if (!loadedEpub->load(true, true)) {
    LOG_ERR("KOSync", "Failed to load EPUB for offline mapping: %s", epubPath.c_str());
    return false;
  }

  epub = std::move(loadedEpub);
  return true;
}

void KOReaderSyncActivity::releaseEpub() { epub.reset(); }

bool KOReaderSyncActivity::prepareComparisonData() {
  if (!ensureEpubLoaded()) {
    return false;
  }

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine, localParagraphIndex,
                                 hasLocalParagraphIndex};
  localProgress = ProgressMapper::toKOReader(epub, localPos);

  const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
  const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  remoteChapterLabel = (remoteTocIndex >= 0)
                           ? epub->getTocItem(remoteTocIndex).title
                           : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
  localChapterLabel = (localTocIndex >= 0)
                          ? epub->getTocItem(localTocIndex).title
                          : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

  releaseEpub();
  return true;
}

bool KOReaderSyncActivity::prepareLocalUploadData() {
  if (!localProgress.xpath.empty()) {
    return true;
  }

  if (!ensureEpubLoaded()) {
    return false;
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine, localParagraphIndex,
                                 hasLocalParagraphIndex};
  localProgress = ProgressMapper::toKOReader(epub, localPos);
  releaseEpub();
  return true;
}

void KOReaderSyncActivity::startWifiSelection(const NetworkAction action) {
  pendingNetworkAction = action;
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  {
    RenderLock lock(*this);
    if (pendingNetworkAction == NetworkAction::FetchRemote) {
      state = SYNCING;
      statusMessage = tr(STR_SYNCING_TIME);
    } else {
      state = RECONNECTING;
      statusMessage = tr(STR_UPLOAD_PROGRESS);
    }
  }
  requestUpdate(true);

  if (pendingNetworkAction == NetworkAction::FetchRemote) {
    if (!HalClock::isSynced()) {
      LOG_DBG("KOSync", "Clock unsynced, attempting NTP before sync");
      HalClock::syncNtp();
    } else {
      LOG_DBG("KOSync", "Clock already synced, skipping pre-sync NTP");
    }

    {
      RenderLock lock(*this);
      statusMessage = tr(STR_CALC_HASH);
    }
    requestUpdate(true);

    performFetch();
    return;
  }

  performUpload();
}

void KOReaderSyncActivity::performFetch() {
  // Calculate document hash based on user's preferred method
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  } else {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("KOSync", "Document hash: %s", documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  const auto authProbeResult = KOReaderSyncClient::authenticate();
  LOG_DBG("KOSync", "Pre-fetch auth probe result: %s (%s)", KOReaderSyncClient::errorString(authProbeResult),
          KOReaderSyncClient::lastFailureDetail());
  if (authProbeResult != KOReaderSyncClient::OK) {
    HalClock::wifiOff(true);
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(authProbeResult);
      const char* detail = KOReaderSyncClient::lastFailureDetail();
      if (detail && detail[0]) {
        statusMessage += " — ";
        statusMessage += detail;
      }
    }
    requestUpdate(true);
    return;
  }

  // Fetch remote progress
  const auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  HalClock::wifiOff(true);

  if (result == KOReaderSyncClient::NOT_FOUND) {
    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      // Combine the short category label with the rich diagnostic so users (and bug
      // reports) can tell network/TLS/server/heap failures apart at a glance.
      statusMessage = KOReaderSyncClient::errorString(result);
      const char* detail = KOReaderSyncClient::lastFailureDetail();
      if (detail && detail[0]) {
        statusMessage += " — ";
        statusMessage += detail;
      }
    }
    requestUpdate(true);
    return;
  }

  // Convert remote progress to CrossPoint position
  hasRemoteProgress = true;
  {
    RenderLock lock(*this);
    state = MAPPING;
    statusMessage = tr(STR_MAPPING_REMOTE);
  }
  requestUpdateAndWait();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_MAPPING_LOCAL);
  }
  requestUpdateAndWait();

  if (!prepareComparisonData()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  if (!prepareLocalUploadData()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    startWifiSelection(NetworkAction::UploadPrepared);
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  const auto result = KOReaderSyncClient::updateProgress(progress);

  if (result != KOReaderSyncClient::OK) {
    HalClock::wifiOff(true);
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      // Combine the short category label with the rich diagnostic so users (and bug
      // reports) can tell network/TLS/server/heap failures apart at a glance.
      statusMessage = KOReaderSyncClient::errorString(result);
      const char* detail = KOReaderSyncClient::lastFailureDetail();
      if (detail && detail[0]) {
        statusMessage += " — ";
        statusMessage += detail;
      }
    }
    requestUpdate();
    return;
  }

  HalClock::wifiOff(true);
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
    uploadCompleteTime = millis();
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  startWifiSelection(NetworkAction::FetchRemote);
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  HalClock::wifiOff(true);
}

void KOReaderSyncActivity::closeCancelled() {
  if (closeRequested) {
    return;
  }

  closeRequested = true;
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void KOReaderSyncActivity::render(RenderLock&&) {
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15 + contentRect.y, tr(STR_KOREADER_SYNC), true, EpdFontFamily::BOLD);

  if (state == NO_CREDENTIALS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_NO_CREDENTIALS_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_KOREADER_SETUP_HINT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == MAPPING || state == RECONNECTING || state == UPLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Get chapter names from TOC
    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 160, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapterLabel.c_str());
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 185, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 210, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 235, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 270, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapterLabel.c_str());
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 295, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 320, localPageStr);

    const int optionY = 350;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(contentRect.x, optionY - 2, contentRect.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, optionY, tr(STR_APPLY_REMOTE), selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(contentRect.x, optionY + optionHeight - 2, contentRect.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, optionY + optionHeight, tr(STR_UPLOAD_LOCAL),
                      selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeCancelled();
      return;
    }

    if (state == UPLOAD_COMPLETE && millis() - uploadCompleteTime >= 3000) {
      closeCancelled();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        // Wifi will be turned off in onExit()
        setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber, remotePosition.paragraphIndex,
                             remotePosition.hasParagraphIndex});
        finish();
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeCancelled();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeCancelled();
    }
    return;
  }
}
