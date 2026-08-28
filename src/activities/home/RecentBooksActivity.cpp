#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <string>

#include "../ActivityManager.h"
#include "../util/ConfirmationActivity.h"
#include "BookInfoActivity.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"
#include "components/CoverGridLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
std::string gridThumbPath(const std::string& coverBmpPath, int tw, int th) {
  return UITheme::getCoverThumbPath(coverBmpPath, tw, th);
}

// Where the grid lives on screen: the content rect and first-row offset from the active theme,
// plus the cell geometry CoverGridLayout derives from that space.
struct GridLayout {
  Rect content{};     // rect the whole screen body lives in
  int contentTop;     // y of the first row (below the header)
  int contentHeight;  // usable height from contentTop down
  CoverGridLayout::Layout cells;
};

GridLayout computeGridLayout(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  GridLayout l{};
  l.content = UITheme::getContentRect(renderer, true, true);
  l.contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  l.contentHeight = l.content.height - l.contentTop - metrics.verticalSpacing;

  // The gesture-hint line sits at the bottom of the content area on X4; keep the last row's
  // labels clear of it (the scroll arrows share that strip).
  l.cells = CoverGridLayout::compute({.contentWidth = l.content.width,
                                      .contentHeight = l.contentHeight,
                                      .bottomReserve = gpio.deviceIsX3() ? 12 : 24,
                                      .maxCellHeight = RecentBooksActivity::GRID_MAX_CELL_HEIGHT});
  return l;
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks = RECENT_BOOKS.getBooks();
  // Cache each book's reading progress once so the grid badge avoids an SD read
  // per cell per repaint. Progress can only change by opening a book, which exits
  // this activity, so the cache stays valid for the activity's lifetime.
  bookProgress.assign(recentBooks.size(), -1);
  for (size_t i = 0; i < recentBooks.size(); i++) {
    bookProgress[i] = static_cast<int8_t>(UITheme::getBookProgressPercent(recentBooks[i]));
  }
}

