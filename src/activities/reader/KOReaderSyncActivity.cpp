#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <cmath>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr time_t NTP_RESYNC_MIN_INTERVAL_SEC = 15 * 60;

// Emits heap snapshots around sync stages so we can correlate TLS failures with
// fragmentation and not just total free heap.
void logSyncMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("KOSync", "Sync mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}

bool shouldSyncNtpNow() {
  const time_t lastSync = HalClock::lastSyncTime();
  const time_t now = HalClock::now();
  if (lastSync <= 0 || now <= 0) {
    return true;
  }

  const time_t age = now - lastSync;
  if (age < 0) {
    return true;
  }
  return age >= NTP_RESYNC_MIN_INTERVAL_SEC;
}
}  // namespace

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, resuming reader");
    resumeReader(KOReaderSyncOutcomeState::CANCELLED);
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate();

  // Avoid repeated NTP churn during rapid sync retries; it can fragment heap
  // right before TLS. Re-sync only when clock is stale.
  if (shouldSyncNtpNow()) {
    HalClock::syncNtp(SETTINGS.ntpServer);
  } else {
    LOG_DBG("KOSync", "Skipping NTP sync (recently synced)");
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate();

  logSyncMemSnapshot("before_performSync");
  trimMemoryForNetworkSession(renderer, "KOSync");
  logSyncMemSnapshot("after_trim_before_performSync");

  performSync();

  logSyncMemSnapshot("after_performSync");
}

std::string KOReaderSyncActivity::hashForMethod(const DocumentMatchMethod method) const {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(epubPath)
                                                 : KOReaderDocumentId::calculate(epubPath);
}

const char* KOReaderSyncActivity::matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

bool KOReaderSyncActivity::calculateDocumentHash() {
  // A previous sync may have proved the server holds this book under the other method's id
  // (KOReader defaults to binary, we default to filename). That is a fact about the server,
  // so it outranks the local preference for both reads and writes.
  const auto learned = KOReaderDocumentId::loadLearnedMatchMethod(epubPath);
  effectiveMatchMethod = learned.value_or(KOREADER_STORE.getMatchMethod());
  if (learned) {
    LOG_DBG("KOSync", "Using learned %s document id for this book", matchMethodName(effectiveMatchMethod));
  }

  documentHash = hashForMethod(effectiveMatchMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return false;
  }

  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(effectiveMatchMethod), documentHash.c_str());
  return true;
}

void KOReaderSyncActivity::applyRemoteAndFinish() {
  // Preserve the apply result and show explicit confirmation before returning
  // to the reader so users can tell the remote position was taken.
  auto& sync = APP_STATE.koReaderSyncSession;
  sync.outcome = KOReaderSyncOutcomeState::APPLIED_REMOTE;
  sync.resultSpineIndex = remotePosition.spineIndex;
  sync.resultPage = remotePosition.pageNumber;
  sync.resultParagraphIndex = remotePosition.paragraphIndex;
  sync.resultHasParagraphIndex = remotePosition.hasParagraphIndex;
  sync.resultListItemIndex = remotePosition.listItemIndex;
  sync.resultHasListItemIndex = remotePosition.hasListItemIndex;
  APP_STATE.saveToFile();

  if (syncIntent == KOReaderSyncIntentState::AUTO_PULL) {
    // Auto-pull skips the success-screen dwell — the reader will render the new
    // position immediately, which is the only visible feedback the user needs.
    esp_wifi_stop();
    resumeReader(KOReaderSyncOutcomeState::APPLIED_REMOTE);
    return;
  }
  {
    RenderLock lock(*this);
    state = APPLY_COMPLETE;
    uploadCompleteTime = millis();
  }
  requestUpdate(true);
}

