#include "HomeActivity.h"

#include <Bitmap.h>
#include <CooperativeAbort.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalCapabilities.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <ImageToBmp.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "HomeClockCadence.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "TouchUi.h"
#include "UiLanguagePacks.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "components/themes/TapTargets.h"
#include "components/themes/lyra/HomeWidgetData.h"
#include "fontIds.h"
#include "util/SerialCommandGrammar.h"

// Builds the menu entry list in display order. Single source of truth for both loop() (which
// dispatches Confirm based on action) and render() (which draws labels/icons).
void HomeActivity::rebuildMenuEntries() {
  menuEntries.clear();
  menuEntries.reserve(6);  // all six shown, or five plus More
  const HomeMenuAvailability availability{.hasBookmarks = GLOBAL_BOOKMARKS.hasAny()};
  {
    // Before the menu: the theme's arrangement decides how many entries Home has places for, and
    // which destinations it shows itself.
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().prepareHome(availableDestinations(availability));
  }
  const uint32_t shownByTheme = GUI.homeShownDestinations(renderer, UITheme::getContentRect(renderer, true, false));
  collectHomeMenuEntries(HomeMenuPlacement::Home, availability, menuEntries, GUI.homeMenuCapacity(), shownByTheme);
  menuTrailing_ = homeMenuTrailingCount(menuEntries);
}

HomeLayout HomeActivity::homeLayout() const {
  return GUI.homeLayout(renderer, UITheme::getContentRect(renderer, true, false), static_cast<int>(menuEntries.size()),
                        menuTrailing_);
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  // The newest books still on the card, their metadata sidecars applied (RecentBooksStore::newestBooks).
  recentBooks = RECENT_BOOKS.newestBooks(static_cast<size_t>(std::max(0, maxBooks)));
  for (RecentBook& book : recentBooks) {
    // Check for a sidecar cover — takes priority over embedded cover.
    // Also catches books registered before sidecar support (empty coverBmpPath).
    const std::string sidecar = ReaderActivity::sidecarCoverPath(book.path);
    if (!sidecar.empty()) {
      const std::string bookCacheDir = ReaderActivity::bookCacheDir(book.path);
      const bool sidecarAlreadyStored =
          book.coverBmpPath == sidecar || (book.coverBmpPath.rfind(bookCacheDir + "/", 0) == 0 &&
                                           book.coverBmpPath.find("[HEIGHT]") != std::string::npos);
      LOG_DBG("HOME", "Sidecar for %s: stored=%s alreadyStored=%d", book.path.c_str(), book.coverBmpPath.c_str(),
              sidecarAlreadyStored ? 1 : 0);
      if (!sidecarAlreadyStored) {
        LOG_DBG("HOME", "Updating coverBmpPath to sidecar: %s", sidecar.c_str());
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, sidecar);
        book.coverBmpPath = sidecar;
      }
    }
  }
}

bool HomeActivity::shouldSkipPeriodicUpdate() const {
  // The minute the clock would show (HalClock::formatTime reads the same local time).
  const time_t now = time(nullptr);
  struct tm shown;
  localtime_r(&now, &shown);
  return !HomeClockCadence::redraws(millis() - activityManager.lastUserActivityMs(), shown.tm_min);
}

bool HomeActivity::onClockMinute() {
  // The theme reads the calendar and the week's reading when Home opens. Left up through midnight,
  // or with the clock set meanwhile, they are read again for the new day. Local midnight is a
  // multiple of ten minutes, so the idle pace never skips that redraw.
  const uint16_t day = currentLocalDayIndex();
  if (day == shownDay_) return showsClock_;
  shownDay_ = day;
  const Rect content = UITheme::getContentRect(renderer, true, false);
  RenderLock lock;
  return UITheme::getInstance().getMutableTheme().refreshHomeDay(renderer, content) || showsClock_;
}

bool HomeActivity::coverAttemptsExhausted(const std::string& path) const {
  const auto it = coverTransientAttempts.find(path);
  return it != coverTransientAttempts.end() && it->second >= COVER_MAX_TRANSIENT_ATTEMPTS;
}

void HomeActivity::giveUpCover(RecentBook& book, ThumbResult res) {
  // Only transient failures count toward the session budget; a structural absence is already
  // permanent (generateThumbBmp wrote a sentinel) and needs no retry accounting.
  if (res == ThumbResult::TransientFail) {
    const uint8_t n = ++coverTransientAttempts[book.path];
    LOG_DBG("HOME", "Transient cover failure %u/%u for %s", n, COVER_MAX_TRANSIENT_ATTEMPTS, book.path.c_str());
  }

  // Permanent give-up = structurally absent, or transient failures past this session's budget
  // (e.g. an embedded cover the decoder rejects every time — oversize PNG, corrupt image). The loader
  // writes a valid placeholder BMP at each size it makes, so isCoverThumbComplete() treats the book
  // as resolved on disk and it is never re-decoded on the next boot. A still-retryable transient
  // failure records an empty cover instead, so it gets a fresh attempt next session.
  const bool permanent = (res == ThumbResult::StructurallyAbsent) || coverAttemptsExhausted(book.path);

  if (permanent) {
    if (coverLoader.writeNoCoverPlaceholders(book.path)) {
      const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
      LOG_DBG("HOME", "Wrote cover placeholder(s) for %s — resolved, will not re-decode", book.path.c_str());
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
      book.coverBmpPath = placeholder;
      return;
    }
    // Placeholder write failed (e.g. tight heap) — fall through to empty; retry next pass.
    LOG_DBG("HOME", "Placeholder write failed for %s — recording empty, will retry", book.path.c_str());
  }

  RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
  book.coverBmpPath = "";
}