bool RecentBooksActivity::loadNextCover() {
  // Fixed thumbnail dimensions shared with FinishedBookActivity.
  // The cell renderer scales the BMP down to the runtime cell width for display.
  const int tw = GRID_THUMB_WIDTH;
  const int th = GRID_THUMB_HEIGHT;

  // ── Cover extract session tick ───────────────────────────────────────────────
  if (extractSession) {
    const auto status = extractSession->continueStep(4096);
    if (status == ReaderActivity::CoverExtractSession::Status::Running) return false;
    extractSession.reset();
    if (status == ReaderActivity::CoverExtractSession::Status::Error) {
      LOG_ERR("RBA", "Cover extract failed for book %zu", nextCoverIndex);
      pngSessionFailed = true;
    }
    // On Done: fall through to PNG session setup on next call via the for-loop.
    return false;
  }

  // ── PNG session tick ────────────────────────────────────────────────────────
  if (pngSession) {
    constexpr uint32_t ROWS_PER_TICK = 6;
    const auto status = pngSession->continueRows(ROWS_PER_TICK);
    // Progress evidence (throttled): proves the sliced decode is advancing rather than stalled.
    const uint32_t now = millis();
    if (now - lastCoverProgressLogMs_ >= 1000) {
      lastCoverProgressLogMs_ = now;
      LOG_DBG("RBA", "PNG cover decode: %u/%u rows for %s", pngSession->rowsDone(), pngSession->totalRows(),
              recentBooks[nextCoverIndex].path.c_str());
    }
    if (status == PngDecodeSession::Status::Running) {
      return false;  // not done yet — render() will requestUpdate() and call us again
    }
    pngSessionFiles.close();
    // Local, not the member: this branch resolves the book itself and advances the index, so a
    // failure here must not leak into the NEXT book's first attempt (as wasPostFailure it would
    // suppress that book's session ladder and record it as coverless for the whole session).
    const bool decodeFailed = (status == PngDecodeSession::Status::Error);
    pngSessionFailed = false;
    pngSession.reset();

    RecentBook& book = recentBooks[nextCoverIndex];
    const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
    if (!decodeFailed) {
      LOG_DBG("RBA", "PNG session complete for %s", book.path.c_str());
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
      book.coverBmpPath = placeholder;
    } else {
      // Remove the partial BMP; the normal failure path below will store empty.
      const std::string thumbPath = gridThumbPath(placeholder, tw, th);
      Storage.remove(thumbPath.c_str());
      LOG_ERR("RBA", "PNG session failed for %s", book.path.c_str());
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
      book.coverBmpPath = "";
    }
    nextCoverIndex++;
    return false;  // advance to next book on next render tick
  }
  // ───────────────────────────────────────────────────────────────────────────

  for (; nextCoverIndex < recentBooks.size(); nextCoverIndex++) {
    RecentBook& book = recentBooks[nextCoverIndex];
    if (!Storage.exists(book.path.c_str())) continue;

    // The grid thumbnail is source-agnostic: ensureCoverThumb() produces an identical
    // "<bookCacheDir>/thumb_<W>x<H>.bmp" whether the cover comes from a sidecar image
    // (preferred source) or the embedded cover, and we always store the canonical placeholder.
    const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
    const std::string thumbPath = gridThumbPath(placeholder, tw, th);

    // Require a COMPLETE BMP (all pixel rows present) at no more than the slot size, not just
    // size>0: a thumbnail truncated by an interrupted write passes size>0 but fails to draw
    // partway, and an oversized thumb from an older build's crop mode gets rescaled at draw time
    // (aliasing the dither into a grid) — regenerate both. A no-cover book's placeholder BMP
    // (written below) is also a complete exact-size BMP, so it passes here and is treated as a
    // resolved cover — no re-opening the EPUB three times per scan to rediscover it has no cover.
    const bool valid = ReaderActivity::isCoverThumbComplete(thumbPath, tw, th);
    if (!valid) {
      const ThumbResult res = ReaderActivity::ensureCoverThumb(book.path, tw, th);
      const bool ok = (res == ThumbResult::Ok);
      const bool wasPostFailure = pngSessionFailed;
      pngSessionFailed = false;  // consumed
      // Only a TRANSIENT failure may walk the session ladder. A structural absence is already
      // permanent — generateThumbBmp() wrote the 0-byte sentinel, and re-extracting the same ZIP
      // entry yields the same undecodable bytes. Treating it like a transient failure livelocked
      // this scan: the sentinel keeps isCoverThumbComplete() false, so the book never resolved,
      // nextCoverIndex never advanced, and the cover was re-inflated from the ZIP every ~1.3 s
      // forever (observed on an EPUB whose "cover.png" is really an AVIF). Mirrors HomeActivity.
      if (res == ThumbResult::TransientFail && !wasPostFailure) {
        pngSession = ReaderActivity::beginPngThumbSession(book.path, tw, th, pngSessionFiles);
        if (pngSession) {
          LOG_DBG("RBA", "Started PNG session for %s (%u rows)", book.path.c_str(), pngSession->totalRows());
          return false;
        }
        extractSession = ReaderActivity::beginCoverExtractSession(book.path);
        if (extractSession) {
          LOG_DBG("RBA", "Started cover extract session for %s (%zu bytes)", book.path.c_str(),
                  extractSession->totalBytes());
          return false;
        }
      }

      // Permanent give-up: structurally absent, or no decode/extract session could be started for
      // a transient failure. But NOT if a sidecar image exists: that is a real cover source that
      // simply failed to convert this pass (e.g. tight heap) and should be retried, not permanently
      // placeholdered. Otherwise write a valid placeholder BMP so future scans treat the book as
      // resolved and stop re-opening the EPUB. (A transient post-failure retry — wasPostFailure —
      // is skipped here too, so the next pass gets a clean attempt.)
      const bool giveUpPermanently = (res == ThumbResult::StructurallyAbsent) || (!ok && !wasPostFailure);
      if (giveUpPermanently && ReaderActivity::sidecarCoverPath(book.path).empty() &&
          ReaderActivity::writeCoverPlaceholderBmp(thumbPath)) {
        LOG_DBG("RBA", "No extractable cover for %s — wrote placeholder", book.path.c_str());
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
        book.coverBmpPath = placeholder;
        nextCoverIndex++;
        return false;
      }
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, ok ? placeholder : "");
      book.coverBmpPath = ok ? placeholder : "";
      nextCoverIndex++;
      return false;
    }

    // Already present — make sure the stored path is the canonical placeholder so a stale
    // "[HEIGHT].bmp" / raw-sidecar entry self-heals to the unified naming without a re-decode.
    if (book.coverBmpPath != placeholder) {
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
      book.coverBmpPath = placeholder;
    }
  }

  return true;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  loadRecentBooks();

  selectorIndex = 0;
  if (initialFocusIndex >= 0 && initialFocusIndex < static_cast<int>(recentBooks.size())) {
    selectorIndex = initialFocusIndex;
  }
  initialFocusIndex = -1;

  coversLoaded = false;
  coversLoading = false;
  firstRenderDone = false;
  nextCoverIndex = 0;
  prevSelectorIndex = -1;
  fullRedrawNeeded = true;
  openingBook = false;
  extractSession.reset();
  pngSession.reset();
  pngSessionFiles.close();
  pngSessionFailed = false;

  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  bookProgress.clear();
}