// -1 remote is further, 0 the two agree, +1 local is further.
int KOReaderSyncActivity::compareLocalToRemote() const {
  if (remotePosition.spineIndex < 0) {
    // No usable mapping; percentage is all we have. Tolerate float noise so a rounding
    // difference doesn't present as a real conflict.
    static constexpr float SAME_PROGRESS_EPSILON = 0.001f;
    const float delta = localProgress.percentage - remoteProgress.percentage;
    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) return 0;
    return delta > 0 ? 1 : -1;
  }
  if (currentSpineIndex != remotePosition.spineIndex) {
    return currentSpineIndex > remotePosition.spineIndex ? 1 : -1;
  }
  if (currentPage != remotePosition.pageNumber) {
    return currentPage > remotePosition.pageNumber ? 1 : -1;
  }
  if (hasLocalParagraphIndex && remotePosition.hasParagraphIndex &&
      localParagraphIndex != remotePosition.paragraphIndex) {
    return localParagraphIndex > remotePosition.paragraphIndex ? 1 : -1;
  }
  return 0;
}

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

bool KOReaderSyncActivity::probeAlternateDocumentId(const bool havePrimaryRecord) {
  const DocumentMatchMethod altMethod = effectiveMatchMethod == DocumentMatchMethod::FILENAME
                                            ? DocumentMatchMethod::BINARY
                                            : DocumentMatchMethod::FILENAME;
  const std::string altHash = hashForMethod(altMethod);
  if (altHash.empty() || altHash == documentHash) {
    return false;
  }

  LOG_DBG("KOSync", "%s under the %s id; probing the %s id %s",
          havePrimaryRecord ? "Checking for a further record" : "No record", matchMethodName(effectiveMatchMethod),
          matchMethodName(altMethod), altHash.c_str());

  KOReaderProgress altProgress;
  const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
  if (altResult != KOReaderSyncClient::OK) {
    LOG_DBG("KOSync", "Alternate %s id has no record either (result=%d)", matchMethodName(altMethod), altResult);
    return false;
  }

  // Both ids hold a record, so this is not a missing-book question any more but a
  // which-one-is-live question. Only move if the other device is genuinely further along;
  // otherwise ours stands and we keep writing where we already were.
  if (havePrimaryRecord && altProgress.percentage <= remoteProgress.percentage) {
    LOG_DBG("KOSync", "Alternate %s id is not ahead (%.4f <= %.4f); keeping the %s id", matchMethodName(altMethod),
            altProgress.percentage, remoteProgress.percentage, matchMethodName(effectiveMatchMethod));
    return false;
  }

  // Proof of which id the server holds this book under. Adopt it for this session and
  // persist it, so subsequent syncs skip the probe entirely and — the point of the whole
  // exercise — upload to the id the other device actually reads.
  LOG_INF("KOSync", "Found remote progress under the %s id; adopting it for this book", matchMethodName(altMethod));
  documentHash = altHash;
  effectiveMatchMethod = altMethod;
  remoteProgress = std::move(altProgress);
  KOReaderDocumentId::saveLearnedMatchMethod(epubPath, altMethod);
  return true;
}

bool KOReaderSyncActivity::handleAutoPushPreflight() {
  KOReaderSyncClient::beginPersistentSession();
  KOReaderProgress warmupProgress;
  auto warmupResult = KOReaderSyncClient::getProgress(documentHash, warmupProgress);

  // Same probe as the compare path. Without it, auto-push-on-close is the most likely way to
  // create the divergence in the first place: it would silently write a second record under
  // our id while the other device's progress sits under theirs.
  if (warmupResult == KOReaderSyncClient::NOT_FOUND && probeAlternateDocumentId(false)) {
    warmupProgress = remoteProgress;
    warmupResult = KOReaderSyncClient::OK;
  }

  if (warmupResult != KOReaderSyncClient::OK && warmupResult != KOReaderSyncClient::NOT_FOUND) {
    KOReaderSyncClient::endPersistentSession();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(warmupResult);
      const char* detail = KOReaderSyncClient::lastFailureDetail();
      if (detail && detail[0]) {
        statusMessage += " — ";
        statusMessage += detail;
      }
    }
    requestUpdate(true);
    return false;
  }
  // Auto-push must not overwrite progress that is already further along on the server.
  if (syncIntent == KOReaderSyncIntentState::AUTO_PUSH && warmupResult == KOReaderSyncClient::OK &&
      warmupProgress.percentage > localProgress.percentage) {
    LOG_DBG("KOSync", "AUTO_PUSH skipped: remote %.4f >= local %.4f", warmupProgress.percentage,
            localProgress.percentage);
    KOReaderSyncClient::endPersistentSession();
    // Drop the radio while user reads the result; full teardown happens at silent reboot.
    esp_wifi_stop();
    APP_STATE.koReaderSyncSession.outcome = KOReaderSyncOutcomeState::UPLOAD_COMPLETE;
    APP_STATE.saveToFile();
    resumeReader(KOReaderSyncOutcomeState::UPLOAD_COMPLETE);
    return false;
  }
  return true;
}