void HomeActivity::loadRecentCovers(const int coverHeight, const std::pair<int, int>* thumbSizes,
                                    const int thumbSizeCount) {
  recentsLoading = true;

  // Cold-cache cover loading runs the JPEG/PNG decoders, the sliced ZIP inflate and
  // an EPUB parse — together these can drive free heap to the edge of OOM (observed
  // Min Free ~2.8 KB) while the ~48 KB secondary framebuffer sits unused. The reader
  // releases that buffer before decoding for the same reason; do the same here for the
  // duration of loading and reallocate it once every cover is resolved (see end of this
  // function) or on exit. Release under the render lock so we never free it mid-render.
  if (secondaryBufferLent && frameCacheInRegion_) {
    // A later pass on the same visit: the carousel's frame cache has the region; the decoders
    // need it back. (The cache would be rebuilt anyway -- new covers are about to land.)
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().setFrameCacheRegion(nullptr, 0);
    frameCacheInRegion_ = false;
    coverScratch_ = makeUniqueNoThrow<BuildArena>(lentRegion_, lentRegionBytes_);
    if (!coverScratch_ || !coverScratch_->valid()) coverScratch_.reset();
  } else if (!secondaryBufferLent && renderer.hasSecondaryBuffer()) {
    RenderLock lock;
    // The lend hands out the DISPLAYED frame and leaves the write buffer as the only one, and
    // after the home render's swap that buffer holds the frame from two refreshes ago -- the
    // boot screen, or whatever screen preceded Home. Unless Home repaints before it exits, the
    // return below seeds the secondary from that stale frame, and the next overlay (the reader's
    // "Indexing" popup, the cold-font popup, anything drawn with drawPopup) lands on it. Copy the
    // displayed frame in while the secondary still holds it.
    renderer.syncWriteBufferFromDisplayed();
    // Seed RED RAM with the displayed frame too, which the SDK requires before single-buffer fast
    // diff. A two-buffer FAST loads RED from the previous frame when it STARTS and leaves it
    // there, so RED now holds whatever preceded Home. Unseeded, the first refresh after the lend
    // -- a carousel move, or Settings' entry frame when Home exits without redrawing -- skips
    // every pixel the new frame shares with that older screen, and Home's ink stays on the glass
    // there. Runs before the borrow, while the secondary still holds the displayed frame.
    renderer.syncRedRamFromFrameBuffer();
    size_t lentSize = 0;
    if (uint8_t* lent = renderer.borrowSecondaryBuffer(&lentSize)) {
      coverScratch_ = makeUniqueNoThrow<BuildArena>(lent, lentSize);
      if (coverScratch_ && coverScratch_->valid()) {
        secondaryBufferLent = true;
        lentRegion_ = lent;
        lentRegionBytes_ = lentSize;
        // Keep X4 fast-differential refresh alive while the secondary buffer is lent: RED RAM
        // was seeded with the home frame above and the driver re-seeds it after every
        // single-buffer refresh, so carousel/menu navigation diffs
        // against that baseline instead of downgrading to a full/half waveform on every press.
        // Precondition holds: we lend right after the first home render (gate requires
        // firstRenderDone) and only issue plain BW redraws until the return. No-op on X3.
        renderer.setSingleBufferFastDiff(true);
        LOG_DBG("HOME", "Lent secondary framebuffer for cover loading (%u bytes, free=%lu)",
                static_cast<unsigned>(lentSize), static_cast<unsigned long>(esp_get_free_heap_size()));
      } else {
        coverScratch_.reset();
        renderer.returnSecondaryBuffer();
      }
    }
  }

  // A frame being drawn shares the core (on the C3 the render task has the loop task's priority)
  // and the SD card with this pass. Bursting beside it stretched the redraw a press asked for
  // from ~360 ms to 0.6-1.1 s (X3, 2026-09-27): the sliced sessions resume after a press instead
  // of restarting, so -- unlike the one-shot decode they replaced -- they were still running when
  // the redraw began. Step aside until the frame is handed to the panel; the ~430 ms refresh
  // after that needs neither, and the pass keeps that window.
  const auto frameInProgress = [this]() { return renderer.isComposingFrame(); };
  if (frameInProgress()) {
    recentsLoading = false;
    delay(2);  // block rather than spin: a spinning loop task still takes its share of the core
    return;
  }

  // HomeActivity::loop runs on the main task while rendering runs on the render task.
  // Invalidate the Lyra frame cache under the render lock to avoid freeing cached
  // frames while tryFastHomeRender is copying from them.
  const auto invalidateFrameCacheSafely = []() {
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().invalidateFrameCache();
  };

  // Called at every early-return point where a cover was just written to disk.
  // Marks the carousel frame cache dirty so the next tryFastHomeRender rebuilds
  // with the new BMP.  Uses markFrameCacheDirty() rather than invalidateFrameCache()
  // so multiple covers arriving between renders coalesce into one SD re-read
  // instead of triggering a full cache rebuild (3 BMP file opens) for each book.
  const auto yieldAfterDecode = [this]() {
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().markFrameCacheDirty();
    coverRendered = false;
    recentsLoading = false;
    requestUpdate();
  };

  // Time budget for a single loadRecentCovers() call. Rather than advancing a session by
  // one small slice per loop() tick (a 1200x1848 PNG cover needs ~940 ZIP-inflate chunks +
  // ~308 decode-row batches ≈ 1250 ticks — minutes of wall-clock), we drain slices in a
  // burst until this budget elapses or button input arrives. Browse Files' cover pass
  // bursts the same way. A slice only writes SD files (no screen change), so
  // bursting costs nothing visually; we still yield promptly to keep input responsive.
  constexpr uint32_t COVER_SLICE_BUDGET_MS = 150;
  // The pass's thumbnails: the carousel's two boxes, cropped, or the single-height form the other
  // themes draw. Set once per height, not per call: configure() abandons the book in hand.
  if (coverLoaderHeight_ != coverHeight) {
    if (thumbSizeCount == 0) {
      coverLoader.configureSingleHeight(coverHeight);
    } else {
      coverLoader.configure(thumbSizes, thumbSizeCount, /*crop=*/true);
    }
    coverLoaderHeight_ = coverHeight;
  }

  // One book at a time, through the shared CoverThumbLoader: every size of it from one decode where
  // the cover allows (R9 item 2), sliced so a press pauses the work where it stands (item 3). This
  // screen keeps only what is its own: the order of the books, the per-session retry budget, the
  // recent-books record, and the frame cache.
  while (true) {
    if (!coverLoader.busy()) {
      // The next book whose cover is not on the card yet.
      for (; nextRecentCoverIndex < recentBooks.size(); nextRecentCoverIndex++) {
        RecentBook& book = recentBooks[nextRecentCoverIndex];
        if (!Storage.exists(book.path.c_str())) continue;
        if (!coverLoader.complete(book.path)) break;
        // Already present -- make sure the stored path is the canonical placeholder so a stale
        // "[HEIGHT].bmp" / raw-sidecar entry self-heals to the unified naming without a re-decode.
        const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
        if (book.coverBmpPath != placeholder) {
          LOG_DBG("HOME", "Self-heal: coverBmpPath=%s -> placeholder=%s", book.coverBmpPath.c_str(),
                  placeholder.c_str());
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
          book.coverBmpPath = placeholder;
        }
      }
      if (nextRecentCoverIndex >= recentBooks.size()) break;  // every cover resolved

      // Button input has priority: never start a decode while a press is queued, or while the
      // frame it asked for is being drawn. The next pass picks the same book up.
      if (mappedInput.hasPendingInput() || frameInProgress()) {
        recentsLoading = false;
        return;
      }
      RecentBook& book = recentBooks[nextRecentCoverIndex];
      // This book already burned its transient-failure budget this session -- stop retrying it (a
      // reboot resets the counter and tries again) so it cannot starve the others.
      if (coverAttemptsExhausted(book.path)) {
        LOG_DBG("HOME", "Cover attempts exhausted for %s this session -- writing placeholder", book.path.c_str());
        giveUpCover(book, ThumbResult::StructurallyAbsent);
        nextRecentCoverIndex++;
        yieldAfterDecode();
        return;
      }
      // Cover decode needs ~42 KB contiguous heap -- free the frame cache first.
      invalidateFrameCacheSafely();
      coverBookPath_ = book.path;
      coverLoader.begin(book.path, coverScratch_.get());
    }

    // A burst on the book in hand. A step only writes SD files, so the screen is redrawn once,
    // when the book is resolved -- not per step.
    const uint32_t deadline = millis() + COVER_SLICE_BUDGET_MS;
    CoverThumbLoader::Step step = CoverThumbLoader::Step::Working;
    while ((step = coverLoader.step()) == CoverThumbLoader::Step::Working) {
      if (mappedInput.hasPendingInput() || frameInProgress() || static_cast<int32_t>(millis() - deadline) >= 0) {
        recentsLoading = false;  // resume where it stands on the next call
        return;
      }
    }
    // Found by path: a swipe that cycled the books while this one was in hand moved it
    // (cycleRecentBooks), and the position the pass started from now holds another book.
    const auto inHand = std::find_if(recentBooks.begin(), recentBooks.end(),
                                     [this](const RecentBook& b) { return b.path == coverBookPath_; });
    if (inHand == recentBooks.end()) {
      recentsLoading = false;
      return;
    }
    RecentBook& book = *inHand;
    const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
    if (step == CoverThumbLoader::Step::Done) {
      // A cover written, or the placeholder of a book that has none: the stored path is canonical.
      if (book.coverBmpPath != placeholder) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
        book.coverBmpPath = placeholder;
      }
    } else {
      // Counts the transient; writes a durable placeholder once the budget is spent.
      giveUpCover(book, ThumbResult::TransientFail);
    }
    // Past the book in hand when it is where the pass left off. After a cycle the pass starts over
    // from the front instead, and skips whatever is complete by then.
    if (nextRecentCoverIndex < recentBooks.size() && &recentBooks[nextRecentCoverIndex] == &book) {
      nextRecentCoverIndex++;
    }
    yieldAfterDecode();
    return;
  }

  // Every cover is resolved. The pictures Home's widgets show come next, one a call, while the
  // scratch is still lent: a JPEG needs the decoder's buffers as a cover does.
  switch (makeNextHomePicture()) {
    case PictureStep::Made:
      yieldAfterDecode();
      return;
    case PictureStep::Paused:
      recentsLoading = false;  // the press is served first; the next pass takes the picture up again
      return;
    case PictureStep::Done:
      break;
  }

  recentsLoaded = true;
  recentsLoading = false;
  if (!keepRegionAsFrameCache()) restoreSecondaryBuffer();
}