void RecentBooksActivity::switchViewMode(bool grid) {
  APP_STATE.recentBooksGridView = grid;
  APP_STATE.saveToFile();
  coversLoaded = false;
  coversLoading = false;
  firstRenderDone = false;
  nextCoverIndex = 0;
  prevSelectorIndex = -1;
  extractSession.reset();
  pngSession.reset();
  pngSessionFiles.close();
  pngSessionFailed = false;
  fullRedrawNeeded = true;
  requestUpdate(true);
}

void RecentBooksActivity::removeSelectedBook() {
  if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  const std::string bookPath = recentBooks[selectorIndex].path;
  const std::string bookTitle = recentBooks[selectorIndex].title;
  auto handler = [this, bookPath](const ActivityResult& res) {
    if (!res.isCancelled) {
      LOG_DBG("RBA", "Removing from recent books: %s", bookPath.c_str());
      RECENT_BOOKS.removeBook(bookPath);
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
        selectorIndex = static_cast<int>(recentBooks.size()) - 1;
      }
      prevSelectorIndex = -1;
      fullRedrawNeeded = true;
      requestUpdate(true);
    }
  };
  std::string heading = tr(STR_REMOVE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, bookTitle), handler);
}

void RecentBooksActivity::showSelectedBookInfo() {
  if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  const std::string& path = recentBooks[selectorIndex].path;
  if (FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path)) {
    startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path),
                           [this](const ActivityResult&) { requestUpdate(); });
  }
}

void RecentBooksActivity::loop() {
  const bool gridView = APP_STATE.recentBooksGridView;
  const int listSize = static_cast<int>(recentBooks.size());

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    // Confirm short/long: open book (long = KOReader sync for EPUBs)
    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
      const bool longPress = (ev.type == ButtonEventManager::PressType::Long) && KOREADER_STORE.hasCredentials();
      const std::string& selectedPath = recentBooks[selectorIndex].path;
      const bool isEpubBook = FsHelpers::hasEpubExtension(selectedPath);
      LOG_DBG("RBA", "Selected recent book: %s (sync=%d epub=%d)", selectedPath.c_str(), longPress ? 1 : 0,
              isEpubBook ? 1 : 0);
      if (longPress && isEpubBook) {
        auto& sync = APP_STATE.koReaderSyncSession;
        sync.autoPullEpubPath = selectedPath;
        sync.postAction = KOReaderSyncPostAction::Reader;
        APP_STATE.saveToFile();
      }
      openingBook = true;
      ReturnHint hint;
      hint.target = ReturnTo::RecentBooks;
      hint.selectIndex = selectorIndex;
      activityManager.replaceWithReader(recentBooks[selectorIndex].path, std::move(hint));
      return;
    }

    // Back short: go home
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      onGoHome();
      return;
    }

    // Up short: navigate (row up in grid, previous in list)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Up) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        if (gridView) {
          selectorIndex = std::max(0, selectorIndex - gridColumns());
        } else {
          selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
        }
        requestUpdate();
      }
      continue;
    }

    // Down short: navigate (row down in grid, next in list)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Down) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        if (gridView) {
          selectorIndex = std::min(listSize - 1, selectorIndex + gridColumns());
        } else {
          selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
        }
        requestUpdate();
      }
      continue;
    }

    // Up long: toggle between list and grid view
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Up) &&
        ev.type == ButtonEventManager::PressType::Long) {
      switchViewMode(!gridView);
      return;
    }

    // Left short: column left in grid, previous in list
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
        requestUpdate();
      }
      continue;
    }

    // Right short: column right in grid, next in list
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
        requestUpdate();
      }
      continue;
    }

    // Left long: remove selected book (both views)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left) &&
        ev.type == ButtonEventManager::PressType::Long) {
      removeSelectedBook();
      return;
    }

    // Right long: show book info (both views)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) &&
        ev.type == ButtonEventManager::PressType::Long) {
      showSelectedBookInfo();
      return;
    }
  }
}

