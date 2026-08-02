#include "HomeActivity.h"

#include <Bitmap.h>
#include <CooperativeAbort.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int CLASSIC_MIN_RECENT_TILE_HEIGHT = 280;
constexpr int LYRA_MIN_RECENT_TILE_HEIGHT = 170;
constexpr int LYRA_3_COVERS_MIN_RECENT_TILE_HEIGHT = 200;
constexpr int CLASSIC_MIN_RECENT_TO_MENU_GAP = 2;
constexpr int LYRA_MIN_RECENT_TO_MENU_GAP = 4;

struct HomeScreenLayout {
  int recentTileHeight;
  int recentToMenuGap;
  int menuHeight;
};

bool isLyraFamilyTheme() {
  const auto theme = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  return theme == CrossPointSettings::UI_THEME::LYRA || theme == CrossPointSettings::UI_THEME::LYRA_3_COVERS;
}

bool isLyraExtendedTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_3_COVERS;
}

int getMinRecentTileHeight() {
  if (isLyraExtendedTheme()) {
    return LYRA_3_COVERS_MIN_RECENT_TILE_HEIGHT;
  }
  if (isLyraFamilyTheme()) {
    return LYRA_MIN_RECENT_TILE_HEIGHT;
  }
  return CLASSIC_MIN_RECENT_TILE_HEIGHT;
}

int getMinRecentToMenuGap() {
  return isLyraFamilyTheme() ? LYRA_MIN_RECENT_TO_MENU_GAP : CLASSIC_MIN_RECENT_TO_MENU_GAP;
}

HomeScreenLayout computeHomeScreenLayout(const ThemeMetrics& metrics, int contentHeight, int menuItemCount) {
  HomeScreenLayout layout{metrics.homeCoverTileHeight, metrics.verticalSpacing, 0};

  const int menuRequiredHeight =
      menuItemCount * metrics.menuRowHeight + std::max(0, menuItemCount - 1) * metrics.menuSpacing;

  auto computeMenuHeight = [&]() {
    return contentHeight - (metrics.homeTopPadding + layout.recentTileHeight + layout.recentToMenuGap);
  };

  layout.menuHeight = computeMenuHeight();
  if (layout.menuHeight >= menuRequiredHeight) {
    return layout;
  }

  const int gapReduction =
      std::min(layout.recentToMenuGap - getMinRecentToMenuGap(), menuRequiredHeight - layout.menuHeight);
  if (gapReduction > 0) {
    layout.recentToMenuGap -= gapReduction;
    layout.menuHeight = computeMenuHeight();
  }

  if (layout.menuHeight >= menuRequiredHeight) {
    return layout;
  }

  const int tileReduction =
      std::min(layout.recentTileHeight - getMinRecentTileHeight(), menuRequiredHeight - layout.menuHeight);
  if (tileReduction > 0) {
    layout.recentTileHeight -= tileReduction;
    layout.menuHeight = computeMenuHeight();
  }

  layout.menuHeight = std::max(0, layout.menuHeight);
  return layout;
}

int getHomeCoverRenderHeight(const HomeScreenLayout& layout) {
  return isLyraExtendedTheme() ? std::max(120, layout.recentTileHeight - 58)
                               : std::max(120, layout.recentTileHeight - (isLyraFamilyTheme() ? 16 : 0));
}
}  // namespace

// Builds the menu entry list in display order. Single source of truth for both loop() (which
// dispatches Confirm based on action) and render() (which draws labels/icons).
void HomeActivity::rebuildMenuEntries() {
  menuEntries.clear();
  menuEntries.reserve(7);

  menuEntries.push_back({MenuAction::FileBrowser, StrId::STR_BROWSE_FILES, Folder});
  menuEntries.push_back({MenuAction::Recents, StrId::STR_MENU_RECENT_BOOKS, Recent});
  if (!GLOBAL_BOOKMARKS.isEmpty()) {
    menuEntries.push_back({MenuAction::GlobalBookmarks, StrId::STR_GLOBAL_BOOKMARKS, Book});
  }
  if (hasOpdsServers) {
    menuEntries.push_back({MenuAction::OpdsBrowser, StrId::STR_OPDS_BROWSER, Library});
  }
  menuEntries.push_back({MenuAction::FileTransfer, StrId::STR_FILE_TRANSFER, Transfer});
  if (SETTINGS.useWeather) {
    menuEntries.push_back({MenuAction::Weather, StrId::STR_WEATHER, Weather});
  }
  menuEntries.push_back({MenuAction::Settings, StrId::STR_SETTINGS_TITLE, Settings});
  menuEntriesDirty = false;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

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
        RecentBook updated = book;
        updated.coverBmpPath = sidecar;
        recentBooks.push_back(updated);
        continue;
      }
    }

    recentBooks.push_back(book);
  }
}