void KOReaderSyncActivity::performFetchAndCompare() {
  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdate();

  // Keep the GET connection alive so Upload can reuse the same session and
  // avoid a second TLS handshake under fragmented heap.
  KOReaderSyncClient::beginPersistentSession();

  // Fetch remote progress
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  // Nothing under our id may just mean the other device computed a different one. Costs one
  // extra GET on the already-warm session, and only when the lookup came up empty.
  if (result == KOReaderSyncClient::NOT_FOUND && probeAlternateDocumentId(false)) {
    result = KOReaderSyncClient::OK;
  } else if (result == KOReaderSyncClient::OK && smartSyncEnabled()) {
    // Smart mode also probes when our own id DID resolve. Otherwise a pairing that diverged
    // before the learned-id work existed stays diverged forever: our record keeps answering,
    // so the miss that would trigger a probe never happens. Costs one extra GET per smart
    // sync, which is the price of not asking the user to reconcile it by hand.
    probeAlternateDocumentId(true);
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (syncIntent == KOReaderSyncIntentState::PULL_REMOTE) {
      // Pull intent must not silently fall back to upload when server has no
      // remote progress. Failing explicitly keeps action semantics predictable.
      KOReaderSyncClient::endPersistentSession();
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_NO_REMOTE_MSG);
      }
      requestUpdate(true);
      return;
    }

    if (syncIntent == KOReaderSyncIntentState::AUTO_PULL) {
      // Auto-pull at book open: nothing to apply, just open the book with local progress.
      KOReaderSyncClient::endPersistentSession();
      esp_wifi_stop();
      resumeReader(KOReaderSyncOutcomeState::CANCELLED);
      return;
    }

    // Keep session open so an immediate upload can reuse the same connection.
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
    KOReaderSyncClient::endPersistentSession();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
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

  // Prepare remote mapping state for the next step.
  hasRemoteProgress = false;
  remotePositionMapped = false;
  remotePosition.spineIndex = -1;
  remotePosition.pageNumber = -1;
  remotePosition.totalPages = 0;
  remotePosition.paragraphIndex = 0;
  remotePosition.hasParagraphIndex = false;
  remotePosition.listItemIndex = 0;
  remotePosition.hasListItemIndex = false;
  remoteChapterLabel.clear();

  if (syncIntent == KOReaderSyncIntentState::PULL_REMOTE || syncIntent == KOReaderSyncIntentState::AUTO_PULL) {
    // Pull intent applies immediately and exits. We bypass chooser UI to keep
    // reader menu actions deterministic ("pull" always means apply remote).
    if (!ensureRemotePositionMapped()) {
      if (syncIntent == KOReaderSyncIntentState::AUTO_PULL) {
        // Auto-pull was best-effort. Fail silently and just open the book.
        esp_wifi_stop();
        resumeReader(KOReaderSyncOutcomeState::CANCELLED);
        return;
      }
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }

    applyRemoteAndFinish();
    return;
  }

  // Compare intent keeps the legacy chooser flow (apply vs upload), which is
  // still useful for manual conflict decisions.
  // Pre-map remote progress now so compare UI always shows concrete chapter/
  // page data. The mapped result is cached and reused if Apply is chosen.
  // closeSessionBeforeMapping=true tears down the warmed TLS session before
  // reverse XPath mapping so the 32 KB inflate ring buffer can allocate. The
  // held-open wolfSSL connection otherwise pins contiguous heap and the inflate
  // malloc fails, falling back to lossy percentage mapping.
  // Trade-off: if the user picks Upload afterwards we eat one extra TLS
  // handshake (~1.7s) — Apply is the common choice and silent inflate
  // failures here previously caused syncs to land on the wrong page.
  if (!ensureRemotePositionMapped(true)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // Local progress was precomputed before network; keep using the cached value.
  releaseEpubForMapping();

  const int comparison = compareLocalToRemote();

  if (smartSyncEnabled()) {
    // Resolve it rather than asking. The chooser only ever had one sensible answer in these
    // cases, and it was already preselected — this just stops making the user confirm it.
    if (comparison == 0) {
      LOG_DBG("KOSync", "Smart sync: the two sides agree, nothing to do");
      {
        RenderLock lock(*this);
        state = SYNC_COMPLETE;
        uploadCompleteTime = millis();
      }
      requestUpdate(true);
      return;
    }
    if (comparison > 0) {
      LOG_DBG("KOSync", "Smart sync: local is further, uploading");
      performUpload();
      return;
    }
    LOG_DBG("KOSync", "Smart sync: remote is further, applying");
    applyRemoteAndFinish();
    return;
  }

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    // Default to the option matching the furthest progress.
    selectedOption = comparison > 0 ? 1 /* Upload local */ : 0 /* Apply remote */;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performSync() {
  if (!calculateDocumentHash()) {
    return;
  }

  // Local mapping is only needed for compare/upload paths.
  // Pull-only modes can skip this expensive step and go straight to remote fetch.
  if (syncIntent != KOReaderSyncIntentState::PULL_REMOTE && syncIntent != KOReaderSyncIntentState::AUTO_PULL) {
    // Precompute local mapping before first network request so the expensive
    // inflate/index work happens before TLS. This avoids a second local mapping
    // pass later and keeps the upload path lightweight.
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_MAPPING_LOCAL);
    }
    requestUpdateAndWait();
    if (!computeLocalProgressAndChapter()) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }
  }

  // Drop EPUB state before HTTPS to maximize contiguous heap for TLS.
  releaseEpubForMapping();

  // PUSH_LOCAL is an explicit user upload — go straight to PUT.
  // AUTO_PUSH needs the preflight GET to bail out if the remote is already ahead.
  if (syncIntent == KOReaderSyncIntentState::PUSH_LOCAL) {
    performUpload();
    return;
  }
  if (syncIntent == KOReaderSyncIntentState::AUTO_PUSH) {
    if (!handleAutoPushPreflight()) return;
    performUpload();
    return;
  }

  performFetchAndCompare();
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // If sync reached this screen without cached local progress, compute it now.
  // This keeps upload robust when UI flow changes or retries happen.
  if (localProgress.xpath.empty()) {
    if (!computeLocalProgressAndChapter()) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }
    releaseEpubForMapping();
  }

  // Hard-stop if we still have no xpath: sending an empty progress payload would
  // be ambiguous server-side and hides the real local mapping failure.
  if (localProgress.xpath.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // Sync UI rendering can repopulate glyph caches after the initial GET / compare
  // phase, so trim again right before the upload request.
  trimMemoryForNetworkSession(renderer, "KOSync");
  logSyncMemSnapshot("after_trim_before_updateProgress");

  // Capture upload-phase memory separately from fetch phase to diagnose failures
  // that only appear on PUT due to allocator state changes.
  logSyncMemSnapshot("before_updateProgress");

  // Ensure a session exists for upload. In compare flow this comes from the
  // earlier GET; in direct-push flow it comes from the warmup GET above.
  // In both cases, reuse avoids a second full TLS handshake.
  KOReaderSyncClient::beginPersistentSession();

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.metadata = localDocumentMetadata;

  const auto result = KOReaderSyncClient::updateProgress(progress);
  KOReaderSyncClient::endPersistentSession();
  logSyncMemSnapshot("after_updateProgress");

  if (result != KOReaderSyncClient::OK) {
    // Drop the radio while user reads the result; full teardown happens at silent reboot.
    esp_wifi_stop();
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

  // Drop the radio while user reads the success screen; full teardown happens at silent reboot.
  esp_wifi_stop();
  APP_STATE.koReaderSyncSession.outcome = KOReaderSyncOutcomeState::UPLOAD_COMPLETE;
  APP_STATE.saveToFile();
  if (syncIntent == KOReaderSyncIntentState::AUTO_PUSH) {
    // Auto-push doesn't need user acknowledgement on success; resume immediately
    // back to the calling activity (RecentBooks / FileBrowser via reader).
    resumeReader(KOReaderSyncOutcomeState::UPLOAD_COMPLETE);
    return;
  }
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
    uploadCompleteTime = millis();
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  logSyncMemSnapshot("onEnter_begin");
  LOG_DBG("KOSync", "Standalone sync start: path=%s spine=%d page=%d/%d intent=%d", epubPath.c_str(), currentSpineIndex,
          currentPage, totalPagesInSpine, static_cast<int>(syncIntent));

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  logSyncMemSnapshot("onExit_before_cleanup");
  KOReaderSyncClient::endPersistentSession();
  releaseEpubForMapping();
  logSyncMemSnapshot("onExit_after_cleanup");

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    switch (postAction_) {
      case KOReaderSyncPostAction::Home:
      case KOReaderSyncPostAction::OpdsSearch:
        // OpdsSearch is resolved post-reboot by HomeActivity (see its onEnter()); the reboot
        // itself just needs to land on Home like a plain post-sync Home would.
        silentRestart();
        break;
      case KOReaderSyncPostAction::OpenBook:
        // Repoint the "book to reopen on boot" record at the next book instead of the one that
        // was just synced, then reuse the ordinary reader restart target.
        if (!postActionTarget_.empty()) {
          APP_STATE.openEpubPath = postActionTarget_;
          APP_STATE.saveToFile();
        }
        silentRestartToReader();
        break;
      case KOReaderSyncPostAction::Reader:
      default:
        silentRestartToReader();
        break;
    }
  }
}