void RecentBooksActivity::render(RenderLock&& lock) {
  // Confirm has committed to opening a book; the reader is taking over the
  // screen. Skip any grid repaint so the selection highlight can't visibly
  // jump to a stale buffer position during the transition.
  if (openingBook) return;

  if (!APP_STATE.recentBooksGridView) {
    renderListView(std::move(lock));
    return;
  }

  // After the first paint, generate covers in time-bounded bursts. A cover slice (a PNG-decode
  // row batch or a ZIP-extract chunk) only writes SD files — it does NOT change the screen — so we
  // must NOT repaint per slice. Doing so turned every 6-row slice into a full ~2 s e-ink refresh,
  // so a 1848-row cover needed 300+ refreshes (~10 min, and brutal on the panel). Instead drive
  // slices here until a cover actually finishes (repaint once to show it), or until input is
  // pending / a small time budget elapses (return WITHOUT repainting and resume on the next tick).
  if (firstRenderDone && !coversLoaded && !coversLoading) {
    // The cursor moved while a cover is still decoding: repaint the selection NOW (cheap two-cell
    // partial) and resume decoding next tick. Otherwise the budget loop below would swallow the
    // move and the highlight would only jump once the cover finished (seen as a frozen cursor).
    if (prevSelectorIndex != selectorIndex) {
      requestUpdate();
      renderGridView(std::move(lock));  // partial path: fullRedrawNeeded stays false
      return;
    }

    constexpr uint32_t COVER_SLICE_BUDGET_MS = 150;
    coversLoading = true;
    const size_t startIdx = nextCoverIndex;
    const uint32_t deadline = millis() + COVER_SLICE_BUDGET_MS;
    bool coverFinished = false;
    while (true) {
      if (loadNextCover()) {  // all covers resolved
        coversLoaded = true;
        coverFinished = true;
        break;
      }
      if (nextCoverIndex != startIdx) {  // a book's cover just completed → worth showing now
        coverFinished = true;
        break;
      }
      if (mappedInput.hasPendingInput() || static_cast<int32_t>(millis() - deadline) >= 0) break;
    }
    coversLoading = false;

    if (!coverFinished) {
      requestUpdate();  // still slicing one cover — come back and continue, no repaint
      return;
    }
    if (!coversLoaded) requestUpdate();  // more covers remain — continue after this repaint
    fullRedrawNeeded = true;
  }

  renderGridView(std::move(lock));
  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();  // kick off cover generation now that the grid is on screen
  }
}