bool HomeActivity::nextHomePictureJob(HomePictureJob& out) const {
  HomePictureJob jobs[kMaxPictureJobs];
  const int count =
      GUI.homePictureJobs(renderer, UITheme::getContentRect(renderer, true, false), jobs, kMaxPictureJobs);
  for (int i = 0; i < count; ++i) {
    const uint32_t key = homePictureKey(jobs[i].target);
    const uint32_t* const tried = pictureAttempts_ + pictureAttemptCount_;
    if (std::find(pictureAttempts_, tried, key) != tried) continue;
    if (ReaderActivity::isCoverThumbComplete(jobs[i].target, jobs[i].width, jobs[i].height)) continue;
    out = std::move(jobs[i]);
    return true;
  }
  return false;
}

HomeActivity::PictureStep HomeActivity::makeNextHomePicture() {
  HomePictureJob job;
  if (!nextHomePictureJob(job)) return PictureStep::Done;
  // Button input first, as for a cover: never start a decode while a press is queued or its frame is
  // being drawn.
  if (mappedInput.hasPendingInput() || renderer.isComposingFrame()) return PictureStep::Paused;
  // Once a visit, made or not: a picture that comes out other than asked is not made on every tick.
  // The list holds as many as a placement has jobs (kMaxPictureJobs).
  const bool noted = pictureAttemptCount_ < kMaxPictureJobs;
  if (noted) pictureAttempts_[pictureAttemptCount_++] = homePictureKey(job.target);
  const std::string dir = job.target.substr(0, job.target.rfind('/'));
  if (!Storage.exists(dir.c_str())) Storage.mkdir(dir.c_str());
  // Written under another name and renamed when whole: the theme draws the target on the render
  // task meanwhile, and must not find half a picture.
  const std::string part = job.target + ".part";
  // The one-shot decode gives way to a press (CooperativeAbort). That is no failure: the picture
  // stayed on Loading for the rest of the visit when it was taken for one.
  CooperativeAbort::clearAborted();
  bool ok = ImageToBmp::convertFile(
      "HOME", job.source, part,
      ImageToBmp::Target{.width = job.width, .height = job.height, .crop = true, .scratch = coverScratch_.get()});
  const bool aborted = CooperativeAbort::consumeAborted();
  if (ok) {
    Storage.remove(job.target.c_str());
    ok = Storage.rename(part.c_str(), job.target.c_str());
  }
  if (ok) {
    LOG_INF("HOME", "Picture made: %s -> %s", job.source.c_str(), job.target.c_str());
    removeReplacedHomePictures(job.target);
    return PictureStep::Made;
  }
  Storage.remove(part.c_str());
  if (aborted) {
    if (noted) --pictureAttemptCount_;  // the key just added: tried again on the next pass
    LOG_DBG("HOME", "Picture paused for input: %s", job.source.c_str());
    return PictureStep::Paused;
  }
  LOG_ERR("HOME", "Picture not made: %s (%dx%d)", job.source.c_str(), job.width, job.height);
  {
    // Drawn as not made rather than as Loading, and decoded again on a later visit only up to
    // HomePictureFailures::kTries times this start.
    RenderLock lock;
    homePictureFailures().add(job.target);
  }
  return PictureStep::Made;
}