void KOReaderSyncActivity::closeCancelled() {
  if (closeRequested) {
    return;
  }

  resumeReader(KOReaderSyncOutcomeState::CANCELLED);
}

void KOReaderSyncActivity::resumeReader(const KOReaderSyncOutcomeState outcome, const SyncResult* appliedResult) {
  if (closeRequested) {
    return;
  }

  closeRequested = true;
  auto& sync = APP_STATE.koReaderSyncSession;
  sync.outcome = outcome;
  if (appliedResult) {
    sync.resultSpineIndex = appliedResult->spineIndex;
    sync.resultPage = appliedResult->page;
    sync.resultParagraphIndex = appliedResult->paragraphIndex;
    sync.resultHasParagraphIndex = appliedResult->hasParagraphIndex;
    sync.resultListItemIndex = appliedResult->listItemIndex;
    sync.resultHasListItemIndex = appliedResult->hasListItemIndex;
  } else if (outcome != KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    // Only zero the result fields when not resuming an already-applied remote
    // position. The PULL_REMOTE path pre-saves the mapped result into APP_STATE
    // before entering APPLY_COMPLETE; zeroing here would overwrite it.
    sync.resultSpineIndex = 0;
    sync.resultPage = 0;
    sync.resultParagraphIndex = 0;
    sync.resultHasParagraphIndex = false;
    sync.resultListItemIndex = 0;
    sync.resultHasListItemIndex = false;
  }
  // Capture the destination before touching the session, since onExit() still needs it after the
  // reboot decision. What may be cleared here depends entirely on who consumes the session next:
  //
  //  - Reader: the reopened reader is the consumer. applyPendingSyncSession() reads active,
  //    epubPath and the result fields to move the user to the position Apply just resolved.
  //    Clearing here throws that away and the book reopens exactly where it was, which looks
  //    from the outside like Apply silently doing nothing.
  //  - OpdsSearch: HomeActivity consumes postActionTarget after the reboot, so only `active`
  //    is dropped and the rest is left in place for it.
  //  - Home / OpenBook: nobody applies a result (Home has no reader; OpenBook opens a
  //    different book, whose reader would reject this session on the epubPath check and leave
  //    it lying around active). Clear it.
  postAction_ = sync.postAction;
  postActionTarget_ = sync.postActionTarget;
  switch (postAction_) {
    case KOReaderSyncPostAction::Reader:
      break;
    case KOReaderSyncPostAction::OpdsSearch:
      sync.active = false;
      break;
    case KOReaderSyncPostAction::Home:
    case KOReaderSyncPostAction::OpenBook:
    default:
      sync.clear();
      break;
  }
  APP_STATE.saveToFile();
  logSyncMemSnapshot("before_resume_reader");
  switch (postAction_) {
    case KOReaderSyncPostAction::Home:
    case KOReaderSyncPostAction::OpdsSearch:
      activityManager.goHome();
      return;
    case KOReaderSyncPostAction::OpenBook:
      activityManager.goToReader(postActionTarget_.empty() ? epubPath : postActionTarget_);
      return;
    case KOReaderSyncPostAction::Reader:
    default:
      activityManager.goToReader(epubPath);
      return;
  }
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

  if (state == SYNCING || state == UPLOADING) {
    // Title frame already composed into the write buffer above (clearScreen); overlay the popup on
    // it. drawPopup ships the frame itself — a second displayBuffer() here would ship the stale
    // post-swap buffer.
    GUI.drawPopup(renderer, statusMessage.c_str(), /*overlayDisplayedFrame=*/false);
    return;
  }

  if (state == SHOWING_RESULT) {
    // Show comparison
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Get chapter names from TOC
    const std::string& remoteChapter = remoteChapterLabel;
    const std::string& localChapter = localChapterLabel;

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, 160, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
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
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
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

    // Conflict policy. Offered here because this screen is the only place the choice is ever
    // felt: a user who has just been asked is the one best placed to say "stop asking".
    if (selectedOption == OPTION_SYNC_BEHAVIOR) {
      renderer.fillRect(contentRect.x, optionY + 2 * optionHeight - 2, contentRect.width - 1, optionHeight);
    }
    char behaviorStr[96];
    snprintf(behaviorStr, sizeof(behaviorStr), "%s: %s", tr(STR_KO_SYNC_CONFLICT),
             smartSyncEnabled() ? tr(STR_KO_SMART_SYNC) : tr(STR_KO_ASK_EVERY_TIME));
    renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, optionY + 2 * optionHeight, behaviorStr,
                      selectedOption != OPTION_SYNC_BEHAVIOR);

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

  if (state == APPLY_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_PULL_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_ALREADY_IN_SYNC), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);

    // Word-wrap the detail message so long TLS/network diagnostics aren't clipped.
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), contentRect.width - 20, 4);
    int y = 320;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += lineHeight;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

