#include "EpubReaderActivity.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <Arduino.h>
#include <Logging.h>

#include <utility>

#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "ProgressComparison.h"
#include "activities/ActivityManager.h"

namespace {
// The reader's position as ProgressComparison wants it: the page, and the paragraph LUT at the
// end of this page and of the one before.
LocalReadingPosition localReadingPosition(const Section& section, const int spineIndex, const int page) {
  LocalReadingPosition local;
  local.spineIndex = spineIndex;
  local.page = page;
  local.paragraphAtPageEnd = section.getParagraphIndexForPage(static_cast<uint16_t>(page)).value_or(0);
  if (page > 0) {
    local.paragraphAtPreviousPageEnd = section.getParagraphIndexForPage(static_cast<uint16_t>(page - 1)).value_or(0);
  }
  return local;
}

DocumentMatchMethod otherMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

std::string hashFor(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}
}  // namespace

bool EpubReaderActivity::autoSyncReaderIsQuiet() const {
  return epub != nullptr && section != nullptr && !section->hasActiveBuild() && readerPhase_ == ReaderPhase::READING &&
         !RenderLock::peek();
}

KOReaderPosition EpubReaderActivity::currentKoPosition(const int page, const int pageCount) const {
  CrossPointPosition pos{};
  pos.spineIndex = currentSpineIndex;
  pos.pageNumber = page;
  pos.totalPages = pageCount;
  if (section) {
    if (const auto off = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
      pos.visibleTextOffset = *off;
      pos.hasVisibleTextOffset = true;
    }
    if (const auto paragraph = section->getParagraphIndexForPage(static_cast<uint16_t>(page))) {
      pos.paragraphIndex = *paragraph;
      pos.hasParagraphIndex = true;
    }
  }
  return ProgressMapper::toKOReader(epub, pos);
}

void EpubReaderActivity::maybeAutoPullOnWake() {
  if (!KOReaderAutoSync::pullEnabled()) {
    autoSyncPullPending = false;
    return;
  }
  if (KOReaderAutoSync::jobActive() || !autoSyncReaderIsQuiet()) {
    return;
  }
  if (!KOReaderAutoSync::heapAllowsBackgroundSession("Wake pull")) {
    return;
  }

  const std::string path = epub->getPath();
  const DocumentMatchMethod method = KOReaderAutoSync::effectiveMatchMethod(path);
  std::string primaryHash = hashFor(path, method);
  if (primaryHash.empty()) {
    LOG_ERR("AutoSync", "Wake pull: document hash failed for %s", path.c_str());
    autoSyncPullPending = false;
    return;
  }

  KOReaderSyncJob job;
  job.kind = KOReaderSyncJob::FETCH_PROGRESS;
  if (KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART) {
    std::string altHash = hashFor(path, otherMethod(method));
    if (!altHash.empty() && altHash != primaryHash) {
      job.smartProbe = true;
      job.altHash = std::move(altHash);
    }
  }
  job.primaryHash = std::move(primaryHash);

  uint64_t seq = 0;
  if (!KOReaderSyncWorker::post(std::move(job), &seq)) {
    LOG_DBG("AutoSync", "Wake pull deferred: sync worker busy");
    return;
  }
  autoSyncPullPending = false;
  autoSyncPullSeq = seq;
  LOG_DBG("AutoSync", "Wake pull started in background: %s", path.c_str());
}

bool EpubReaderActivity::pollAutoSyncPull() {
  if (autoSyncPullSeq != 0) {
    if (!KOReaderSyncWorker::isDone(autoSyncPullSeq)) {
      return false;
    }
    autoSyncPullJob = KOReaderSyncWorker::consume(autoSyncPullSeq);
    autoSyncPullSeq = 0;
    autoSyncPullJobPending = true;
  }
  if (!autoSyncPullJobPending || !autoSyncReaderIsQuiet()) {
    return false;
  }
  autoSyncPullJobPending = false;
  evaluateAutoSyncPull();
  return autoSyncPullDialogLaunched;
}