bool HomeActivity::keepRegionAsFrameCache() {
  // Every cover is resolved, so the region would go back to the display now -- where, on the
  // carousel, it leaves Home without a frame cache: the cache wants one ~49 KB block (X3; 45 KB
  // X4), which Home's heap never has with the buffer resident (run 16: "cover region 0 (49104
  // bytes, 36108 free)" on every render, then "OOM: cover buffer (20592 bytes)" for the fallback),
  // so every render redrew the three covers from SD: ~360 ms against ~40 ms restored from a cache.
  // The region is idle for the rest of the visit, so the cache lives there instead. The display
  // keeps working as it does during the cover pass (single-buffer fast diff).
  if (!secondaryBufferLent || lentRegion_ == nullptr || frameCacheInRegion_) return false;
  if (coverLoader.busy()) return false;  // nothing may still hold a block
  auto& theme = UITheme::getInstance().getMutableTheme();
  const size_t wanted = theme.frameCacheRegionBytes(renderer);
  if (wanted == 0 || wanted > lentRegionBytes_) return false;
  RenderLock lock;
  coverScratch_.reset();
  theme.setFrameCacheRegion(lentRegion_, lentRegionBytes_);
  frameCacheInRegion_ = true;
  // The fallback cover buffer (20 592 B on the X3) is dead weight from here on: the fast path
  // serves every render. Left allocated it held Home at ~28 KB free for the rest of the visit
  // instead of ~49 KB (X3, 2026-09-27). If the region goes back early the fallback re-stores it.
  freeCoverBuffer();
  LOG_DBG("HOME", "Kept the lent framebuffer as the carousel frame cache (%u of %u bytes, free=%lu)",
          static_cast<unsigned>(wanted), static_cast<unsigned>(lentRegionBytes_),
          static_cast<unsigned long>(esp_get_free_heap_size()));
  return true;
}

void HomeActivity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  restoreSecondaryBuffer(/*callerHoldsRenderLock=*/false);
  Activity::startActivityForResult(std::move(activity), std::move(resultHandler));
}