bool KOReaderSyncActivity::ensureEpubLoadedForMapping() {
  if (epub) {
    return true;
  }

  // Reload on demand to keep steady-state sync memory low. Mapping and chapter
  // lookup need EPUB metadata; TLS steps do not.
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  if (!epub->load(true, true)) {
    LOG_ERR("KOSync", "Failed to reload EPUB for mapping: %s", epubPath.c_str());
    epub.reset();
    return false;
  }
  epub->setupCacheDir();
  return true;
}

bool KOReaderSyncActivity::ensureRemotePositionMapped(const bool closeSessionBeforeMapping) {
  if (remotePositionMapped) {
    return true;
  }

  // Mapping remote->local triggers EPUB inflate work, which needs a 32 KB
  // contiguous block for the deflate ring buffer. The held-open wolfSSL TLS
  // session otherwise pins contiguous heap, causing the inflate alloc to fail
  // and the reverse XPath mapper to silently degrade to lossy percentage-only
  // mapping. Tearing the session down here
  // costs a fresh handshake if Upload runs afterwards, but Apply (the common
  // outcome) wins both heap headroom and round-trip accuracy.
  if (closeSessionBeforeMapping) {
    KOReaderSyncClient::endPersistentSession();
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_MAPPING_REMOTE);
  }
  requestUpdateAndWait();

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  if (!ensureEpubLoadedForMapping()) {
    return false;
  }
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);
  computeRemoteChapter();
  releaseEpubForMapping();
  hasRemoteProgress = true;
  remotePositionMapped = true;
  return true;
}