void EpubReaderActivity::evaluateAutoSyncPull() {
  autoSyncPullDialogLaunched = false;
  KOReaderSyncJob job = std::exchange(autoSyncPullJob, {});

  const bool smart = KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
  const std::string path = epub->getPath();

  // What each record says from its XPath alone (spine, body-child paragraph): enough to compare
  // without inflating a chapter, which only an upload or an apply needs.
  const int spineCount = epub->getSpineItemsCount();
  const auto peek = [spineCount](const KOReaderProgress& progress) {
    return ProgressMapper::peekRemote({progress.progress, progress.percentage}, spineCount);
  };

  KOReaderSyncClient::Error result = job.result;
  KOReaderProgress remote = std::move(job.remote);
  if (smart && job.altResult == KOReaderSyncClient::OK && !job.altHash.empty() &&
      (result == KOReaderSyncClient::NOT_FOUND ||
       selectRemoteRecord(peek(remote), remote.percentage, peek(job.altRemote), job.altRemote.percentage) ==
           RemoteRecordChoice::Alternate)) {
    const DocumentMatchMethod altMethod = otherMethod(KOReaderAutoSync::effectiveMatchMethod(path));
    LOG_INF("AutoSync", "Wake pull: found remote progress under the alternate id; adopting it");
    KOReaderDocumentId::saveLearnedMatchMethod(path, altMethod);
    remote = std::move(job.altRemote);
    result = KOReaderSyncClient::OK;
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smart) {
      LOG_DBG("AutoSync", "Wake pull: no remote progress; uploading local silently");
      silentUploadCurrentPosition();
    } else {
      autoSyncPullDialogLaunched = handOffToInteractiveSync();
    }
    return;
  }
  if (result != KOReaderSyncClient::OK) {
    LOG_INF("AutoSync", "Wake pull failed (%s); keeping current position", KOReaderSyncClient::errorString(result));
    AUTOSYNC_STATE.markSyncFailed();
    return;
  }

  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  // The percentage only. The local XPath costs an inflate and two parses of the chapter, and is
  // computed by silentUploadCurrentPosition() if an upload turns out to be needed.
  const float localPercentage = ProgressMapper::percentageFor(epub, currentSpineIndex, page, total);
  const ProgressComparison comparison = compareProgress(localReadingPosition(*section, currentSpineIndex, page),
                                                        localPercentage, peek(remote), remote.percentage);
  switch (comparison) {
    case ProgressComparison::Synchronized:
      AUTOSYNC_STATE.markSynced(path, currentSpineIndex, page);
      LOG_DBG("AutoSync", "Wake pull: remote matches local (%.6f at %s); nothing to do", remote.percentage,
              remote.progress.c_str());
      return;
    case ProgressComparison::LocalAhead:
      if (smart) {
        LOG_DBG("AutoSync", "Wake pull: local ahead, uploading silently");
        silentUploadCurrentPosition();
        return;
      }
      break;
    case ProgressComparison::RemoteAhead:
      if (smart) {
        LOG_DBG("AutoSync", "Wake pull: remote ahead, applying silently");
        silentApplyRemote(remote);
        return;
      }
      break;
    case ProgressComparison::Unknown:
      LOG_DBG("AutoSync", "Wake pull: cannot tell which side is further; asking");
      break;
  }

  autoSyncPullDialogLaunched = handOffToInteractiveSync();
}

bool EpubReaderActivity::handOffToInteractiveSync() {
  if (!KOREADER_STORE.hasCredentials() || !epub) {
    return false;
  }
  LOG_DBG("AutoSync", "Wake pull: progress differs, handing off to the compare screen");
  launchKOReaderSync(SyncLaunchMode::COMPARE, nullptr, {KOReaderSyncPostAction::Reader, {}});
  return true;
}

void EpubReaderActivity::silentUploadCurrentPosition() {
  if (autoSyncPushSeq != 0) {
    LOG_DBG("AutoSync", "Wake upload deferred: another push in flight");
    return;
  }
  if (!epub || !section) {
    return;
  }
  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  if (page < 0 || total <= 0) {
    return;
  }
  const KOReaderPosition local = currentKoPosition(page, total);
  autoSyncPushSeq = KOReaderAutoSync::pushSilentAsync(epub->getPath(), local, currentSpineIndex, page, epub->getTitle(),
                                                      epub->getAuthor());
}

void EpubReaderActivity::silentApplyRemote(const KOReaderProgress& remote) {
  if (!epub) {
    return;
  }
  const KOReaderPosition koPos{remote.progress, remote.percentage};
  const int total = section ? section->estimatedTotalPages() : 0;
  const CrossPointPosition remotePos = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, total);
  if (remotePos.spineIndex < 0 || remotePos.spineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("AutoSync", "Wake apply: remote position unmappable (spine %d)", remotePos.spineIndex);
    return;
  }

  NavigationTarget target;
  if (remotePos.hasVisibleTextOffset) {
    target = NavigationTarget::makeVisibleOffset(remotePos.visibleTextOffset, remotePos.pageNumber);
  } else if (remotePos.hasListItemIndex) {
    target = NavigationTarget::makeListItem(remotePos.listItemIndex, remotePos.pageNumber);
  } else if (remotePos.hasParagraphIndex) {
    target = NavigationTarget::makeParagraph(remotePos.paragraphIndex, remotePos.pageNumber);
  } else {
    target = NavigationTarget::makePage(remotePos.pageNumber);
  }
  target.cachedSpineIdx = remotePos.spineIndex;

  {
    RenderLock lock(*this);
    currentSpineIndex = remotePos.spineIndex;
    navTarget = target;
    section.reset();
  }
  if (!writeReaderProgressCache(epub->getCachePath(), remotePos.spineIndex, remotePos.pageNumber, 0, 0)) {
    LOG_ERR("AutoSync", "Wake apply: failed to persist remote position; live state still seeded");
  }
  AUTOSYNC_STATE.markSynced(epub->getPath(), remotePos.spineIndex, remotePos.pageNumber);
  requestUpdate();
  LOG_DBG("AutoSync", "Wake apply: jumped to spine %d page %d (%.6f)", remotePos.spineIndex, remotePos.pageNumber,
          remote.percentage);
}