bool HomeActivity::coverAttemptsExhausted(const std::string& path) const {
  const auto it = coverTransientAttempts.find(path);
  return it != coverTransientAttempts.end() && it->second >= COVER_MAX_TRANSIENT_ATTEMPTS;
}

void HomeActivity::giveUpCover(RecentBook& book, ThumbResult res, const std::vector<ThumbSlot>& slots) {
  // Only transient failures count toward the session budget; a structural absence is already
  // permanent (generateThumbBmp wrote a sentinel) and needs no retry accounting.
  if (res == ThumbResult::TransientFail) {
    const uint8_t n = ++coverTransientAttempts[book.path];
    LOG_DBG("HOME", "Transient cover failure %u/%u for %s", n, COVER_MAX_TRANSIENT_ATTEMPTS, book.path.c_str());
  }

  // Permanent give-up = structurally absent, or transient failures past this session's budget
  // (e.g. an embedded cover the decoder rejects every time — oversize PNG, corrupt image). Mirror
  // RecentBooksActivity: write a valid placeholder BMP at each thumb slot so isCoverThumbComplete()
  // treats the book as resolved on disk and it is never re-decoded on the next boot. A still-retryable
  // transient failure records an empty cover instead, so it gets a fresh attempt next session.
  const bool permanent = (res == ThumbResult::StructurallyAbsent) || coverAttemptsExhausted(book.path);

  if (permanent) {
    bool allWritten = !slots.empty();
    for (const auto& slot : slots) {
      if (!ReaderActivity::isCoverThumbComplete(slot.path, slot.width, slot.height) &&
          !ReaderActivity::writeCoverPlaceholderBmp(slot.path, slot.width, slot.height)) {
        allWritten = false;
      }
    }
    if (allWritten) {
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

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;

  // Cold-cache cover loading runs the JPEG/PNG decoders, the sliced ZIP inflate and
  // an EPUB parse — together these can drive free heap to the edge of OOM (observed
  // Min Free ~2.8 KB) while the ~48 KB secondary framebuffer sits unused. The reader
  // releases that buffer before decoding for the same reason; do the same here for the
  // duration of loading and reallocate it once every cover is resolved (see end of this
  // function) or on exit. Release under the render lock so we never free it mid-render.
  if (!secondaryBufferReleased && renderer.hasSecondaryBuffer()) {
    RenderLock lock;
    if (renderer.releaseSecondaryBuffer()) {
      secondaryBufferReleased = true;
      // Keep X4 fast-differential refresh alive while the secondary buffer is gone:
      // the controller still holds the last home frame in RED RAM and displayBuffer()
      // re-seeds it after every refresh (syncRedRamFromFrameBuffer), so carousel/menu
      // navigation diffs against that baseline instead of downgrading to a full/half
      // waveform on every press. Precondition holds: we release right after the first
      // home render (gate requires firstRenderDone) and only issue plain BW redraws
      // until restore. Same pattern as KOReaderSyncActivity. No-op on X3.
      renderer.setSingleBufferFastDiff(true);
      LOG_DBG("HOME", "Released secondary framebuffer for cover loading (free=%lu)",
              static_cast<unsigned long>(esp_get_free_heap_size()));
    }
  }

  const auto thumbSizes = GUI.getCoverThumbSizes(coverHeight);

  // Build the placeholder slots for a book on give-up. Multi-size themes placeholder every
  // WxH thumb; the single-height path placeholders its one thumb_<h>.bmp (width = h*0.6, matching
  // the decode). Each slot's path is exactly what the loader's isCoverThumbComplete() checks.
  const auto slotsForBook = [&](const std::string& placeholder) {
    std::vector<ThumbSlot> slots;
    if (!thumbSizes.empty()) {
      slots.reserve(thumbSizes.size());
      std::transform(thumbSizes.begin(), thumbSizes.end(), std::back_inserter(slots), [&](const auto& sz) {
        return ThumbSlot{UITheme::getCoverThumbPath(placeholder, sz.first, sz.second), sz.first, sz.second};
      });
    } else {
      slots.push_back({UITheme::getCoverThumbPath(placeholder, coverHeight), coverHeight * 6 / 10, coverHeight});
    }
    return slots;
  };

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
  // burst until this budget elapses or button input arrives. Mirrors RecentBooksActivity's
  // COVER_SLICE_BUDGET_MS burst driver. A slice only writes SD files (no screen change), so
  // bursting costs nothing visually; we still yield promptly to keep input responsive.
  constexpr uint32_t COVER_SLICE_BUDGET_MS = 150;
  // Larger ZIP-inflate chunk than the per-tick 4 KB: with the ~48 KB secondary framebuffer
  // released during loading (see above) there is headroom, and 16 KB cuts the inflate
  // iteration count 4× on the multi-MB cover.img. The session reuses one malloc'd buffer of
  // this size across all continueStep() calls (realloc only if the size changes).
  constexpr size_t COVER_EXTRACT_CHUNK = 16384;

  // ── Cover extract session drain ──────────────────────────────────────────────
  // Sliced ZIP extraction of cover.img for a large embedded PNG cover. Burst-drain
  // chunks within the time budget; when done, fall through to beginPngThumbSession.
  if (extractSession) {
    const uint32_t deadline = millis() + COVER_SLICE_BUDGET_MS;
    auto status = ReaderActivity::CoverExtractSession::Status::Running;
    while (status == ReaderActivity::CoverExtractSession::Status::Running) {
      status = extractSession->continueStep(COVER_EXTRACT_CHUNK);
      // Stop the burst (but keep the session alive) when input is waiting or the budget
      // is spent — resume from where we left off on the next loadRecentCovers() call.
      if (status == ReaderActivity::CoverExtractSession::Status::Running &&
          (mappedInput.hasPendingInput() || static_cast<int32_t>(millis() - deadline) >= 0)) {
        recentsLoading = false;
        return;
      }
    }
    extractSession.reset();
    if (status == ReaderActivity::CoverExtractSession::Status::Error) {
      LOG_ERR("HOME", "Cover extract failed for book %zu", nextRecentCoverIndex);
      // Leave whatever partial file exists; beginPngThumbSession will see it missing/invalid
      // and leave a sentinel, which is the right outcome.
      pngSessionFailed = true;
    }
    // On Done: cover.img is now on disk. Fall through to the for-loop which will
    // retry ensureCoverThumb (still fails for PNG) then call beginPngThumbSession.
  }

  // ── PNG session drain ────────────────────────────────────────────────────────
  // A sliced PNG decode was started in a previous loadRecentCovers() call. Burst-drain
  // row batches within the time budget so a tall cover finishes in a few ticks instead
  // of hundreds, while still yielding promptly for button input between batches.
  if (pngSession) {
    constexpr uint32_t ROWS_PER_BATCH = 6;
    const uint32_t deadline = millis() + COVER_SLICE_BUDGET_MS;
    auto status = PngDecodeSession::Status::Running;
    while (status == PngDecodeSession::Status::Running) {
      status = pngSession->continueRows(ROWS_PER_BATCH);
      if (status == PngDecodeSession::Status::Running &&
          (mappedInput.hasPendingInput() || static_cast<int32_t>(millis() - deadline) >= 0)) {
        recentsLoading = false;  // pause between batches; resume on the next call
        return;
      }
    }
    // Session finished (done or error) — close files.
    pngSessionFiles.close();
    pngSessionFailed = (status == PngDecodeSession::Status::Error);
    pngSession.reset();
    if (pngSessionFailed) {
      // Remove the partially-written BMP and leave no sentinel — ensureCoverThumb will retry.
      // (If the cover is permanently undecodable, generateThumbBmp will write a fresh sentinel.)
      const auto& failBook = recentBooks[nextRecentCoverIndex];
      const std::string placeholder = ReaderActivity::coverThumbPlaceholder(failBook.path);
      const std::string failPath =
          thumbSizes.empty() ? UITheme::getCoverThumbPath(placeholder, coverHeight)
                             : (nextThumbSizeIndex < thumbSizes.size()
                                    ? UITheme::getCoverThumbPath(placeholder, thumbSizes[nextThumbSizeIndex].first,
                                                                 thumbSizes[nextThumbSizeIndex].second)
                                    : std::string());
      if (!failPath.empty()) Storage.remove(failPath.c_str());
      LOG_ERR("HOME", "PNG session failed for %s", failBook.path.c_str());
      // Fall through to the for-loop where the normal failure path stores empty-path and continues.
    } else {
      LOG_DBG("HOME", "PNG session complete");
      // The BMP was written — advance past this size and yield to redraw.
      nextThumbSizeIndex++;
      // Check if more sizes remain for this book.
      const auto& book = recentBooks[nextRecentCoverIndex];
      const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
      if (nextThumbSizeIndex < thumbSizes.size()) {
        yieldAfterDecode();
        return;
      }
      // All sizes done — store placeholder, advance book, yield.
      if (book.coverBmpPath != placeholder) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
        recentBooks[nextRecentCoverIndex].coverBmpPath = placeholder;
      }
      nextRecentCoverIndex++;
      nextThumbSizeIndex = 0;
      yieldAfterDecode();
      return;
    }
  }
  // ─────────────────────────────────────────────────────────────────────────────

  for (; nextRecentCoverIndex < recentBooks.size(); nextRecentCoverIndex++, nextThumbSizeIndex = 0) {
    RecentBook& book = recentBooks[nextRecentCoverIndex];
    if (!Storage.exists(book.path.c_str())) continue;

    // The grid thumbnail is source-agnostic: ReaderActivity::ensureCoverThumb() produces an
    // identical "<bookCacheDir>/thumb_..." BMP whether the cover comes from a sidecar image
    // (preferred source) or the embedded cover, and we always store the canonical placeholder.
    const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);

    if (!thumbSizes.empty()) {
      // Multi-size path (e.g. LyraCarousel needs center + side BMPs).
      // Process one missing size per loop() call so input stays responsive between decodes.
      bool allValid = true;
      for (size_t i = nextThumbSizeIndex; i < thumbSizes.size(); i++) {
        const auto& sz = thumbSizes[i];
        const std::string path = UITheme::getCoverThumbPath(placeholder, sz.first, sz.second);
        // Require a complete BMP (all pixel rows present) at no more than the slot size — not just
        // size>0: a thumbnail truncated by an interrupted write passes size>0 but fails to draw, and
        // an oversized thumb from an older build's crop mode would be rescaled at draw time
        // (aliasing the dither into a grid); neither would ever be regenerated.
        const bool validThumb = ReaderActivity::isCoverThumbComplete(path, sz.first, sz.second);
        LOG_DBG("HOME", "Cover check [%dx%d] path=%s valid=%d", sz.first, sz.second, path.c_str(), validThumb ? 1 : 0);

        if (!validThumb) {
          // Button input has priority: never start a fresh decode while a press is
          // queued. Yield with this size still pending so the next pass retries it.
          if (mappedInput.hasPendingInput()) {
            nextThumbSizeIndex = i;
            recentsLoading = false;
            return;
          }

          // This book already burned its transient-failure budget this session — stop retrying it
          // (a reboot resets the counter and tries again) so it can't starve the others.
          if (coverAttemptsExhausted(book.path)) {
            LOG_DBG("HOME", "Cover attempts exhausted for %s this session — writing placeholder", book.path.c_str());
            // Already counted to the budget; don't re-count. coverAttemptsExhausted() makes this
            // a permanent give-up inside giveUpCover(), so a durable placeholder BMP is written.
            giveUpCover(book, ThumbResult::StructurallyAbsent, slotsForBook(placeholder));
            allValid = false;
            break;
          }

          // Cover decode needs ~42 KB contiguous heap — free the frame cache first.
          invalidateFrameCacheSafely();

          // Try synchronous decode first (handles JPEG and cached covers).
          CooperativeAbort::clearAborted();
          const ThumbResult res = ReaderActivity::ensureCoverThumb(book.path, sz.first, sz.second);
          LOG_DBG("HOME", "ensureCoverThumb(%dx%d) for %s: %s", sz.first, sz.second, book.path.c_str(),
                  res == ThumbResult::Ok                   ? "ok"
                  : res == ThumbResult::StructurallyAbsent ? "absent"
                                                           : "transient");
          if (res != ThumbResult::Ok) {
            // A decode that genuinely bailed mid-loop for pending input is not a real
            // failure: retry the same size later instead of recording an empty cover.
            // consumeAborted() is true only when a decode row loop broke for input, so a
            // plain failure (oversize image, missing cover) falls through to the fallbacks.
            if (CooperativeAbort::consumeAborted()) {
              LOG_DBG("HOME", "Cover decode for %s yielded to input — will retry size %dx%d", book.path.c_str(),
                      sz.first, sz.second);
              nextThumbSizeIndex = i;
              recentsLoading = false;
              return;
            }
            // Structural absence is permanent (generateThumbBmp already wrote a sentinel): don't
            // waste EPUB opens trying to start a PNG/extract session that can't succeed. Only a
            // transient failure walks the session ladder.
            if (res == ThumbResult::TransientFail && !pngSessionFailed) {
              // Try sliced PNG decode (succeeds when cover.img is already cached).
              pngSession = ReaderActivity::beginPngThumbSession(book.path, sz.first, sz.second, pngSessionFiles);
              if (!pngSession) {
                // cover.img not yet cached — try sliced ZIP extraction first.
                extractSession = ReaderActivity::beginCoverExtractSession(book.path);
                if (extractSession) {
                  LOG_DBG("HOME", "Started cover extract session for %s (%zu bytes)", book.path.c_str(),
                          extractSession->totalBytes());
                  nextThumbSizeIndex = i;
                  recentsLoading = false;
                  return;
                }
              }
            }
            pngSessionFailed = false;  // consumed
            if (pngSession) {
              LOG_DBG("HOME", "Started PNG session for %s [%dx%d] (%u rows)", book.path.c_str(), sz.first, sz.second,
                      pngSession->totalRows());
              nextThumbSizeIndex = i;
              recentsLoading = false;
              return;
            }
            // No session could be started — give up on this book for now (counts the transient;
            // writes a durable placeholder once the budget is spent).
            giveUpCover(book, res, slotsForBook(placeholder));
            allValid = false;
            break;
          }
          // One decode done — yield back to loop() so input can be serviced.
          // nextThumbSizeIndex advances past this size; next call continues from i+1.
          nextThumbSizeIndex = i + 1;
          if (nextThumbSizeIndex < thumbSizes.size()) {
            // More sizes remain for this book — stay on current book next call.
            yieldAfterDecode();
            return;
          }
          // All sizes for this book now complete — fall through to store placeholder.
          break;
        }
      }

      LOG_DBG("HOME", "loadRecentCovers[%zu]: coverBmpPath=%s placeholder=%s", nextRecentCoverIndex,
              book.coverBmpPath.c_str(), placeholder.c_str());
      if (!allValid) continue;  // gave up on this book — giveUpCover() already stored empty/placeholder
      // All sizes valid or just completed — store canonical placeholder and yield if a decode happened.
      if (book.coverBmpPath != placeholder) {
        LOG_DBG("HOME", "Self-heal/store: coverBmpPath -> placeholder=%s", placeholder.c_str());
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
        book.coverBmpPath = placeholder;
      }
      // nextThumbSizeIndex > 0 means at least one decode happened this call — yield.
      if (nextThumbSizeIndex > 0) {
        // Advance past this book before yielding so the next loop() call starts on book N+1.
        nextRecentCoverIndex++;
        nextThumbSizeIndex = 0;
        yieldAfterDecode();
        return;
      }
      // All sizes were already cached — no decode work done, continue to next book immediately.
    } else {
      // Single-height path (non-carousel themes). Expected size mirrors ensureCoverThumb(height):
      // width = height * 0.6.
      const std::string path = UITheme::getCoverThumbPath(placeholder, coverHeight);
      // Require a complete BMP (all pixel rows present) at no more than the slot size — see the
      // multi-size path above for why truncated AND oversized thumbs must both be regenerated.
      const bool validThumb = ReaderActivity::isCoverThumbComplete(path, coverHeight * 6 / 10, coverHeight);
      LOG_DBG("HOME", "Cover check [h=%d] path=%s valid=%d", coverHeight, path.c_str(), validThumb ? 1 : 0);

      LOG_DBG("HOME", "loadRecentCovers[%zu]: coverBmpPath=%s placeholder=%s anyMissing=%d", nextRecentCoverIndex,
              book.coverBmpPath.c_str(), placeholder.c_str(), validThumb ? 0 : 1);

      if (!validThumb) {
        // Button input has priority: don't start a decode while a press is queued.
        // Yield without advancing so this book's cover is retried on a later pass.
        if (mappedInput.hasPendingInput()) {
          recentsLoading = false;
          return;
        }

        // This book already burned its transient-failure budget this session — stop retrying it
        // (a reboot resets the counter) and advance.
        if (coverAttemptsExhausted(book.path)) {
          LOG_DBG("HOME", "Cover attempts exhausted for %s this session — writing placeholder", book.path.c_str());
          // Already counted to the budget; don't re-count. coverAttemptsExhausted() makes this a
          // permanent give-up inside giveUpCover(), so a durable placeholder BMP is written.
          giveUpCover(book, ThumbResult::StructurallyAbsent, slotsForBook(placeholder));
          nextRecentCoverIndex++;
          yieldAfterDecode();
          return;
        }

        // Cover decode needs ~42 KB contiguous heap — free the frame cache first.
        invalidateFrameCacheSafely();
        CooperativeAbort::clearAborted();
        const ThumbResult res = ReaderActivity::ensureCoverThumb(book.path, coverHeight);
        LOG_DBG("HOME", "ensureCoverThumb(h=%d) for %s: %s", coverHeight, book.path.c_str(),
                res == ThumbResult::Ok                   ? "ok"
                : res == ThumbResult::StructurallyAbsent ? "absent"
                                                         : "transient");
        // A decode that genuinely bailed mid-loop for pending input is not a real failure
        // — retry this book later rather than recording an empty cover path.
        if (res != ThumbResult::Ok && CooperativeAbort::consumeAborted()) {
          LOG_DBG("HOME", "Cover decode for %s yielded to input — will retry", book.path.c_str());
          recentsLoading = false;
          return;
        }
        if (res == ThumbResult::Ok) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
          book.coverBmpPath = placeholder;
          LOG_DBG("HOME", "After generate: stored coverBmpPath=%s", placeholder.c_str());
          yieldAfterDecode();
          return;
        }

        // Transient failure — walk the same session ladder as the multi-size path (skip it for a
        // structural absence: nothing to extract, and generateThumbBmp already wrote a sentinel).
        // (1) Sliced PNG decode: succeeds when cover.img is already cached, and unlike the
        //     synchronous decoder it has no ~960k-px area cap — it decodes large covers (e.g.
        //     1200x1848) across ticks. This is why the synchronous ensureCoverThumb "too large"
        //     rejection is only transient: the sliced path below can still produce the thumb.
        // (2) Sliced ZIP extract: when cover.img isn't cached yet, extract it across ticks; the
        //     drain at the top of loadRecentCovers re-runs this ladder once it lands.
        if (res == ThumbResult::TransientFail) {
          if (!pngSessionFailed) {
            pngSession = ReaderActivity::beginPngThumbSession(book.path, coverHeight, pngSessionFiles);
            if (pngSession) {
              LOG_DBG("HOME", "Started PNG session for %s (single-height, %u rows)", book.path.c_str(),
                      pngSession->totalRows());
              recentsLoading = false;
              return;
            }
            extractSession = ReaderActivity::beginCoverExtractSession(book.path);
            if (extractSession) {
              LOG_DBG("HOME", "Started cover extract session for %s (single-height)", book.path.c_str());
              recentsLoading = false;
              return;
            }
          }
          pngSessionFailed = false;  // consumed
        }
        // No session could be started — give up on this book for now (counts the transient;
        // writes a durable placeholder once the budget is spent).
        giveUpCover(book, res, slotsForBook(placeholder));
        LOG_DBG("HOME", "No cover for %s; advancing", book.path.c_str());
        nextRecentCoverIndex++;
        yieldAfterDecode();
        return;
      }

      // Already present — make sure the stored path is the canonical placeholder so a stale
      // "[HEIGHT].bmp" / raw-sidecar entry self-heals to the unified naming without a re-decode.
      if (book.coverBmpPath != placeholder) {
        LOG_DBG("HOME", "Self-heal: coverBmpPath=%s -> placeholder=%s", book.coverBmpPath.c_str(), placeholder.c_str());
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
        book.coverBmpPath = placeholder;
      }
    }
  }

  recentsLoaded = true;
  recentsLoading = false;
  restoreSecondaryBuffer();
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
  if (!secondaryBufferReleased) return;
  const auto doRestore = [this]() {
    if (renderer.reallocSecondaryBuffer()) {
      secondaryBufferReleased = false;
      // Two-buffer differential is available again — turn off the single-buffer
      // RED-RAM-baseline mode so normal fast refresh resumes against the secondary.
      renderer.setSingleBufferFastDiff(false);
      // Do NOT syncRedRamFromFrameBuffer() here: reallocSecondaryBuffer() fills the new secondary
      // with WHITE, and syncRedRamFromFrameBuffer() copies that white buffer into RED RAM —
      // overwriting the correct baseline. RED already holds the home frame (synced before the
      // release; the controller retains it through release/realloc, which don't touch RED). The
      // white reseed made the next FAST refresh (e.g. Home->Settings) diff against white, ghosting
      // the home screen through. Leave RED intact.
      LOG_DBG("HOME", "Restored secondary framebuffer after cover loading (free=%lu)",
              static_cast<unsigned long>(esp_get_free_heap_size()));
    }
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

  // A finished-book "sync to KOReader, then search OPDS for this author" request that just
  // rebooted (the sync reboots to reclaim WiFi-session heap fragmentation, see
  // KOReaderSyncActivity::onExit()) lands here first — there's no reader to hand it to the way
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

  hasOpdsServers = OPDS_STORE.hasServers();

  selectorIndex = 0;
  recentsLoading = false;
  recentsLoaded = false;
  firstRenderDone = false;
  nextRecentCoverIndex = 0;
  nextThumbSizeIndex = 0;
  extractSession.reset();
  pngSession.reset();
  pngSessionFiles.close();
  pngSessionFailed = false;
  coverTransientAttempts.clear();
  coverRendered = false;
  secondaryBufferReleased = false;
  freeCoverBuffer();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  if (recentBooks.empty()) {
    recentsLoaded = true;
  }

  // Apply focus: book path takes priority, else combined selector index (covers
  // "return to the menu entry I was on").
  bool focused = false;
  if (!focusBookPath.empty()) {
    for (size_t i = 0; i < recentBooks.size(); ++i) {
      if (recentBooks[i].path == focusBookPath) {
        selectorIndex = static_cast<int>(i);
        focused = true;
        break;
      }
    }
    focusBookPath.clear();
  }
  if (!focused && focusSelectorIndex >= 0) {
    rebuildMenuEntries();  // need menu count to clamp; rebuild is idempotent
    const int combinedSize = static_cast<int>(recentBooks.size() + menuEntries.size());
    if (combinedSize > 0) {
      selectorIndex = std::min(focusSelectorIndex, combinedSize - 1);
    }
  }
  focusSelectorIndex = -1;

  // Trigger first update
  menuEntriesDirty = true;
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  freeCoverBuffer();
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

void HomeActivity::loop() {
  if (menuEntriesDirty) {
    rebuildMenuEntries();
  }

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
      loadRecentCovers(UITheme::getInstance().getMetrics().homeCoverHeight);
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
    const bool rowToggle = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Down);

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + 1) % menuItemCount;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
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
    const int totalItems = static_cast<int>(recentBooks.size() + menuEntries.size());

    if (firstRenderDone && !recentsLoaded && !recentsLoading && !inputWaiting) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect contentRect = UITheme::getContentRect(renderer, true, false);
      const HomeScreenLayout layout =
          computeHomeScreenLayout(metrics, contentRect.height, static_cast<int>(menuEntries.size()));
      loadRecentCovers(getHomeCoverRenderHeight(layout));
      return;
    }

    buttonNavigator.onNext([this, totalItems] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, totalItems] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int recentsCount = static_cast<int>(recentBooks.size());
    if (selectorIndex < recentsCount) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIdx = selectorIndex - recentsCount;
      if (menuIdx < static_cast<int>(menuEntries.size())) {
        dispatchMenuAction(menuEntries[menuIdx].action);
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  if (menuEntriesDirty) {
    rebuildMenuEntries();
  }

  const int menuCount = static_cast<int>(menuEntries.size());
  const bool isCarousel = (GUI.getHomeNavigation() == HomeNavigation::Carousel);

  // Fast path: theme owns its own pre-rendered frame cache
  if (isCarousel) {
    const auto carouselLabels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    const bool handled = GUI.tryFastHomeRender(
        renderer, recentBooks, selectorIndex, menuCount,
        [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
        [this](int index) { return menuEntries[index].icon; }, carouselLabels.btn1, carouselLabels.btn2,
        carouselLabels.btn3, carouselLabels.btn4);
    if (handled) {
      if (!firstRenderDone) {
        firstRenderDone = true;
        requestUpdate();
      }
      return;
    }
    // Fast path failed (e.g. frame cache malloc failed). Force cover re-render
    // so the fallback drawRecentBookCover below doesn't skip based on stale state.
    coverRendered = false;
    coverBufferStored = false;
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.homeTopPadding}, nullptr);

  const int totalItems = static_cast<int>(recentBooks.size() + menuEntries.size());
  if (selectorIndex >= totalItems) {
    selectorIndex = std::max(0, totalItems - 1);
  }

  const HomeScreenLayout layout = computeHomeScreenLayout(metrics, contentRect.height, menuCount);

  coverRectX = contentRect.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = contentRect.width;
  coverRectH = layout.recentTileHeight;

  GUI.drawRecentBookCover(renderer,
                          Rect{contentRect.x, metrics.homeTopPadding, contentRect.width, layout.recentTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  GUI.drawButtonMenu(
      renderer,
      Rect{contentRect.x, metrics.homeTopPadding + layout.recentTileHeight + layout.recentToMenuGap, contentRect.width,
           layout.menuHeight},
      menuCount, selectorIndex - static_cast<int>(recentBooks.size()),
      [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
      [this](int index) { return menuEntries[index].icon; });

  const auto labels = isCarousel ? mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                                 : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  }
}

void HomeActivity::onSelectBook(const std::string& path) {
  // Arm a clean HALF baseline for the reader's first page. It diffs against whatever RED RAM holds
  // — this home frame — and a FAST diff of a dramatic home->page change leaves the home screen
  // ghosting through. The reader's onEnter expects the launching activity to arm this (FileBrowser
  // does via enforceExitFullRefresh; Home didn't, so books opened from Home ghosted on entry).
  renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectName = path;  // used to re-focus the book in the recents strip after return
  activityManager.replaceWithReader(path, std::move(hint));
}

void HomeActivity::dispatchMenuAction(MenuAction action) {
  // Record where the menu entry was focused so that when the launched activity exits
  // (via returnFromChild() or an empty-stack finish()), we come back to the same row.
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectIndex = selectorIndex;
  activityManager.setReturnHint(std::move(hint));

  switch (action) {
    case MenuAction::FileBrowser:
      activityManager.goToFileBrowser();
      break;
    case MenuAction::Recents:
      activityManager.goToRecentBooks();
      break;
    case MenuAction::GlobalBookmarks:
      activityManager.goToGlobalBookmarks();
      break;
    case MenuAction::OpdsBrowser:
      activityManager.goToBrowser();
      break;
    case MenuAction::FileTransfer:
      activityManager.goToFileTransfer();
      break;
    case MenuAction::Weather:
      activityManager.goToWeather();
      break;
    case MenuAction::Settings:
      activityManager.goToSettings();
      break;
    default:
      LOG_ERR("HOME", "Unexpected menu action: %d", static_cast<int>(action));
      break;
  }
}