void HomeActivity::restoreSecondaryBuffer(bool callerHoldsRenderLock) {
  // Reallocate the framebuffer we released for cover loading. Best-effort: if malloc
  // fails (heap still tight) we stay degraded and retry on the next call — rendering
  // tolerates a missing secondary buffer (full/half refresh instead of fast AA).
  //
  // The RenderLock is non-recursive. onExit() already runs UNDER the lock (held by
  // ActivityManager::exitActivity), so it must pass callerHoldsRenderLock=true — taking
  // a second RenderLock there self-deadlocks and hangs the Home→Reader transition. The
  // end-of-loading caller runs from loop() with no lock held and passes false.
  if (!secondaryBufferLent) return;
  const auto doRestore = [this]() {
    // Anything still holding a block in the lent region goes first: an abandoned extract, PNG or
    // JPEG session at exit would otherwise release into a region the display owns again.
    coverLoader.reset();
    coverScratch_.reset();
    if (frameCacheInRegion_) {
      UITheme::getInstance().getMutableTheme().setFrameCacheRegion(nullptr, 0);
      frameCacheInRegion_ = false;
    }
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    secondaryBufferLent = false;
    lentRegion_ = nullptr;
    lentRegionBytes_ = 0;
    // Two-buffer differential is available again — turn off the single-buffer RED-RAM-baseline
    // mode so normal fast refresh resumes against the secondary. No syncRedRamFromFrameBuffer()
    // here: the return re-seeds the baseline exactly as a realloc does, and RED already holds the
    // home frame -- seeded at the lend, and re-seeded by every single-buffer refresh since.
    renderer.setSingleBufferFastDiff(false);
    LOG_DBG("HOME", "Returned secondary framebuffer after cover loading (free=%lu contig=%lu)",
            static_cast<unsigned long>(esp_get_free_heap_size()),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  };
  if (callerHoldsRenderLock) {
    doRestore();
  } else {
    RenderLock lock;
    doRestore();
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // A finished-book "sync to KOReader, then search OPDS for this author" request lands here
  // first, after the sync's restart on the C3s or its goHome() in the same boot on the PSRAM boards
  // (KOReaderSyncActivity::onExit(), wifi_session::end()) — there's no reader to hand it to the way
  // the OpenBook/Reader post-actions can via APP_STATE.openEpubPath, so Home is what consumes it.
  // replaceActivity() defers this to the next loop() tick rather than re-entering while this
  // activity is still being constructed (see the "delete this" guard in ActivityManager).
  auto& pendingSync = APP_STATE.koReaderSyncSession;
  if (pendingSync.postAction == KOReaderSyncPostAction::OpdsSearch) {
    const std::string author = pendingSync.postActionTarget;
    pendingSync.clear();
    APP_STATE.saveToFile();
    if (!author.empty() && OPDS_STORE.hasServers()) {
      activityManager.goToBrowserWithSearch(author);
      return;
    }
  }

  selectorIndex = 0;
  recentsLoading = false;
  recentsLoaded = false;
  firstRenderDone = false;
  nextRecentCoverIndex = 0;
  coverLoader.reset();
  coverTransientAttempts.clear();
  coverRendered = false;
  secondaryBufferLent = false;
  lentRegion_ = nullptr;
  lentRegionBytes_ = 0;
  frameCacheInRegion_ = false;
  freeCoverBuffer();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  // The themes draw each recent book's history and pace from the stats cache; fill it here, on the
  // loop task, so their render() reads no file. A warm cache makes this free.
  std::vector<std::string> recentDocIds;
  recentDocIds.reserve(recentBooks.size());
  for (const auto& book : recentBooks) recentDocIds.push_back(KOReaderDocumentId::calculateFromFilename(book.path));
  READING_STATS.prefetchRecent(recentDocIds);
  pictureAttemptCount_ = 0;
  // The day before the theme reads its widgets for it (prepareHome, in the rebuild below): one that
  // turns in between is then caught at the next minute (onClockMinute) rather than missed.
  shownDay_ = currentLocalDayIndex();
  // Built here, once per entry, before the first render is requested. render() runs on the render
  // task and loop() on this one; when both rebuilt the list on a dirty flag, they could run at once,
  // each clearing and then appending, and Home showed every entry twice. The rebuild also reads the
  // bookmark index off the card.
  rebuildMenuEntries();
  {
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().setHomeMenu(static_cast<int>(menuEntries.size()), menuTrailing_);
  }
  shownBooks_ = std::min(static_cast<int>(recentBooks.size()), homeLayout().bookSlots);
  const Rect content = UITheme::getContentRect(renderer, true, false);
  grid_ = GUI.homeGrid(renderer, content, shownBooks_, static_cast<int>(menuEntries.size()), menuTrailing_);
  widgetActionCount_ = GUI.homeWidgetActions(renderer, content, widgetActions_, kMaxWidgetActions);
  showsClock_ = GUI.homeShowsClock(renderer, content);
  // Nothing for the cover pass to make: no book, and no picture a widget shows.
  if (recentBooks.empty()) {
    HomePictureJob jobs[kMaxPictureJobs];
    recentsLoaded = GUI.homePictureJobs(renderer, content, jobs, kMaxPictureJobs) == 0;
  }
  gridNav_ = grid_.rowCount > 0 && HalCapabilities::hasFourDirectionKeys();

  // Apply focus: book path takes priority, else combined selector index (covers
  // "return to the menu entry I was on").
  bool focused = false;
  if (!focusBookPath.empty()) {
    for (int i = 0; i < shownBooks_; ++i) {
      if (recentBooks[i].path == focusBookPath) {
        selectorIndex = i;
        focused = true;
        break;
      }
    }
    focusBookPath.clear();
  }
  if (!focused && focusSelectorIndex >= 0) {
    const int combinedSize = selectableCount();
    if (combinedSize > 0) {
      selectorIndex = std::min(focusSelectorIndex, combinedSize - 1);
    }
  }
  focusSelectorIndex = -1;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  // The cover-loading burst is over; release the one book's metadata the memo still holds.
  Epub::clearCoverMetadataMemo();
  Activity::onExit();
  freeCoverBuffer();
  // The clock and weather widgets draw scaled text, which takes the reader's scaled-glyph cache
  // (4.75 KB) on first use. Given back as a reader gives it back, so it does not stay under the
  // next screen: X3, kept, it cost Home 5 KB of free heap and the next reader its stable placement.
  renderer.releaseScaledGlyphCache();
  UITheme::getInstance().getMutableTheme().invalidateFrameCache();
  // Never hand the next activity a degraded display: if we exit mid-load (e.g. the
  // user opened a book before covers finished), put the secondary framebuffer back.
  // The reader will release it again itself if it needs the headroom.
  // onExit runs under the RenderLock held by ActivityManager::exitActivity, so we must
  // NOT take another (non-recursive → self-deadlock); pass callerHoldsRenderLock=true.
  restoreSecondaryBuffer(/*callerHoldsRenderLock=*/true);
}

bool HomeActivity::storeCoverBuffer() {
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  coverBufferInverted = renderer.isOutputInverted();
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

bool HomeActivity::offerLanguagePack() {
  const PackOffer::Offer offer = UiLanguagePacks::takeHomeOffer();
  if (!offer) return false;
  // Without the Font Manager, a fresh Home, not this one: opening the question took back the framebuffer this visit
  // had lent to the carousel's frame cache (startActivityForResult), and the next carousel render here would build
  // that 49 KB frame on the heap instead, leaving Home ~20 KB free (X3). A new Home lends it again.
  return PackOffer::ask(*this, renderer, mappedInput, offer, FontDownloadActivity::ReturnTo::Home,
                        [](Activity&) { activityManager.goHome(); });
}

void HomeActivity::loop() {
  // Ahead of the button handling: a tap that opens a book or a menu entry replaces or stacks on
  // this activity, and running the rest against a screen that is going away serves nobody.
  if (handleHomeTouch()) return;

  // The interface language's pack, when the card lacks it: once a start and language (UiLanguagePacks).
  if (firstRenderDone && offerLanguagePack()) return;

  const bool isCarousel = (GUI.getHomeNavigation() == HomeNavigation::Carousel);

  // A press may be in EITHER place: hasPendingInput() reads the live sampler accumulator
  // (presses arriving this tick), while wasAnyPressed()/wasAnyReleased() read the snapshot
  // that gpio.update() already drained the accumulator INTO at the top of the main loop —
  // BEFORE this loop() runs. Gating cover loading on hasPendingInput() alone therefore
  // starves the snapshot edge: the press sits unconsumed in snapPressed_ while the gate
  // keeps diverting to loadRecentCovers() every tick, and the next update() overwrites it.
  // Defer cover loading whenever input is waiting in either place so the handlers below run.
  const bool inputWaiting =
      mappedInput.hasPendingInput() || mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased();

  if (isCarousel) {
    if (firstRenderDone && !recentsLoaded && !recentsLoading && !inputWaiting) {
      const HomeLayout layout = homeLayout();
      loadRecentCovers(layout.coverHeight, layout.thumbSizes, layout.thumbSizeCount);
      return;
    }

    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuItemCount = static_cast<int>(menuEntries.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int menuIdx = inCarouselRow ? 0 : (selectorIndex - bookCount);

    // Up and Down both toggle between the carousel row and the menu row. They are
    // handled together, and as a single else-if chain with Left/Right, so that AT MOST
    // ONE direction acts per tick. Two row-toggle edges landing in the same drained
    // input batch (easy when a slow cover-loading tick lets the sampler accumulate
    // several edges) would otherwise both run against the stale `inCarouselRow` above:
    // the first sets selectorIndex = bookCount, the second then captures THAT into
    // lastCarouselBookIndex (= bookCount, an invalid carousel index), permanently
    // stranding the selector in the menu row — Left/Right still move the icon highlight
    // but Up/Down can never return to the carousel. One-direction-per-tick prevents it.
    const bool rowToggle = mappedInput.wasLogicalPressed(MappedInputManager::Direction::Up) ||
                           mappedInput.wasLogicalPressed(MappedInputManager::Direction::Down);

    // A horizontal swipe scrolls the carousel, which is the gesture the shape of this screen
    // invites: a strip of covers with one in front and its neighbours peeking out either side.
    //
    // Direction follows the reader's own mapping (ReaderUtils::detectTouchPageTurn) rather than
    // being invented here -- a swipe that travels right-to-left brings the NEXT item, the way
    // content follows the finger. Reusing the mapping matters because the two screens sit one
    // gesture apart: swiping to the next book and then swiping to the next page must not mean
    // opposite things.
    //
    // Folded into the same else-if chain as the buttons, not handled separately, so it inherits
    // the one-action-per-tick rule that chain exists for. Two directions resolved in one drained
    // input batch is exactly what once stranded the selector in the menu row.
    //
    // GestureEventManager has already run in main.cpp and consumes the contact when the user has
    // BOUND that swipe to an action, so a bound gesture still wins; only an unbound swipe -- the
    // default -- reaches this. wasSwipe() is silent on non-touch boards and while Touch
    // Navigation is off, so neither needs a guard of its own.
    const auto swipe = mappedInput.wasSwipe();
    const bool swipeNext = swipe == MappedInputManager::SwipeDir::Left;
    const bool swipePrev = swipe == MappedInputManager::SwipeDir::Right;

    if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Right) || swipeNext) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + 1) % menuItemCount;
      requestUpdate();
    } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Left) || swipePrev) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + menuItemCount - 1) % menuItemCount;
      requestUpdate();
    } else if (rowToggle) {
      if (inCarouselRow) {
        lastCarouselBookIndex = selectorIndex;  // selectorIndex < bookCount here — always valid
        selectorIndex = bookCount;              // jump to first menu entry
      } else {
        // Restore the remembered carousel slot, clamped in case it was never set or
        // the recents list shrank since it was recorded.
        selectorIndex = (bookCount > 0) ? std::min(std::max(lastCarouselBookIndex, 0), bookCount - 1) : 0;
      }
      requestUpdate();
    }
  } else {
    const int totalItems = selectableCount();

    // Ahead of the cover pass, which would otherwise take this tick and let the swipe go by. The
    // direction is the reader's (and the carousel's): a swipe to the left brings the next book.
    // wasSwipe() is silent on boards without touch and while Touch Navigation is off. Only where the
    // book card shows the book in front (shownBooks_): on a layout without one the swipe turned the
    // unseen list round and redrew Home for nothing.
    if (GUI.homeSwipeCyclesBooks() && shownBooks_ > 0) {
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right) {
        cycleRecentBooks(swipe == MappedInputManager::SwipeDir::Left ? 1 : -1);
        return;
      }
    }

    if (firstRenderDone && !recentsLoaded && !recentsLoading && !inputWaiting) {
      const HomeLayout layout = homeLayout();
      loadRecentCovers(layout.coverHeight, layout.thumbSizes, layout.thumbSizeCount);
      return;
    }

    if (gridNav_) {
      // Rows and columns (HomeGridNav): Up and Down between rows, Left and Right along one, and on
      // the book card Left and Right bring the previous or next book. One direction per tick, as
      // the carousel does, so two edges drained in one batch cannot both act on a stale position.
      using Move = HomeGridNav::Move;
      using Direction = MappedInputManager::Direction;
      bool pressed = true;
      Move move = Move::Down;
      if (mappedInput.wasLogicalPressed(Direction::Up)) {
        move = Move::Up;
      } else if (mappedInput.wasLogicalPressed(Direction::Down)) {
        move = Move::Down;
      } else if (mappedInput.wasLogicalPressed(Direction::Left)) {
        move = Move::Left;
      } else if (mappedInput.wasLogicalPressed(Direction::Right)) {
        move = Move::Right;
      } else {
        pressed = false;
      }
      if (pressed) {
        const HomeGridNav::Step step = HomeGridNav::step(grid_, selectorIndex, move);
        if (step.cycle != 0) {
          cycleRecentBooks(step.cycle);
        } else if (step.index != selectorIndex) {
          selectorIndex = step.index;
          requestUpdate();
        }
      }
    } else if (grid_.cellCount > 0) {
      // Up and Down only: through the theme's cells in screen order, which need not be the
      // selector's order (HomeGridNav::next).
      buttonNavigator.onNext([this] {
        selectorIndex = HomeGridNav::next(grid_, selectorIndex, 1);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        selectorIndex = HomeGridNav::next(grid_, selectorIndex, -1);
        requestUpdate();
      });
    } else {
      buttonNavigator.onNext([this, totalItems] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
        requestUpdate();
      });

      buttonNavigator.onPrevious([this, totalItems] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
        requestUpdate();
      });
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int recentsCount = shownBooks_;
    if (selectorIndex < recentsCount) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      dispatchSelection(selectorIndex - recentsCount);
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  const int menuCount = static_cast<int>(menuEntries.size());
  const bool isCarousel = (GUI.getHomeNavigation() == HomeNavigation::Carousel);

  // Fast path: theme owns its own pre-rendered frame cache
  if (isCarousel) {
    // The carousel scrolls along logical Left/Right and the row toggle sits on logical Up/Down, so
    // in landscape the front strip carries the toggle instead — mapHints puts the matching arrows
    // there. (Only the front strip is drawn; the side buttons are unlabelled on the home screen.)
    const auto carouselLabels =
        mappedInput.mapHints("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
            .front;
    const bool handled = GUI.tryFastHomeRender(
        renderer, recentBooks, selectorIndex, menuCount,
        [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
        [this](int index) { return menuEntries[index].icon; }, carouselLabels.btn1, carouselLabels.btn2,
        carouselLabels.btn3, carouselLabels.btn4);
    if (handled) {
      // Opens the cover-loading gate in loop(); see the note at the end of render() for
      // why this deliberately does NOT also request a second render.
      firstRenderDone = true;
      return;
    }
    // Fast path failed (e.g. frame cache malloc failed). Force cover re-render
    // so the fallback drawRecentBookCover below doesn't skip based on stale state.
    coverRendered = false;
    coverBufferStored = false;
  }

  renderer.clearScreen();
  // Dark Mode turned on or off over Home (the light drawer, the slide switch, a Scheduled boundary):
  // the snapshot holds the cover inverted for the other polarity. Ported from crosspoint-reader PR
  // #4329 (Mikalai Kryvusha).
  if (coverBufferStored && coverBufferInverted != renderer.isOutputInverted()) {
    freeCoverBuffer();
    coverRendered = false;
  }
  // Only while coverRendered still vouches for the stored pixels. A cover decoded since they were
  // stored clears it (yieldAfterDecode) but leaves the buffer, which still holds the placeholder:
  // restored, its edge showed through the new cover, whose white pixels do not overwrite.
  // Ported from crosspoint-reader PR #4076 (Leopoldo Pla Sempere).
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.homeTopPadding}, nullptr);

  // Books, menu, and the widgets numbered after it: clamping to the first two pulled a step onto a
  // widget back to the last menu entry.
  const int totalItems = selectableCount();
  if (selectorIndex >= totalItems) {
    selectorIndex = std::max(0, totalItems - 1);
  }

  const HomeLayout layout = homeLayout();

  coverRectX = layout.coverSnapshot.x;
  coverRectY = layout.coverSnapshot.y;
  coverRectW = layout.coverSnapshot.width;
  coverRectH = layout.coverSnapshot.height;

  // The carousel stores nothing: this draw is its fallback for a render its frame cache could not serve, and every
  // carousel render is either served by that cache or clears coverRendered (above) before it could restore, so the
  // copy was never read. It was a malloc of the tile region with no heap floor, which the X3 logged as "OOM: cover
  // buffer (20592 bytes)".
  GUI.drawRecentBookCover(renderer, layout.books, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                          bufferRestored, [this, isCarousel] { return !isCarousel && storeCoverBuffer(); });

  GUI.drawHomeMenu(
      renderer, layout.menu, menuCount, menuTrailing_, selectorIndex - shownBooks_,
      [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
      [this](int index) { return menuEntries[index].icon; });

  const auto labels =
      (isCarousel || gridNav_)
          ? mappedInput
                .mapHints("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                .front
          : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  // Opens the cover-loading gate in loop() (which requires firstRenderDone, so that covers are
  // read off SD only after something is already on screen).
  //
  // No requestUpdate() here. It used to post a second full render unconditionally, before
  // anything knew whether the cover pass would change a pixel; the render task serialises
  // behind that pass on the RenderLock, so it landed after it and repainted the same screen.
  // On a device whose thumbs are all cached -- the steady state -- that was a whole extra panel
  // update per Home entry for nothing (measured: 643 ms on the 960x540 S3, on top of the 1573 ms
  // first paint).
  //
  // Nothing is lost by dropping it:
  //   - a cover that IS decoded already repaints itself; every path in loadRecentCovers() that
  //     writes a BMP ends in yieldAfterDecode(), which calls requestUpdate(). The remaining early
  //     returns are all "produced nothing, retry later" and need no repaint.
  //   - cover loading still starts: ActivityManager calls loop() every tick regardless of whether
  //     an update is pending, and the gate lives in loop().
  //   - the _redBaselineAuthoritative one-shot armed by reallocSecondaryBuffer() is state, not a
  //     timer: it survives until whatever paints next, which then diffs against the controller's
  //     retained RED plane exactly as it would have here.
  firstRenderDone = true;
}

// A tap on a cover or a menu entry. Both sets of targets were published by whichever theme
// painted them, so this knows nothing about where a cover or a row physically sits -- which is
// the point, given four themes that lay the home screen out four different ways (one cover, a
// card with tiles, a carousel, and a menu that is a vertical row list in two themes, a grid of
// tiles in one and a horizontal icon strip in the carousel).
//
// Apply the same configurable selection/activation policy as other selectable touch surfaces.
// This includes the carousel's horizontal icon row as well as covers and vertical menu rows.
bool HomeActivity::handleHomeTouch() {
#if !CP_TOUCH_UI
  // The TapTargets stubs report no targets and hit-test to -1 on a board with no digitiser, so
  // none of this can run. See MappedInputManager::listTouch for why it is returned rather than
  // left to the optimiser.
  return false;
#else
  if (!mappedInput.hasTouch()) return false;
  if (!TapTargets::homeCovers().hasTargets() && !TapTargets::homeMenu().hasTargets()) return false;

  const int recentsCount = shownBooks_;
  // The menu's targets go on after its entries with the widgets that open something.
  const int menuCount = static_cast<int>(menuEntries.size()) + widgetActionCount_;

  // Covers first: in the carousel theme the side covers slide behind the centre one, and the
  // recorder orders them so the visible one wins. The two sets never overlap each other.
  // Both are bounds-checked against the CURRENT data, not the data the render saw: the targets
  // are published on the render task and recentBooks/menuEntries can change under a tap.
  int selector = -1;
  bool isBook = false;
  const auto resolve = [&](const int x, const int y) {
    const int book = TapTargets::homeCovers().hitTest(x, y);
    if (book >= 0 && book < recentsCount) {
      selector = book;
      isBook = true;
      return true;
    }
    const int menu = TapTargets::homeMenu().hitTest(x, y);
    if (menu >= 0 && menu < menuCount) {
      selector = recentsCount + menu;
      isBook = false;
      return true;
    }
    return false;
  };

  int x = 0;
  int y = 0;
  // Claimed but not acted on: moving the selection mid-contact would defeat the two-step below.
  if (mappedInput.wasScreenTouchDown(x, y) && resolve(x, y)) return true;
  if (!mappedInput.wasScreenTapped(x, y) || !resolve(x, y)) return false;

  // The whole selector is one range (covers then menu), so the comparison happens in that frame
  // and a tap on the menu correctly counts as "new" while a cover is selected, and vice versa.
  const auto tapResult = listTapActivation.applyPreference(
      selector, ListRowTap::apply(selector, recentsCount + menuCount, selectorIndex),
      SETTINGS.touchListActivation == CrossPointSettings::TOUCH_LIST_ACTIVATE_IMMEDIATELY);
  switch (tapResult) {
    case ListRowTap::Result::Rejected:
      return true;
    case ListRowTap::Result::Selected:
      requestUpdate();
      return true;
    case ListRowTap::Result::Activate:
      if (isBook) {
        onSelectBook(recentBooks[selector].path);
      } else {
        dispatchSelection(selector - recentsCount);
      }
      return true;
  }
  return true;
#endif  // CP_TOUCH_UI
}

void HomeActivity::onSelectBook(const std::string& path) {
  // replaceWithReader() arms the clean HALF the reader's first page needs over this home frame,
  // as for every list a book is opened from (ActivityManager::armBookOpenRefresh).
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectName = path;  // used to re-focus the book in the recents strip after return
  activityManager.replaceWithReader(path, std::move(hint));
}

void HomeActivity::cycleRecentBooks(const int step) {
  if (recentBooks.size() < 2) return;
  {
    // render() reads the list, and the theme's cover flags with it.
    RenderLock lock;
    if (step > 0) {
      std::rotate(recentBooks.begin(), recentBooks.begin() + 1, recentBooks.end());
    } else {
      std::rotate(recentBooks.rbegin(), recentBooks.rbegin() + 1, recentBooks.rend());
    }
    // A different cover goes to the front, so the stored copy no longer matches.
    coverRendered = false;
  }
  // The cover pass walks the list by position, and the rotation moved what it had done: it starts
  // over and skips the books that are complete. The one in hand, if any, it finds by path.
  nextRecentCoverIndex = 0;
  requestUpdate();
}

int HomeActivity::selectableCount() const {
  return shownBooks_ + static_cast<int>(menuEntries.size()) + widgetActionCount_;
}

void HomeActivity::dispatchSelection(const int menuIndex) {
  if (menuIndex < 0) return;
  const int menuCount = static_cast<int>(menuEntries.size());
  if (menuIndex < menuCount) {
    dispatchMenuAction(menuEntries[menuIndex].action);
    return;
  }
  const int widget = menuIndex - menuCount;
  if (widget >= widgetActionCount_) return;
  dispatchMenuAction(actionFor(widgetActions_[widget]));
}

void HomeActivity::dispatchMenuAction(HomeMenuAction action) {
  // Every entry replaces Home, so the buffer lent for the cover pass or kept as the carousel's frame cache
  // (keepRegionAsFrameCache) goes back now, as startActivityForResult() does, not at onExit(): the busy indicator
  // ActivityManager paints first skips while the secondary is out (showBusyIndicator()), so the carousel never showed
  // it, nor any theme during the cover pass. The return seeds the buffer from the write buffer, which holds Home's
  // frame in single-buffer mode, and arms the one-shot RED baseline, so the indicator's FAST diffs against the glass.
  restoreSecondaryBuffer(/*callerHoldsRenderLock=*/false);
  // Record where the menu entry was focused so that when the launched activity exits
  // (via returnFromChild() or an empty-stack finish()), we come back to the same row.
  // Also carries through More, whose goTo*() leaves the hint in place for what it opens.
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectIndex = selectorIndex;
  activityManager.setReturnHint(std::move(hint));

  activityManager.goToHomeMenuAction(action);
}

#if SERIAL_TEST_API
void HomeActivity::appendDebugState(std::string& out) const {
  using SerialCommandGrammar::appendField;
  appendField(out, "sel", static_cast<long>(selectorIndex));
  if (selectorIndex < 0) return;
  const int books = shownBooks_;
  const int menuIndex = selectorIndex - books;
  if (menuIndex < 0) {
    appendField(out, "label", recentBooks[selectorIndex].title);
  } else if (menuIndex < static_cast<int>(menuEntries.size())) {
    appendField(out, "label", I18N.get(menuEntries[menuIndex].label));
  } else if (menuIndex - static_cast<int>(menuEntries.size()) < widgetActionCount_) {
    // A widget that opens something: named by where it leads.
    static constexpr const char* kNames[] = {"widget:Library",      "widget:ReadingStats", "widget:Bookmarks",
                                             "widget:FileTransfer", "widget:Weather",      "widget:Recent"};
    const auto d = static_cast<size_t>(widgetActions_[menuIndex - static_cast<int>(menuEntries.size())]);
    if (d < std::size(kNames)) appendField(out, "label", kNames[d]);
  }
}
#endif  // SERIAL_TEST_API
