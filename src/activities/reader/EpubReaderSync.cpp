// KOReader sync handoff for EpubReaderActivity: launching a sync session (compare / pull / push /
// auto-push-on-close) and applying a completed session's result back into live reader state. Split
// out of EpubReaderActivity.cpp because this has almost no coupling to the render-pass state
// machine that dominates that file — it only touches navigation/position state and APP_STATE's
// koReaderSyncSession.
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "KOReaderCredentialStore.h"

#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

namespace {
#if DEBUG_MEMORY_CONSUMPTION
void logReaderMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ERS", "Reader mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}
#else
inline void logReaderMemSnapshot(const char*) {}
#endif
}  // namespace

void EpubReaderActivity::launchKOReaderSync(const SyncLaunchMode mode, const SyncPositionOverride* positionOverride,
                                            const SyncPostAction postAction) {
  if (!epub) {
    return;
  }

  int currentPage = positionOverride ? positionOverride->page : (section ? section->currentPage : 0);
  int totalPages = positionOverride ? positionOverride->pageCount : (section ? section->pageCount : 0);

  // An override only ever comes from the finished-book flow, so it always means "the reader
  // reached the end of this book". Its call sites can usually say which page that was, but not
  // when there is no live section left to ask (renderFinishedBookPass, and the guarded fallbacks
  // in the other two), and they then hand over 0/0 — the same end-of-book sentinel they write to
  // progress.bin, where a page count of zero plus 100% is understood locally as "finished".
  //
  // ProgressMapper does not read it that way. It derives intra-spine progress as
  // page / (pageCount - 1), so 0/0 becomes 0.0 and the upload describes the START of the final
  // chapter. Field log of exactly that: a book finished at "Chapter 20, Page 4 (100%)" went up as
  // "spine=20 progress=0.000 -> /body/DocFragment[21]/body/p[1]/span[1]/b[1]", 98.85%.
  //
  // Any pair with page == pageCount - 1 encodes "the end of this spine", which is what finishing
  // the book means, so use the smallest one.
  if (positionOverride && totalPages <= 0) {
    currentPage = 1;
    totalPages = 2;
    LOG_DBG("ERS", "Finished-book sync has no page count; syncing the end of spine %d", positionOverride->spineIndex);
  }
  KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE;
  if (mode == SyncLaunchMode::PULL_REMOTE) {
    syncIntent = KOReaderSyncIntentState::PULL_REMOTE;
  } else if (mode == SyncLaunchMode::PUSH_LOCAL) {
    syncIntent = KOReaderSyncIntentState::PUSH_LOCAL;
  } else if (mode == SyncLaunchMode::AUTO_PUSH) {
    syncIntent = KOReaderSyncIntentState::AUTO_PUSH;
  }

  auto& sync = APP_STATE.koReaderSyncSession;
  sync.active = true;
  sync.epubPath =
      (positionOverride && !positionOverride->bookPath.empty()) ? positionOverride->bookPath : epub->getPath();
  sync.spineIndex = positionOverride ? positionOverride->spineIndex : currentSpineIndex;
  sync.page = currentPage;
  sync.totalPagesInSpine = totalPages;
  // Populate paragraph index and XHTML seek hint from section LUT if available. Not applicable
  // (and not attempted) for an overridden position — see the SyncPositionOverride comment.
  if (section && !positionOverride) {
    if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
      sync.paragraphIndex = *pIdx;
      sync.hasParagraphIndex = true;
      if (const auto hint = section->getXhtmlByteOffsetForPage(static_cast<uint16_t>(currentPage))) {
        sync.xhtmlSeekHint = *hint;
      } else {
        sync.xhtmlSeekHint = 0;
      }
    } else {
      sync.paragraphIndex = 0;
      sync.hasParagraphIndex = false;
      sync.xhtmlSeekHint = 0;
    }
  } else {
    sync.paragraphIndex = 0;
    sync.hasParagraphIndex = false;
    sync.xhtmlSeekHint = 0;
  }
  sync.intent = syncIntent;
  sync.outcome = KOReaderSyncOutcomeState::PENDING;
  sync.resultSpineIndex = 0;
  sync.resultPage = 0;
  sync.resultParagraphIndex = 0;
  sync.resultHasParagraphIndex = false;
  // Reset here (rather than trusting whatever the struct already held) so a stale destination
  // from a prior run cannot steal the user to the wrong place; every caller states what it wants,
  // defaulting to Reader (the pre-existing behavior for reader-menu-triggered syncs).
  sync.postAction = postAction.action;
  sync.postActionTarget = postAction.target;
  APP_STATE.saveToFile();

  LOG_DBG("ERS", "Standalone sync handoff: spine=%d page=%d/%d", sync.spineIndex, currentPage, totalPages);
  logReaderMemSnapshot("before_replace_with_sync");
  activityManager.goToKOReaderSync();
}