void KOReaderSyncActivity::releaseEpubForMapping() { epub.reset(); }

bool KOReaderSyncActivity::computeLocalProgressAndChapter() {
  if (!ensureEpubLoadedForMapping()) {
    localProgress = KOReaderPosition{};
    localChapterLabel.clear();
    return false;
  }

  CrossPointPosition localPos = {currentSpineIndex,
                                 currentPage,
                                 totalPagesInSpine,
                                 localParagraphIndex,
                                 hasLocalParagraphIndex,
                                 0,  // no list item index
                                 false,
                                 localXhtmlSeekHint};
  localProgress = ProgressMapper::toKOReader(epub, localPos);

  const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  localChapterLabel = (localTocIndex >= 0)
                          ? epub->getTocItem(localTocIndex).title
                          : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

  if (KOREADER_STORE.getSendMetadata()) {
    const size_t slash = epubPath.rfind('/');
    KOReaderMetadata meta;
    meta.filename = (slash != std::string::npos) ? epubPath.substr(slash + 1) : epubPath;
    meta.title = epub->getTitle();
    meta.authors = epub->getAuthor();
    localDocumentMetadata = std::move(meta);
  } else {
    localDocumentMetadata.reset();
  }

  return true;
}

void KOReaderSyncActivity::computeRemoteChapter() {
  if (!epub) {
    return;
  }
  const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
  remoteChapterLabel = (remoteTocIndex >= 0)
                           ? epub->getTocItem(remoteTocIndex).title
                           : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == APPLY_COMPLETE ||
      state == SYNC_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (state == APPLY_COMPLETE) {
        resumeReader(KOReaderSyncOutcomeState::APPLIED_REMOTE);
      } else if (state == UPLOAD_COMPLETE) {
        resumeReader(KOReaderSyncOutcomeState::UPLOAD_COMPLETE);
      } else if (state == SYNC_FAILED || state == NO_CREDENTIALS) {
        resumeReader(KOReaderSyncOutcomeState::FAILED);
      } else {
        resumeReader(KOReaderSyncOutcomeState::CANCELLED);
      }
      return;
    }

    if ((state == UPLOAD_COMPLETE || state == APPLY_COMPLETE || state == SYNC_COMPLETE) &&
        millis() - uploadCompleteTime >= 3000) {
      if (state == SYNC_COMPLETE) {
        // Nothing moved on either side, so there is no outcome to report to the reader.
        resumeReader(KOReaderSyncOutcomeState::CANCELLED);
      } else if (state == APPLY_COMPLETE) {
        resumeReader(KOReaderSyncOutcomeState::APPLIED_REMOTE);
      } else {
        resumeReader(KOReaderSyncOutcomeState::UPLOAD_COMPLETE);
      }
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options. Up and Down previously both advanced, which was invisible with two rows
    // and wrong as soon as a third existed.
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + OPTION_COUNT - 1) % OPTION_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % OPTION_COUNT;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == OPTION_SYNC_BEHAVIOR) {
        const bool nowSmart = !smartSyncEnabled();
        KOREADER_STORE.setSyncBehavior(nowSmart ? KOReaderSyncBehavior::SMART : KOReaderSyncBehavior::ASK_EVERY_TIME);
        KOREADER_STORE.saveToFile();
        // Leave the cursor on an actionable row rather than the toggle, and put it on the row
        // the new policy would have chosen — which both avoids a second Confirm toggling straight
        // back and shows what "use furthest" actually resolves to for this book.
        {
          RenderLock lock(*this);
          selectedOption = compareLocalToRemote() > 0 ? 1 : 0;
        }
        requestUpdate();
        return;
      }
      if (selectedOption == 0) {
        if (!ensureRemotePositionMapped()) {
          {
            RenderLock lock(*this);
            state = SYNC_FAILED;
            statusMessage = tr(STR_SYNC_FAILED_MSG);
          }
          requestUpdate(true);
          return;
        }
        // Wifi will be turned off in onExit()
        const SyncResult result = {remotePosition.spineIndex,     remotePosition.pageNumber,
                                   remotePosition.paragraphIndex, remotePosition.hasParagraphIndex,
                                   remotePosition.listItemIndex,  remotePosition.hasListItemIndex};
        resumeReader(KOReaderSyncOutcomeState::APPLIED_REMOTE, &result);
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
        // Must go through the effective method, not the configured one: a book the server
        // holds under the other device's id has to keep uploading there.
        documentHash = hashForMethod(effectiveMatchMethod);
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeCancelled();
    }
    return;
  }
}