void RecentBooksActivity::renderListView(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight},
        static_cast<int>(recentBooks.size()), selectorIndex, [this](int index) { return recentBooks[index].title; },
        [this](int index) {
          const auto& book = recentBooks[index];
          if (!book.author.empty() && !book.series.empty()) return book.author + "\n" + book.series;
          if (!book.series.empty()) return book.series;
          return book.author;
        },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

  if (!gpio.deviceIsX3()) {
    const int hintY = contentRect.y + contentRect.height - metrics.verticalSpacing - 14;
    const std::string hint = std::string(tr(STR_DIR_UP)) + "+L: " + tr(STR_VIEW_GRID) + "/" + tr(STR_VIEW_LIST) +
                             "   " + tr(STR_DIR_LEFT) + "+L: " + tr(STR_REMOVE) + "   " + tr(STR_DIR_RIGHT) +
                             "+L: " + tr(STR_INFO);
    renderer.drawText(SMALL_FONT_ID, contentRect.x + metrics.contentSidePadding, hintY, hint.c_str());
  }

  const bool hasBooks = !recentBooks.empty();
  const auto hints =
      mappedInput.mapHints(tr(STR_HOME), hasBooks ? tr(STR_OPEN) : "", "", "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}

void RecentBooksActivity::renderGridCell(int index, bool selected, int cellX, int cellY, int tw, int th, int labelW) {
  const auto& book = recentBooks[index];
  const int labelY = cellY + th + 3;
  const int cellFillHeight = th + CoverGridLayout::kLabelHeight + 3;

  if (selected) {
    renderer.fillRect(cellX, cellY, tw, cellFillHeight);
    renderer.drawRect(cellX, cellY, tw, th, false);
  } else {
    // Clear to white before redrawing (needed when deselecting)
    renderer.fillRect(cellX, cellY, tw, cellFillHeight, false);
    renderer.drawRect(cellX, cellY, tw, th);
  }

  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = gridThumbPath(book.coverBmpPath, GRID_THUMB_WIDTH, GRID_THUMB_HEIGHT);
    FsFile file;
    bool thumbDrawn = false;
    if (Storage.openFileForRead("RBA", thumbPath, file)) {
      Bitmap bmp(file);
      // Skip a truncated thumbnail (interrupted write): its header parses but readNextRow() fails
      // partway, leaving a half-drawn cover and a GFX error. The validity check above regenerates it.
      if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete()) {
        const int imgW = bmp.getWidth();
        const int imgH = bmp.getHeight();
        const int innerW = tw - 2;
        const int innerH = th - 2;
        if (imgW > 0 && imgH > 0) {
          // Mirror drawBitmap1Bit scale = min(maxW/imgW, maxH/imgH) to get rendered size,
          // then center the image within the frame.
          const float scaleX = static_cast<float>(innerW) / imgW;
          const float scaleY = static_cast<float>(innerH) / imgH;
          // Cap at 1.0: never upscale (drawBitmap1Bit also won't upscale beyond maxW/maxH).
          const float scale = std::min(1.0f, std::min(scaleX, scaleY));
          const int rendW = static_cast<int>(imgW * scale);
          const int rendH = static_cast<int>(imgH * scale);
          const int offsetX = std::max(1, (tw - rendW) / 2);
          const int offsetY = std::max(1, (th - rendH) / 2);
          // Pre-clear only the exact rendered image area; the black selection background
          // shows through in the surrounding space.
          renderer.fillRect(cellX + offsetX, cellY + offsetY, rendW, rendH, false);
          renderer.drawBitmap1Bit(bmp, cellX + offsetX, cellY + offsetY, rendW, rendH);
          thumbDrawn = true;
        }
      }
      file.close();
    }
    if (!thumbDrawn) {
      // Thumbnail not yet generated — clear interior and show loading label
      renderer.fillRect(cellX + 1, cellY + 1, tw - 2, th - 2, false);
      const char* loadingText = tr(STR_LOADING);
      const int textW = renderer.getTextWidth(SMALL_FONT_ID, loadingText);
      const int textH = renderer.getLineHeight(SMALL_FONT_ID);
      renderer.drawText(SMALL_FONT_ID, cellX + (tw - textW) / 2, cellY + (th - textH) / 2, loadingText, true);
    }
  } else {
    // No cover — clear the whole interior so the placeholder looks clean.
    renderer.fillRect(cellX + 1, cellY + 1, tw - 2, th - 2, false);
  }

  // Reading-progress overlay on the cover: bottom-edge bar while in progress,
  // folded corner when finished, nothing for unread books.
  const int progressPercent = (index >= 0 && index < static_cast<int>(bookProgress.size())) ? bookProgress[index] : -1;
  UITheme::drawCoverProgressIndicator(renderer, Rect{cellX, cellY, tw, th}, progressPercent);

  // Label: title line 1, author line 2; white text on black for selected, black on white otherwise
  const bool black = !selected;
  std::string titleStr = renderer.truncatedText(SMALL_FONT_ID, book.title.c_str(), labelW);
  renderer.drawText(SMALL_FONT_ID, cellX + 2, labelY, titleStr.c_str(), black);
  if (!book.author.empty()) {
    std::string authorStr = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), labelW);
    renderer.drawText(SMALL_FONT_ID, cellX + 2, labelY + 17, authorStr.c_str(), black);
  }
}

// Column count for row-wise navigation. Recomputed rather than cached: it depends on the theme
// metrics, which the settings screen can change while this activity is on the stack.
int RecentBooksActivity::gridColumns() const { return computeGridLayout(renderer).cells.cols; }