bool EpubReaderActivity::onFinishedBookSyncRequested(void* ctx, const std::string& bookPath,
                                                     const KOReaderSyncPostAction postAction,
                                                     const std::string& target) {
  auto* self = static_cast<EpubReaderActivity*>(ctx);
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("ERS", "Finished-book sync requested without credentials; leaving navigation to the caller");
    return false;
  }
  // launchKOReaderSync() silently does nothing without a live Epub, and since the sync path
  // replaces the finished-book flow's own navigation, that would strand the user. Check the same
  // condition here so the caller can fall back instead.
  if (!self->epub) {
    LOG_ERR("ERS", "Finished-book sync requested with no Epub loaded; leaving navigation to the caller");
    return false;
  }
  const SyncPositionOverride position{self->finishedBookSyncSpineIndex_, self->finishedBookSyncPage_,
                                      self->finishedBookSyncPageCount_, bookPath};
  self->launchKOReaderSync(SyncLaunchMode::PUSH_LOCAL, &position, {postAction, target});
  return true;
}

bool EpubReaderActivity::tryAutoPushOnClose() {
  // A page minimum filters out brief inspections — opening to check the cover or skim the TOC
  // shouldn't burn a network round-trip. Counter is per-activity-instance. The threshold is a
  // user setting because what counts as "a real session" differs between someone reading a
  // couple of pages at a time and someone reading in long sittings.
  if (!SETTINGS.koSyncOnBookClose) {
    return false;
  }
  if (!KOREADER_STORE.hasCredentials()) {
    return false;
  }
  const int minSessionPages = SETTINGS.koSyncMinSessionPages > 0 ? SETTINGS.koSyncMinSessionPages : 1;
  if (sessionPagesAdvanced < minSessionPages) {
    LOG_DBG("ERS", "Skipping AUTO_PUSH: %d pages this session, threshold is %d", sessionPagesAdvanced, minSessionPages);
    return false;
  }
  if (!epub) {
    return false;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0 || currentSpineIndex >= spineCount || !section) {
    LOG_DBG("ERS", "Skipping AUTO_PUSH on end-of-book sentinel: spine=%d section=%s", currentSpineIndex,
            section ? "present" : "null");
    return false;
  }

  // Reader-close auto-push has no reader session left to return to once it completes.
  launchKOReaderSync(SyncLaunchMode::AUTO_PUSH, nullptr, {KOReaderSyncPostAction::Home, {}});
  return true;
}