void EpubReaderActivity::maybeAutoPushInterval() {
  if (activityManager.inSleepTransition()) {
    return;
  }
  if (!KOReaderAutoSync::intervalPushEnabled() || !autoSyncReaderIsQuiet()) {
    return;
  }
  if (!AUTOSYNC_STATE.lastNoteWasTurn()) {
    return;
  }
  const uint16_t interval = KOREADER_STORE.getPushIntervalPages();
  if (interval == 0 || AUTOSYNC_STATE.getTurnsSincePush() < interval) {
    return;
  }
  if ((autoSyncLastPushAt != 0 && millis() - autoSyncLastPushAt < KOReaderAutoSync::MIN_INTERVAL_PUSH_GAP_MS) ||
      KOReaderAutoSync::jobActive()) {
    return;
  }

  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  if (page < 0 || total <= 0) {
    return;
  }
  const KOReaderPosition local = currentKoPosition(page, total);
  autoSyncLastPushAt = millis();
  autoSyncPushSeq = KOReaderAutoSync::pushSilentAsync(epub->getPath(), local, currentSpineIndex, page, epub->getTitle(),
                                                      epub->getAuthor());
}

void EpubReaderActivity::pollAutoSyncJob() {
  if (autoSyncPushSeq == 0 || !KOReaderSyncWorker::isDone(autoSyncPushSeq)) {
    return;
  }
  KOReaderAutoSync::finalizeSilentPush(autoSyncPushSeq);
  autoSyncPushSeq = 0;
}

void EpubReaderActivity::refreshAutoSyncIndicator() {
  SyncIndicator indicator = SyncIndicator::None;
  if (KOREADER_STORE.getShowSyncIndicator()) {
    if (KOReaderSyncWorker::isBusy()) {
      indicator = SyncIndicator::Active;
    } else if (AUTOSYNC_STATE.lastSyncFailed()) {
      indicator = SyncIndicator::Failed;
    }
  }
  if (indicator == autoSyncIndicator) {
    return;
  }
  autoSyncIndicator = indicator;
  requestUpdate();
}

void EpubReaderActivity::serviceAutoSync() {
  if (autoSyncPullPending) {
    maybeAutoPullOnWake();
  }
  if (pollAutoSyncPull()) {
    return;
  }
  pollAutoSyncJob();
  refreshAutoSyncIndicator();
  if (RenderLock::peek()) {
    return;
  }

  if (epub != nullptr && section != nullptr) {
    AUTOSYNC_STATE.noteProgress(epub->getPath(), currentSpineIndex, section->currentPage);
  }
  maybeAutoPushInterval();
}

void EpubReaderActivity::releaseAutoSyncSlot() {
  if (autoSyncPushSeq != 0) {
    KOReaderAutoSync::finalizeSilentPush(autoSyncPushSeq);
    autoSyncPushSeq = 0;
  }
  if (autoSyncPullSeq != 0) {
    KOReaderSyncWorker::consume(autoSyncPullSeq);
    autoSyncPullSeq = 0;
  }
  autoSyncPullJob = KOReaderSyncJob{};
  autoSyncPullJobPending = false;
}

void EpubReaderActivity::maybeAutoPushOnSleep() {
  if (epub != nullptr && section != nullptr) {
    AUTOSYNC_STATE.noteProgress(epub->getPath(), currentSpineIndex, section->currentPage);
  }
  AUTOSYNC_STATE.flush();

  if (!activityManager.inSleepTransition() || !KOReaderAutoSync::sleepPushEnabled() || !AUTOSYNC_STATE.isPending()) {
    return;
  }
  if (footnoteDepth > 0) {
    LOG_DBG("AutoSync", "Footnote open at sleep; push deferred to the next boundary");
    return;
  }
  if (!epub || !section) {
    return;
  }
  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  if (page < 0 || total <= 0) {
    return;
  }

  saveProgress(currentSpineIndex, page, total);
  const KOReaderPosition local = currentKoPosition(page, total);
  KOReaderAutoSync::stashSleepPush(epub->getPath(), local, currentSpineIndex, page, epub->getTitle(),
                                   epub->getAuthor());
}

#endif