void RecentBooksActivity::renderGridView(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const GridLayout layout = computeGridLayout(renderer);
  const Rect contentRect = layout.content;
  const int contentTop = layout.contentTop;
  const int contentHeight = layout.contentHeight;
  const int margin = CoverGridLayout::kMargin;
  const int cols = layout.cells.cols;
  const int tw = layout.cells.cellWidth;
  const int th = layout.cells.cellHeight;
  const int cellHeight = layout.cells.rowStride;
  const int visibleRows = layout.cells.rows;
  const int totalRows = (static_cast<int>(recentBooks.size()) + cols - 1) / cols;
  const int selectedRow = selectorIndex / cols;
  const int pageStartRow = (selectedRow / visibleRows) * visibleRows;
  const int startIndex = pageStartRow * cols;
  const int labelW = layout.cells.labelWidth;

  auto cellPos = [&](int i, int& cx, int& cy) {
    const int row = (i / cols) - pageStartRow;
    const int col = i % cols;
    cx = contentRect.x + margin + col * (tw + margin);
    cy = contentTop + row * cellHeight;
  };
  LOG_DBG("RBA", "Render grid: sel=%d prev=%d start=%d pageStartRow=%d visibleRows=%d totalRows=%d", selectorIndex,
          prevSelectorIndex, startIndex, pageStartRow, visibleRows, totalRows);
  // Partial fast path: only the selection changed within the same page
  const int prevPage = prevSelectorIndex >= 0 ? (prevSelectorIndex / cols / visibleRows) : -1;
  const int curPage = selectedRow / visibleRows;
  if (!fullRedrawNeeded && prevSelectorIndex >= 0 && prevSelectorIndex != selectorIndex && prevPage == curPage) {
    // The write framebuffer holds the frame from two refreshes ago (displayBuffer()
    // swaps buffers), which still shows an older selection. Resync it to the
    // displayed frame before patching just the two affected cells; without this,
    // the stale highlight ships back to the panel and multiple cells appear selected.
    LOG_DBG("RBA", "Partial grid redraw: sel=%d prev=%d", selectorIndex, prevSelectorIndex);
    renderer.syncWriteBufferFromDisplayed();
    int cx, cy;
    cellPos(prevSelectorIndex, cx, cy);
    renderGridCell(prevSelectorIndex, false, cx, cy, tw, th, labelW);
    cellPos(selectorIndex, cx, cy);
    renderGridCell(selectorIndex, true, cx, cy, tw, th, labelW);
    prevSelectorIndex = selectorIndex;
    renderer.displayBuffer();
    return;
  }

  // Full redraw
  fullRedrawNeeded = false;
  prevSelectorIndex = selectorIndex;

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_MENU_RECENT_BOOKS));

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_RECENT_BOOKS));
    const auto labels = mappedInput.mapLabels(tr(STR_HOME), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int endIndex = std::min(startIndex + visibleRows * cols, static_cast<int>(recentBooks.size()));
  LOG_DBG("RBA", "Full grid redraw: sel=%d prev=%d", selectorIndex, prevSelectorIndex);
  for (int i = startIndex; i < endIndex; i++) {
    int cx, cy;
    cellPos(i, cx, cy);
    renderGridCell(i, i == selectorIndex, cx, cy, tw, th, labelW);
  }

  // Scroll arrows when content spans multiple pages
  if (totalRows > visibleRows) {
    constexpr int arrowSize = 6;
    const int centerX = contentRect.x + contentRect.width / 2;
    if (pageStartRow > 0) {
      const int arrowY = contentTop + 2;
      for (int j = 0; j < arrowSize; ++j) {
        const int half = arrowSize - 1 - j;
        renderer.drawLine(centerX - half, arrowY + j, centerX + half, arrowY + j);
      }
    }
    if (pageStartRow + visibleRows < totalRows) {
      const int arrowY = contentTop + contentHeight - arrowSize - 2;
      for (int j = 0; j < arrowSize; ++j) {
        renderer.drawLine(centerX - j, arrowY + j, centerX + j, arrowY + j);
      }
    }
  }

  // On X4 (taller screen) there is room for a one-line gesture hint below the grid.
  if (!gpio.deviceIsX3()) {
    const int hintY = contentRect.y + contentRect.height - metrics.verticalSpacing - 14;
    const std::string hint = std::string(tr(STR_DIR_UP)) + "+L: " + tr(STR_VIEW_GRID) + "/" + tr(STR_VIEW_LIST) +
                             "   " + tr(STR_DIR_LEFT) + "+L: " + tr(STR_REMOVE) + "   " + tr(STR_DIR_RIGHT) +
                             "+L: " + tr(STR_INFO);
    renderer.drawText(SMALL_FONT_ID, contentRect.x + metrics.contentSidePadding, hintY, hint.c_str());
  }

  const auto hints = mappedInput.mapHints(tr(STR_HOME), tr(STR_OPEN), "", "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}