void EpubReaderActivity::applyPendingSyncSession() {
  auto& sync = APP_STATE.koReaderSyncSession;
  if (!sync.active || !epub || sync.epubPath != epub->getPath()) {
    return;
  }

  LOG_DBG("ERS", "Applying pending sync session outcome=%d path=%s", static_cast<int>(sync.outcome),
          sync.epubPath.c_str());

  // Upload-complete returns to the same local position the reader already persisted
  // before sync launched, so there is no need to rewrite progress.bin here.
  if (sync.outcome == KOReaderSyncOutcomeState::UPLOAD_COMPLETE) {
    LOG_DBG("ERS", "Upload-complete resume keeps existing local progress.bin unchanged");
    sync.clear();
    APP_STATE.saveToFile();
    logReaderMemSnapshot("after_apply_pending_sync_session");
    return;
  }

  // AUTO_PULL handed off zeroed local state (the reader was not yet running when sync started),
  // so on cancel/fail we must NOT restore those zeros to progress.bin — they would clobber the
  // user's real local progress. Just clear the session and let the normal startup load progress.bin.
  if (sync.intent == KOReaderSyncIntentState::AUTO_PULL && sync.outcome != KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    LOG_DBG("ERS", "AUTO_PULL non-success outcome=%d: leaving progress.bin untouched", static_cast<int>(sync.outcome));
    sync.clear();
    APP_STATE.saveToFile();
    logReaderMemSnapshot("after_apply_pending_sync_session");
    return;
  }

  int restoreSpineIndex = sync.spineIndex;
  int restorePage = sync.page;

  if (restoreSpineIndex < 0 || restoreSpineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid sync restore spine index %d, resetting to 0", restoreSpineIndex);
    restoreSpineIndex = 0;
    restorePage = 0;
  }

  // Build the navigation target from the sync result. For LUT-anchored targets the
  // estimated restorePage is plumbed through as fallbackPage so a LUT miss in the
  // target spine still lands the user on a sensible page rather than page 0.
  NavigationTarget restoreTarget;
  if (sync.outcome == KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    const int spineCount = epub->getSpineItemsCount();
    if (sync.resultSpineIndex < 0 || sync.resultSpineIndex >= spineCount) {
      LOG_ERR("ERS", "Sync resultSpineIndex %d out of range [0,%d), clamping to previous %d", sync.resultSpineIndex,
              spineCount, restoreSpineIndex);
      // Keep restoreSpineIndex / restorePage from the pre-validation block above.
    } else {
      restoreSpineIndex = sync.resultSpineIndex;
      restorePage = sync.resultPage;
    }
    if (sync.resultHasListItemIndex) {
      restoreTarget = NavigationTarget::makeListItem(sync.resultListItemIndex, restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d li[%u]", restoreSpineIndex, restorePage,
              sync.resultListItemIndex);
    } else if (sync.resultHasParagraphIndex) {
      restoreTarget = NavigationTarget::makeParagraph(sync.resultParagraphIndex, restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d p[%u]", restoreSpineIndex, restorePage,
              sync.resultParagraphIndex);
    } else {
      restoreTarget = NavigationTarget::makePage(restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d (no LUT)", restoreSpineIndex, restorePage);
    }
  } else {
    restoreTarget = NavigationTarget::makePage(restorePage);
    LOG_DBG("ERS", "Restored local pre-sync position: spine=%d page=%d", restoreSpineIndex, restorePage);
  }

  // sync.totalPagesInSpine is the page count of the local spine at launch time.
  // When the restore targets a different spine, that count is meaningless for
  // rescaling the fallbackPage estimate (which was estimated from cross-spine
  // density anyway). Store 0 to disable rescaling — the LUT lookup is the precise
  // path, and the cross-spine fallback can't usefully be rescaled here.
  const int restorePageCount = (restoreSpineIndex == sync.spineIndex) ? sync.totalPagesInSpine : 0;
  restoreTarget.cachedPageCount = restorePageCount;
  restoreTarget.cachedSpineIdx = restoreSpineIndex;

  // Seed live state directly — the previous write-then-reload-from-disk pattern relied
  // on progress.bin being read after this function ran, which clobbered the LUT target.
  // Live-state seeding is authoritative; the persistent write below is just for crash
  // recovery so a power loss before the next saveProgress() doesn't lose the synced
  // spine/page. The next render's saveProgress() supplies the real percent before
  // the user can return to the home screen.
  currentSpineIndex = restoreSpineIndex;
  navTarget = restoreTarget;
  if (!writeReaderProgressCache(epub->getCachePath(), restoreSpineIndex, restorePage, restorePageCount, 0)) {
    LOG_ERR("ERS", "Failed to persist sync restore to progress.bin; live state still seeded");
  } else {
    LOG_DBG("ERS", "Prepared progress.bin for sync restore: spine=%d page=%d/%d", restoreSpineIndex, restorePage,
            sync.totalPagesInSpine);
  }

  sync.clear();
  APP_STATE.saveToFile();
  logReaderMemSnapshot("after_apply_pending_sync_session");
}
