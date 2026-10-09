#include "FileBrowserActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalCapabilities.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <SidecarFiles.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>

#include "../ActivityManager.h"
#include "../ActivityResult.h"
#include "../ListRowTap.h"
#include "../reader/FinishedBookActivity.h"
#include "../reader/ReaderActivity.h"
#include "../util/BmpViewerActivity.h"
#include "../util/ConfirmationActivity.h"
#include "../util/KeyboardEntryActivity.h"
#include "BookDetails.h"
#include "BookInfoActivity.h"
#include "CoverThumbLoader.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileContextMenuActivity.h"
#include "KOReaderCredentialStore.h"
#include "LibraryFreshness.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/BookProgressPresentation.h"
#include "components/UITheme.h"
#include "components/icons/folder.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// The builder's resolver: the author the book lists already show, from details.bin when it is there
// and from the book (and its sidecar) when not. False -- try again next build -- when that parse
// could not run just now.
bool resolveAuthor(void*, const std::string& path, const uint32_t size, LibraryBuilder::Author& out,
                   BuildArena* scratch) {
  BookDetails details;
  if (!BookDetailsLookup::cached(path, size, details) && !BookDetailsLookup::parse(path, size, details, scratch)) {
    return false;
  }
  out.name = std::move(details.primaryAuthor);
  out.fileAs = std::move(details.authorSort);
  return true;
}
}  // namespace

FileBrowserActivity::FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string initialPath, std::string focusName, const Mode mode)
    : UiListActivity("FileBrowser", renderer, mappedInput),
      model(mode),
      focusName(std::move(focusName)),
      coverLoader(std::make_unique<CoverThumbLoader>()) {
  model.setPath(std::move(initialPath));
}

FileBrowserActivity::~FileBrowserActivity() = default;

// A row's display name (defined with materializeListWindow below).
std::string getFileName(std::string filename);

void FileBrowserActivity::onEnter() {
  // Recent Books keeps the home screen's snapshot of each book's details current, as the screen
  // it replaced did on the way in: a metadata sidecar may have changed since the book was read.
  // One load of the recent-books list for both the refresh and the rows (loadRecents copies the
  // paths out), let go again before the screen draws: the list is not kept while browsing.
  std::optional<RecentBooksStore::Hold> recents;
  if (model.getMode() == Mode::Recents) {
    recents.emplace();
    RECENT_BOOKS.refreshSidecarMetadata(static_cast<size_t>(RECENT_BOOKS.getCount()));
  }
  model.load();
  recents.reset();
  startLibraryBuildIfStale();
  int selectedIndex = 0;

  if (!focusName.empty()) {
    const size_t idx = model.findEntry(focusName);
    if (idx < model.entryCount()) {
      selectedIndex = static_cast<int>(idx);
    }
    focusName.clear();
  }

  // The thumbnail the grid's cells take on this panel -- the Recent Books grid's, cell for cell.
  // The UI is always portrait and the theme only changes in Settings, so it holds while this is open.
  const CoverGridLayout::Layout cells = coverGrid().cells;
  const std::pair<int, int> gridThumb{cells.thumbWidth, cells.thumbHeight};
  coverLoader->configure(&gridThumb, 1, CoverGridLayout::kThumbCrop);

  RenderLock lock(*this);
  UiListActivity::onEnter();
  resetNavigation(selectedIndex);
}

void FileBrowserActivity::onExit() {
  UiListActivity::onExit();
  libraryBuilder.reset();  // abandoned: the next visit walks again; details.bin keeps the resolves
  model.clear();
  bookRows.release();
  // ActivityManager::exitActivity holds the render lock around onExit().
  returnLentBuffer(/*callerHoldsRenderLock=*/true);
  coverFailed.clear();
  coverFailed.shrink_to_fit();
}

void FileBrowserActivity::startActivityForResult(std::unique_ptr<Activity>&& activity,
                                                 ActivityResultHandler resultHandler) {
  returnLentBuffer(/*callerHoldsRenderLock=*/false);
  UiListActivity::startActivityForResult(std::move(activity), std::move(resultHandler));
}

void FileBrowserActivity::loop() {
  UiListActivity::loop();
  if (bookRows.hasPending()) {
    // Titles first: they are quick, and a title card wants its title. An OPF parse's inflate ring
    // is up to 32 KB of contiguous memory, more than an X3 holding both framebuffers can spare
    // ("Failed to init inflate reader" on a 350 KB OPF), so it runs in the borrowed framebuffer as
    // the covers do. A cover in progress holds blocks of that region: it is dropped, and started
    // again once the titles are done.
    if (!renderer.isComposingFrame()) {
      coverLoader->reset();
      lendForBackgroundWork();
    }
    if (bookRows.resolveOne(renderer, coverScratch.get())) requestUpdate();
    return;
  }
  if (stepLibraryBuild()) return;
  generateCovers();
}

// The view chosen for this list in Options: Browse Files has one, Recent Books its own
// (APP_STATE.recentBooksView), and the other browsers list files by name.
uint8_t FileBrowserActivity::chosenView() const {
  switch (model.getMode()) {
    case Mode::Books:
      return SETTINGS.fileBrowserView;
    case Mode::Recents:
      return APP_STATE.recentBooksView;
    default:
      return CrossPointSettings::BROWSER_VIEW_FILES;
  }
}

// Details chosen. A card-wide search keeps filenames: its rows are paths, and the folder they sit
// in is what tells two same-named results apart.
bool FileBrowserActivity::detailsView() const {
  return chosenView() == CrossPointSettings::BROWSER_VIEW_DETAILS && !model.isDeepSearch();
}

// Covers chosen, on the same terms as detailsView().
bool FileBrowserActivity::coversView() const {
  return chosenView() == CrossPointSettings::BROWSER_VIEW_COVERS && !model.isDeepSearch();
}

// What a row is called before its details are known: its filename -- for a Recent Books row, whose
// name is a path from the root, the last part of it.
std::string FileBrowserActivity::rowName(const std::string& entry) const {
  if (model.getMode() != Mode::Recents) return getFileName(entry);
  const size_t slash = entry.rfind('/');
  return getFileName(slash == std::string::npos ? entry : entry.substr(slash + 1));
}

// The cover grid on this panel: one layout for Browse Files and Recent Books alike, so a book has one
// grid thumbnail, made at the size the cells come out at and drawn 1:1 -- never resampled, which
// would alias its dither into a visible grid.
FileBrowserActivity::CoverGrid FileBrowserActivity::coverGrid() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  CoverGrid g;
  g.content = UITheme::getContentRect(renderer, true, true);
  g.top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // The 12 px floor at the bottom is where the scroll arrows go.
  g.cells = CoverGridLayout::compute({.contentWidth = g.content.width,
                                      .contentHeight = g.content.height - g.top - metrics.verticalSpacing,
                                      .bottomReserve = 12,
                                      .maxCellHeight = CoverGridLayout::kMaxCellHeight,
                                      .maxCellWidth = CoverGridLayout::kMaxCellWidth});
  g.perPage = std::max(1, g.cells.cols * g.cells.rows);
  return g;
}

void FileBrowserActivity::afterUiRender() {
  if (coversView()) drawCoverGrid();
}

void FileBrowserActivity::drawCoverGrid() {
  const int total = listCount();
  if (total == 0) return;
  const CoverGrid g = coverGrid();
  const int start = (nav.selected / g.perPage) * g.perPage;
  const int end = std::min(total, start + g.perPage);
  for (int i = start; i < end; ++i) {
    const int slot = i - start;
    const int x =
        g.content.x + CoverGridLayout::kMargin + (slot % g.cells.cols) * (g.cells.cellWidth + CoverGridLayout::kMargin);
    const int y = g.top + (slot / g.cells.cols) * g.cells.rowStride;
    drawCoverCell(i, x, y, g.cells, i == nav.selected);
  }
}

// One cell: the cover slot, then two lines under it. The slot is a 2:3 card centred in the cell --
// the shape of the usual cover, so fitted covers, title cards and folders line up. Selection is
// the recent-books grid's: the whole cell inverted, the cover itself left as it is.
void FileBrowserActivity::drawCoverCell(const int index, const int x, const int y, const CoverGridLayout::Layout& cells,
                                        const bool selected) {
  const int tw = cells.cellWidth;
  const int th = cells.cellHeight;
  if (selected) renderer.fillRect(x, y, tw, th + CoverGridLayout::kLabelHeight + 3);
  const int cardH = th - 2;
  const int cardW = std::min(tw - 2, cardH * 2 / 3);
  const Rect card{x + (tw - cardW) / 2, y + 1, cardW, cardH};

  const std::string entry = model.entryName(static_cast<size_t>(index));
  if (entry.empty()) return;
  const std::string path = model.entryFullPath(static_cast<size_t>(index));
  std::string title;
  std::string subtitle;
  if (entry.back() == '/') {
    title = utf8NfcNorm(entry.substr(0, entry.size() - 1));
    const int books = bookRows.folder(path).bookCount;
    drawFolderCard(card, books);
  } else {
    const BookRowResolver::Row& row = bookRows.row(path, model.entrySize(static_cast<size_t>(index)));
    title = row.title.empty() ? rowName(entry) : row.title;
    subtitle = row.subtitle;
    const int percent = row.percent;  // copied before anything else can reuse the row's slot
    Rect cover = card;
    const CoverThumb thumb = drawCoverThumb(path, x, y, tw, th, cover);
    if (thumb != CoverThumb::Drawn) drawTitleCard(title, card);
    if (thumb == CoverThumb::Missing) coverWork = true;
    // Framed tight round what is drawn: a fitted cover is narrower than the cell.
    renderer.drawRect(cover.x - 1, cover.y - 1, cover.width + 2, cover.height + 2, !selected);
    BookProgressPresentation::drawIndicator(renderer, Rect{cover.x - 1, cover.y - 1, cover.width + 2, cover.height + 2},
                                            percent);
  }

  const int labelY = y + th + 3;
  const bool black = !selected;
  renderer.drawText(SMALL_FONT_ID, x + 2, labelY,
                    renderer.truncatedText(SMALL_FONT_ID, title.c_str(), cells.labelWidth).c_str(), black);
  if (!subtitle.empty()) {
    renderer.drawText(SMALL_FONT_ID, x + 2, labelY + 17,
                      renderer.truncatedText(SMALL_FONT_ID, subtitle.c_str(), cells.labelWidth).c_str(), black);
  }
}

// The book's cover, from the grid thumbnail the recent-books grid or generateCovers() made (on a
// panel whose cells take the full-size box, the finished-book screen's too), centred in the cell; `drawn` is where it
// landed. NoCover is the 1x1 placeholder of a book that has none; Missing is a thumbnail not made yet, or cut short.
FileBrowserActivity::CoverThumb FileBrowserActivity::drawCoverThumb(const std::string& bookPath, const int x,
                                                                    const int y, const int tw, const int th,
                                                                    Rect& drawn) {
  FsFile file;
  if (!Storage.openFileForRead("FBR", coverLoader->thumbPath(bookPath), file)) return CoverThumb::Missing;
  Bitmap bmp(file);
  CoverThumb result = CoverThumb::Missing;  // unreadable, or cut short by an interrupted write
  if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete()) {
    if (bmp.getWidth() <= 1 || bmp.getHeight() <= 1) {
      result = CoverThumb::NoCover;
    } else {
      // Never upscaled -- and a current thumbnail always fits, so never scaled at all.
      const float scale = std::min(
          1.0f, std::min(static_cast<float>(tw - 2) / bmp.getWidth(), static_cast<float>(th - 2) / bmp.getHeight()));
      drawn.width = static_cast<int>(bmp.getWidth() * scale);
      drawn.height = static_cast<int>(bmp.getHeight() * scale);
      drawn.x = x + std::max(1, (tw - drawn.width) / 2);
      drawn.y = y + std::max(1, (th - drawn.height) / 2);
      renderer.fillRect(drawn.x, drawn.y, drawn.width, drawn.height, false);  // white under it, on a selection
      renderer.drawBitmap1Bit(bmp, drawn.x, drawn.y, drawn.width, drawn.height);
      result = CoverThumb::Drawn;
    }
  }
  file.close();
  return result;
}

// A book without a cover thumbnail: its title, set large on a blank card.
void FileBrowserActivity::drawTitleCard(const std::string& title, const Rect& card) {
  renderer.fillRect(card.x, card.y, card.width, card.height, false);
  constexpr int pad = 10;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int maxLines = std::max(1, std::min(6, (card.height - 2 * pad) / lineH));
  const auto lines =
      renderer.wrappedText(UI_12_FONT_ID, title.c_str(), card.width - 2 * pad, maxLines, EpdFontFamily::BOLD);
  int lineY = card.y + (card.height - static_cast<int>(lines.size()) * lineH) / 2;
  for (const auto& line : lines) {
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, card.x + (card.width - lineW) / 2, lineY, line.c_str(), true, EpdFontFamily::BOLD);
    lineY += lineH;
  }
}

// A folder at the size of a cover: a tab over a body, drawn in outline, and in the body how many
// books are in it and below it -- "..." until BookRowResolver has counted them.
void FileBrowserActivity::drawFolderCard(const Rect& card, const int books) {
  constexpr int line = 3;
  const int tabW = card.width * 2 / 5;
  const int tabH = std::max(12, card.height / 14);
  const int bodyTop = card.y + card.height / 8;  // a folder is wider than tall; a cover is not
  const int bodyH = card.height - (bodyTop - card.y) - card.height / 8;
  renderer.fillRect(card.x, bodyTop - tabH, tabW, tabH + line, false);
  renderer.fillRect(card.x, bodyTop, card.width, bodyH, false);
  for (int i = 0; i < line; ++i) {
    renderer.drawRect(card.x + i, bodyTop - tabH + i, tabW - 2 * i, tabH + line);
    renderer.drawRect(card.x + i, bodyTop + i, card.width - 2 * i, bodyH - 2 * i);
  }

  std::string number = "...";
  if (books > FileBrowserModel::MAX_COUNTED_BOOKS) {
    number = std::to_string(FileBrowserModel::MAX_COUNTED_BOOKS) + "+";
  } else if (books >= 0) {
    number = std::to_string(books);
  }
  const char* noun = books == 1 ? tr(STR_BOOK_SINGULAR) : tr(STR_BOOK_PLURAL);
  const int numberH = renderer.getLineHeight(UI_12_FONT_ID);
  const int nounH = renderer.getLineHeight(SMALL_FONT_ID);
  const int textY = bodyTop + (bodyH - numberH - nounH) / 2;
  const int numberW = renderer.getTextWidth(UI_12_FONT_ID, number.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, card.x + (card.width - numberW) / 2, textY, number.c_str(), true,
                    EpdFontFamily::BOLD);
  if (books >= 0) {
    const int nounW = renderer.getTextWidth(SMALL_FONT_ID, noun);
    renderer.drawText(SMALL_FONT_ID, card.x + (card.width - nounW) / 2, textY + numberH, noun, true);
  }
}

// One burst of cover making for the page on screen: carry on with the book in hand if it is still
// on that page, else start on the first book there without a cover. A cover that lands is shown
// at once -- one redraw per cover, not per decode slice -- and the next burst moves on.
void FileBrowserActivity::generateCovers() {
  if (!coversView()) {
    returnLentBuffer(/*callerHoldsRenderLock=*/false);
    return;
  }
  if (!coverWork && !coverLoader->busy()) return;
  if (renderer.isComposingFrame() || mappedInput.hasPendingInput()) return;

  if (!coverLoader->busy()) {
    std::string next;
    {
      RenderLock lock(*this);  // the model's index file is the render task's too
      const int total = listCount();
      const int perPage = coverGrid().perPage;
      const int start = (nav.selected / perPage) * perPage;
      for (int i = start; i < std::min(total, start + perPage) && next.empty(); ++i) {
        const std::string entry = model.entryName(static_cast<size_t>(i));
        if (entry.empty() || entry.back() == '/') continue;
        const std::string path = model.entryFullPath(static_cast<size_t>(i));
        if (std::find(coverFailed.begin(), coverFailed.end(), path) != coverFailed.end()) continue;
        if (!coverLoader->complete(path)) next = path;
      }
    }
    if (next.empty()) {
      coverWork = false;
      returnLentBuffer(/*callerHoldsRenderLock=*/false);
      return;
    }
    // Without the lend the decoders fall back to the heap, as the Recent Books grid always does.
    lendForBackgroundWork();
    coverLoader->begin(next, coverScratch ? coverScratch.get() : nullptr);
  } else {
    // The selection may have moved off the page since the last burst: then this book can wait.
    RenderLock lock(*this);
    const int perPage = coverGrid().perPage;
    const int start = (nav.selected / perPage) * perPage;
    bool onPage = false;
    for (int i = start; i < std::min(listCount(), start + perPage) && !onPage; ++i) {
      onPage = model.entryFullPath(static_cast<size_t>(i)) == coverLoader->book();
    }
    if (!onPage) {
      coverLoader->reset();
      coverWork = true;  // look again at the page that is on screen now
      return;
    }
  }

  constexpr uint32_t COVER_SLICE_BUDGET_MS = 150;
  // Full speed for the burst: this runs between presses, which is when the governor has the clock
  // down at 10 MHz.
  HalPowerManager::Lock fullSpeed;
  const uint32_t deadline = millis() + COVER_SLICE_BUDGET_MS;
  const std::string book = coverLoader->book();
  while (true) {
    const CoverThumbLoader::Step step = coverLoader->step();
    if (step == CoverThumbLoader::Step::Done) {
      requestUpdate();
      return;
    }
    if (step == CoverThumbLoader::Step::Failed) {
      constexpr size_t MAX_REMEMBERED_FAILURES = 32;
      if (coverFailed.size() >= MAX_REMEMBERED_FAILURES) coverFailed.erase(coverFailed.begin());
      coverFailed.push_back(book);
      return;
    }
    if (mappedInput.hasPendingInput() || renderer.isComposingFrame() ||
        static_cast<int32_t>(millis() - deadline) >= 0) {
      return;  // resume where it stands on the next loop()
    }
  }
}

// Borrows the secondary framebuffer for the title parses and the cover decoders, as
// HomeActivity::loadRecentCovers does and for the same reasons it gives: decoding beside two full
// framebuffers has taken free heap to a few KB. Before the lend: the write buffer is two frames old after the last
// swap, so it is brought up to the frame on the panel, and on the X4 RED RAM is seeded with that frame, which
// single-buffer fast diff requires.
bool FileBrowserActivity::lendForBackgroundWork() {
  if (lentRegion != nullptr) return true;
  if (!renderer.hasSecondaryBuffer()) return false;
  RenderLock lock(*this);
  renderer.syncWriteBufferFromDisplayed();
  if (!renderer.isX3()) renderer.syncRedRamFromFrameBuffer();
  size_t size = 0;
  uint8_t* region = renderer.borrowSecondaryBuffer(&size);
  if (region == nullptr) return false;
  coverScratch = makeUniqueNoThrow<BuildArena>(region, size);
  if (!coverScratch || !coverScratch->valid()) {
    coverScratch.reset();
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    return false;
  }
  lentRegion = region;
  renderer.setSingleBufferFastDiff(true);
  LOG_INF("FBR", "Lent secondary framebuffer for titles and covers (%u bytes, free=%lu)", static_cast<unsigned>(size),
          static_cast<unsigned long>(esp_get_free_heap_size()));
  return true;
}

// Everything holding a block of the region goes first, then the region goes back to the display.
// onExit() calls this under the render lock ActivityManager already holds; taking it again would
// deadlock, hence the flag.
void FileBrowserActivity::returnLentBuffer(const bool callerHoldsRenderLock) {
  // Nothing borrowed and nothing in hand is the common case, checked every loop() outside Covers:
  // answer it without the lock.
  if (lentRegion == nullptr && !coverLoader->busy()) return;
  const auto doReturn = [this] {
    coverLoader->reset();
    if (lentRegion == nullptr) return;
    coverScratch.reset();
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    renderer.setSingleBufferFastDiff(false);
    lentRegion = nullptr;
    Epub::clearCoverMetadataMemo();
    LOG_INF("FBR", "Returned secondary framebuffer after titles and covers (free=%lu contig=%lu)",
            static_cast<unsigned long>(esp_get_free_heap_size()),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  };
  if (callerHoldsRenderLock) {
    doReturn();
  } else {
    RenderLock lock(*this);
    doReturn();
  }
}

// A tap on a cover: the first selects it, a second opens it -- the list's two-step, through the
// same hit test the recent-books grid uses. A swipe turns the page.
bool FileBrowserActivity::handleCoverTouch() {
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageSelection(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return true;
  }
  const int total = listCount();
  if (total == 0) return false;
  const CoverGrid g = coverGrid();
  const int pageStartRow = (nav.selected / g.perPage) * g.cells.rows;
  int index = -1;
  const auto hit = [&](const int px, const int py) {
    index = CoverGridLayout::hitTest(g.cells, g.content.x, g.top, pageStartRow, total, px, py);
    return index >= 0;
  };
  int px = 0;
  int py = 0;
  // Down is claimed but not acted on, so the same contact cannot also be read by anything else.
  if (mappedInput.wasScreenTouchDown(px, py) && hit(px, py)) return true;
  if (!(mappedInput.wasScreenTapped(px, py) && hit(px, py))) return false;
  switch (ListRowTap::apply(index, total, nav.selected)) {
    case ListRowTap::Result::Rejected:
      return true;
    case ListRowTap::Result::Selected:
      requestUpdate();
      return true;
    case ListRowTap::Result::Activate:
      activateIndex(index);
      return true;
  }
  return true;
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

// Iterative post-order traversal: clear book caches then delete files/dirs.
// Adapted from upstream PR #1892 (WuTofu) to handle our EPUB+XTC cache clearing.
bool FileBrowserActivity::removeDirRecursive(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FBR", "Failed to open for removal: %s", fullPath.c_str());
    return false;
  }
  if (!file.isDirectory()) {
    file.close();
    clearFileMetadata(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  constexpr size_t NAME_BUF = 500;
  char nameBuf[NAME_BUF];

  // Stack of (path, postOrder): postOrder=true means rmdir this path after its children.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FBR", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir || !dir.isDirectory()) {
      LOG_ERR("FBR", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }

    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(nameBuf, NAME_BUF);
      if (strcmp(nameBuf, ".") == 0 || strcmp(nameBuf, "..") == 0) continue;
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') entryPath += '/';
      entryPath += nameBuf;
      const bool isDir = entry.isDirectory();
      entry.close();
      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearFileMetadata(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FBR", "Failed to remove file: %s", entryPath.c_str());
          dir.close();
          return false;
        }
      }
    }
    dir.close();
  }
  return true;
}

bool FileBrowserActivity::handleCustomInput() {
  // The grid has no list for the base class to route a touch to.
  if (coversView() && handleCoverTouch()) return true;
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back) {
      // Recent Books is one list with nowhere to go up to: Back, short or long, is Home.
      if (model.getMode() == Mode::Recents) {
        onGoHome();
        return true;
      }
      if (ev.type == ButtonEventManager::PressType::Long) {
        if (model.getMode() == Mode::Books) {
          onGoHome();
          return true;
        }
        // PickFirmware: long Back = same as short Back (cancel / up dir)
      }
      // A search is the first thing Back undoes. Without this, a search that matched nothing in
      // the root folder leaves the screen empty with Home as the only way out, which throws away
      // where you were standing.
      if (model.isFiltered() || model.isDeepSearch()) {
        {
          RenderLock lock(*this);
          model.setFilter("");
          model.clearSearch();
          resetNavigation(0);
        }
        requestUpdate();
        return true;
      }
      if (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long) {
        if (model.path() != "/") {
          std::string parent = model.path();
          while (parent.size() > 1 && parent.back() == '/') parent.pop_back();
          const std::string oldPath = parent;
          parent.replace(parent.find_last_of('/'), std::string::npos, "");
          // The rows are about to be replaced. Any contact still queued was aimed at the folder
          // we are leaving, and the list it would land in is a different one.
          mappedInput.flushTouchEvents();
          model.setPath(std::move(parent));  // empty -> "/"
          model.load();
          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          const size_t idx = model.findEntry(dirName);
          resetNavigation((idx < model.entryCount()) ? static_cast<int>(idx) : 0);
          requestUpdate();
        } else if (model.getMode() != Mode::Books) {
          // At root in a picker: cancel back to caller.
          ActivityResult res;
          res.isCancelled = true;
          setResult(std::move(res));
          finish();
        } else {
          onGoHome();
        }
        return true;
      }
    }

    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      if (confirmOpensOptions()) {
        openContextMenu();
        return true;
      }
      activateSelected(ev.type == ButtonEventManager::PressType::Long);
      return true;
    }

    // Picking a destination: the page-forward slot commits and the page-back slot makes a folder.
    // Those two are a real key on a board that has them and a tappable box on one that does not,
    // so both are reachable everywhere without a hold. A folders-only list is short enough that
    // losing the page buttons to them costs little, and the side keys and a swipe still scroll.
    if (model.getMode() == Mode::PickFolder && ev.type == ButtonEventManager::PressType::Short) {
      if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right)) {
        ActivityResult res{FilePathResult{model.path()}};
        res.isCancelled = false;
        setResult(std::move(res));
        finish();
        return true;
      }
      if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left)) {
        createFolderHere();
        return true;
      }
    }

    // The cover grid moves as the Recent Books grid does: Up/Down a row, Left/Right a cover, all
    // wrapping at the ends, and the page follows the selection. Left and Right are needed here even
    // on a board without those keys -- on the T5S3 they are the hint strip's boxes -- and a press
    // that paged would skip the covers beside the selection. Options stays on a long Right.
    if (coversView() && ev.type == ButtonEventManager::PressType::Short && moveInCoverGrid(ev.button)) {
      return true;
    }

    // Logical Left/Right page through the list, one screenful per press — the same thing they do in
    // the chapter selector, and the reason the context menu moved to a long press on Right. Paging
    // is driven from the event stream rather than ButtonNavigator: the navigator acts on the press
    // edge, which would page on the way into every long press.
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) &&
        ev.type == ButtonEventManager::PressType::Short && listPages()) {
      pageSelection(1);
      return true;
    }

    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left) &&
        ev.type == ButtonEventManager::PressType::Short && listPages()) {
      pageSelection(-1);
      return true;
    }

    // Options: a long press on the page-forward button, and a short press when the folder fits on
    // one screen and there is nothing to page. Either way the button hint says which one it is —
    // and it rides the same logical button, so rotating the device never separates the two.
    const bool optionsPress = (ev.type == ButtonEventManager::PressType::Long) ||
                              (ev.type == ButtonEventManager::PressType::Short && !listPages() && !coversView());
    if (model.getMode() != Mode::PickFolder &&
        MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) && optionsPress) {
      // Open the context menu for any selection. openContextMenu() shows
      // file-specific actions for supported files and the browser display
      // options (sort + visibility) for directories / unsupported types.
      openContextMenu();
      return true;
    }
  }
  return false;
}

void FileBrowserActivity::navigateButtons() {
  if (coversView()) return;  // the grid moves by rows and covers, from handleCustomInput()
  bool changed = false;
  {
    RenderLock lock(*this);
    const int previous = nav.selected;
    const int pageSize = listPageSize();
    buttonNavigator.onNextList(
        ButtonNavigator::getStepNextButtons(), nav.selected, listCount(), [&changed] { changed = true; }, pageSize);
    buttonNavigator.onPreviousList(
        ButtonNavigator::getStepPreviousButtons(), nav.selected, listCount(), [&changed] { changed = true; }, pageSize);
    // The grid shows whichever page holds the selection; only the list keeps a viewport to follow.
    if (changed && !coversView()) {
      if (std::abs(nav.selected - previous) > 2) {
        nav.top = nav.selected;
      } else {
        nav.follow(listCount());
      }
    }
  }
  if (changed) requestUpdate();
}

void FileBrowserActivity::activateSelected(const bool longPress) {
  if (model.entryCount() == 0 || nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string entry = model.entryName(static_cast<size_t>(nav.selected));
  if (entry.empty()) return;
  const bool isDirectory = entry.back() == '/';
  if (isDirectory) {
    if (longPress) return;
    std::string child = model.path();
    if (child.back() != '/') child += "/";
    child += entry.substr(0, entry.length() - 1);
    // As in the Back branch: drop contacts aimed at the folder we are leaving.
    mappedInput.flushTouchEvents();
    model.setPath(std::move(child));
    model.load();
    resetNavigation();
    requestUpdate();
    return;
  }
  if (model.getMode() == Mode::PickFirmware) {
    std::string cleanBasePath = model.path();
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }
  // All files lists what the reader cannot open too. Selecting one of those offers what can be
  // done with it -- move it, remove it -- instead of handing the reader a file it would reject.
  if (model.getMode() == Mode::AllFiles && !FileBrowserModel::isOpenable(entry)) {
    openContextMenu();
    return;
  }

  std::string fullPath = model.entryFullPath(static_cast<size_t>(nav.selected));
  if (longPress && KOREADER_STORE.hasCredentials() && FsHelpers::hasEpubExtension(fullPath)) {
    auto& sync = APP_STATE.koReaderSyncSession;
    sync.autoPullEpubPath = fullPath;
    sync.postAction = KOReaderSyncPostAction::Reader;
    APP_STATE.saveToFile();
  }
  ReturnHint hint;
  hint.target = returnTarget();
  hint.path = model.path();
  hint.selectName = entry;
  activityManager.replaceWithReader(std::move(fullPath), std::move(hint));
}

void FileBrowserActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;
  activateSelected(false);
}

void FileBrowserActivity::resetNavigation(const int selected) {
  listTapActivation.reset();
  app.clearTapFlash();
  const int last = listCount() > 0 ? listCount() - 1 : 0;
  nav.reset(std::max(0, std::min(last, selected)));
}

// Rows one Left/Right press moves. drawList reports what the last render fit — which for wrapped
// rows is not a constant — and before the first render there is nothing to report yet.
int FileBrowserActivity::listPageSize() const {
  return coversView() ? coverGrid().perPage : nav.pageRowsFor(listCount());
}

// True when the folder is longer than one screen. When it is not, paging has nothing to do, so
// Right keeps its old short-press meaning (Options) instead of quietly stepping the selection.
bool FileBrowserActivity::listPages() const { return listCount() > listPageSize(); }

// Moves the selection a screenful, clamped at both ends.
//
// Deliberately relative, not ButtonNavigator::nextPageIndex: that snaps to index-aligned page
// boundaries, which is right for the fixed-height lists it serves but wrong here. Wrapped names
// make rows different heights, so page boundaries are not multiples of anything — snapping from
// index 39 with 8 rows on screen would land on 40 and look like the button moved one row.
//
// Moving the window too, rather than only the selection, is what makes it a page turn. The layout
// keeps the selection on screen but cannot tell a page jump from a step: landing one row past the
// last visible row looks identical either way, so it would scroll by one and put the selection on
// the bottom row — a "page" showing a single row the reader had not already seen. Anchoring the
// window to the new selection puts a full screen of new names above it instead.
void FileBrowserActivity::pageSelection(const int direction) {
  const int total = listCount();
  if (total <= 0) return;
  const int target = nav.selected + direction * listPageSize();
  nav.selected = std::max(0, std::min(total - 1, target));
  nav.top = nav.selected;
  nav.followPending = false;
  requestUpdate();
}

// One step in the cover grid for a short press on a direction key, wrapping at the ends; false for
// any other key. Up/Down take a whole row (CoverGridLayout::rowAbove/rowBelow), Left/Right one cover.
bool FileBrowserActivity::moveInCoverGrid(const MappedInputManager::Button button) {
  const int total = listCount();
  using Direction = MappedInputManager::Direction;
  const bool up = MappedInputManager::isDirection(button, Direction::Up);
  const bool down = MappedInputManager::isDirection(button, Direction::Down);
  const bool left = MappedInputManager::isDirection(button, Direction::Left);
  const bool right = MappedInputManager::isDirection(button, Direction::Right);
  if (!up && !down && !left && !right) return false;
  if (total <= 0) return true;
  {
    RenderLock lock(*this);
    const int cols = coverGrid().cells.cols;
    if (up) nav.selected = CoverGridLayout::rowAbove(nav.selected, total, cols);
    if (down) nav.selected = CoverGridLayout::rowBelow(nav.selected, total, cols);
    if (left) nav.selected = ButtonNavigator::previousIndex(nav.selected, total);
    if (right) nav.selected = ButtonNavigator::nextIndex(nav.selected, total);
  }
  requestUpdate();
  return true;
}

// Display copy only. FileBrowserModel's entry names, and every path built from them, keep the
// raw directory-entry bytes, because FAT long-filename lookup is byte-exact: opening or
// deleting an NFD entry through an NFC-normalized name fails. Composing here fixes
// rendering -- the fonts carry precomposed Hangul syllables but no conjoining jamo, so a
// Korean filename written by macOS drew as blanks -- without touching what we hand to
// storage.
// Ported from crosspoint-reader PR #3036 (Sung-jin Brian Hong <serialx@serialx.net>).
std::string getFileName(std::string filename) {
  filename = utf8NfcNorm(std::move(filename));
  if (filename.back() == '/') {
    // Bracketed unconditionally. This used to skip the brackets when the theme reported
    // showsFileIcons(), on the assumption that a folder icon carried the distinction -- but
    // buildScreen() below fills in nothing but `label` and `actionValue`, so no row in this
    // browser has ever had an icon, and under Lyra (the one theme that answers true) folders
    // were left indistinguishable from files.
    filename.pop_back();
    return "[" + filename + "]";
  }
  if (SETTINGS.showFileExtensions) {
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

void FileBrowserActivity::materializeListWindow() {
  const int total = listCount();
  windowFirst = static_cast<uint16_t>(std::max(0, std::min(nav.top, total)));
  windowCount = static_cast<uint16_t>(
      std::min(static_cast<size_t>(total - windowFirst), static_cast<size_t>(LIST_WINDOW_CAPACITY)));
  // Details are looked up for the rows that will be drawn, not the whole window: the window runs
  // past the bottom of the screen, and each lookup is a few small file reads.
  const bool details = detailsView();
  const int drawnEnd = nav.top + nav.pageRowsFor(total);
  for (uint16_t offset = 0; offset < windowCount; ++offset) {
    const uint16_t index = static_cast<uint16_t>(windowFirst + offset);
    const std::string entry = model.entryName(index);
    windowLabels[offset] = rowName(entry);
    windowSubtitles[offset].clear();
    windowValues[offset].clear();
    if (details && !entry.empty() && entry.back() != '/' && index < drawnEnd) {
      const BookRowResolver::Row& row = bookRows.row(model.entryFullPath(index), model.entrySize(index));
      if (!row.title.empty()) windowLabels[offset] = row.title;
      windowSubtitles[offset] = row.subtitle;
      windowValues[offset] = row.value;
    }
    windowItems[offset] = {};
    windowItems[offset].label = windowLabels[offset].c_str();
    if (!windowSubtitles[offset].empty()) windowItems[offset].subtitle = windowSubtitles[offset].c_str();
    if (!windowValues[offset].empty()) windowItems[offset].value = windowValues[offset].c_str();
    windowItems[offset].actionValue = static_cast<int16_t>(index);
  }
}

void FileBrowserActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen);

  if (listCount() == 0) {
    fui::TextAreaProps empty;
    empty.text = model.getMode() == Mode::PickFirmware ? tr(STR_NO_BIN_FILES)
                 : model.getMode() == Mode::Recents    ? tr(STR_NO_RECENT_BOOKS)
                                                       : tr(STR_NO_FILES_FOUND);
    empty.style = screen.theme().bodyText;
    empty.showCaret = false;
    screen.textArea(empty);
    return;
  }

  // The grid is drawn straight to the frame after the (otherwise empty) screen, in afterUiRender().
  if (coversView()) return;

  auto props = listProps(screen);
  props.count = static_cast<uint16_t>(listCount());
  // A filename may wrap; a Details row is a title over a subtitle at a fixed two-line height.
  const bool details = detailsView();
  props.labelText.maxLines = details ? 1 : 3;
  syncListViewport(screen, props, /*hasSubtitle=*/details);
  materializeListWindow();
  props.items = windowItems.data();
  props.itemsWindowFirst = windowFirst;
  props.itemsWindowCount = windowCount;
  screen.list(props);
}

void FileBrowserActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const std::string destination = (model.path() == "/") ? std::string(tr(STR_SD_CARD)) : model.path();
  std::string folderName =
      (model.getMode() == Mode::Recents)        ? std::string(tr(STR_MENU_RECENT_BOOKS))
      : (model.getMode() == Mode::PickFirmware) ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
      : (model.getMode() == Mode::PickFolder)
          // Names the DESTINATION in full: "Move here" means the folder being browsed, never the
          // row under the highlight, and the first reading of it is the other way round.
          ? std::string(tr(STR_MOVE_TO_FOLDER)) + ": " + destination
          : (model.path() != "/") ? model.path().substr(model.path().rfind('/') + 1)
            // At the root, All files says which browser this is: the rows alone look like the book one.
            : (model.getMode() == Mode::AllFiles) ? std::string(tr(STR_ALL_FILES))
                                                  : std::string(tr(STR_SD_CARD));
  // A narrowed folder is indistinguishable from a small one unless the header says otherwise.
  if (model.isFiltered()) {
    folderName += ": \"" + model.filter() + "\"";
  }
  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer), folderName.c_str());
}

void FileBrowserActivity::drawFooter() {
  // Only the book browser leaves to Home; the others were opened from somewhere and go back there.
  const char* backLabel =
      (model.path() == "/")
          ? (model.getMode() == Mode::Books || model.getMode() == Mode::Recents ? tr(STR_HOME) : tr(STR_BACK))
          : tr(STR_BACK);
  const bool hasEntries = listCount() > 0;
  bool selectingFirmwareFile = false;
  if (model.getMode() == Mode::PickFirmware && hasEntries) {
    const std::string selectedEntry = model.entryName(static_cast<size_t>(nav.selected));
    selectingFirmwareFile = !selectedEntry.empty() && selectedEntry.back() != '/';
  }
  const char* confirmLabel = !hasEntries             ? ""
                             : selectingFirmwareFile ? tr(STR_SELECT)
                             : confirmOpensOptions() ? tr(STR_OPTIONS)
                                                     : tr(STR_OPEN);
  // The Options menu is available for every entry in Books mode and in the firmware picker, where
  // it is the only place a .bin can be removed (Books mode never lists one). The menu always
  // offers the browser display options (sort + visibility); supported files get
  // extra file-specific actions appended. So the hint shows for files and dirs alike.
  // Same gate as the Right press in handleCustomInput(), so the label and the key never disagree.
  const bool showOptionsHint = model.getMode() != Mode::PickFolder && hasEntries;
  // In a folder worth paging through, Left/Right are the page buttons and the hints say so —
  // Options is then the long press on Right. In a folder that fits on one screen there is nothing
  // to page, so the strip looks exactly as it always did.
  const bool pages = listPages();
  const char* prevLabel = (model.getMode() == Mode::PickFolder) ? tr(STR_NEW) : pages ? tr(STR_LIST_PAGE_PREV) : "";
  // In a folder small enough not to page, this slot carries Options. Where Confirm already
  // carries it that would draw the same word twice on one strip, and the second copy would sit on
  // a slot such a board has no key for.
  // Where it pages, the same key still opens Options on a hold, so the label says both rather
  // than hiding the menu.
  const bool optionsOnRight = showOptionsHint && !confirmOpensOptions();
  const char* nextLabel = (model.getMode() == Mode::PickFolder) ? tr(STR_MOVE_HERE)
                          : pages ? (optionsOnRight ? tr(STR_LIST_PAGE_NEXT_OR_OPTIONS) : tr(STR_LIST_PAGE_NEXT))
                          : optionsOnRight ? tr(STR_OPTIONS)
                                           : "";
  // The cover grid steps a cover on Left and Right (moveInCoverGrid), short press first and the
  // long one after the slash, as the Recent Books grid names them. Always labelled: on the T5S3 the
  // boxes are the only Left and Right there are, and an unlabelled box is not tappable.
  std::string gridRight;
  if (coversView() && hasEntries) {
    prevLabel = tr(STR_DIR_LEFT);
    gridRight = optionsOnRight ? std::string(tr(STR_DIR_RIGHT)) + " / " + tr(STR_OPTIONS) : tr(STR_DIR_RIGHT);
    nextLabel = gridRight.c_str();
  }
  // Paging is bound to logical Left/Right and stepping to logical Up/Down, so which physical pair
  // carries which — and therefore which hint strip each label belongs on — is the orientation's
  // business, not this screen's.
  const auto hints =
      mappedInput.mapHints(backLabel, confirmLabel, prevLabel, nextLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);
}

void FileBrowserActivity::openContextMenu() {
  // If no file selected or a directory selected, show browser options only
  if (model.entryCount() == 0 || nav.selected < 0 || nav.selected >= listCount()) {
    showBrowserOptionsMenu();
    return;
  }

  const std::string entry = model.entryName(static_cast<size_t>(nav.selected));
  if (entry.empty() || entry.back() == '/') {
    showBrowserOptionsMenu(entry);
    return;
  }

  const std::string fullPath = model.entryFullPath(static_cast<size_t>(nav.selected));

  startActivityForResult(std::make_unique<FileContextMenuActivity>(
                             renderer, mappedInput, fullPath, model.getSortMode(), model.getSortDirection(),
                             /*offerDirectoryActions=*/false, model.isFiltered() || model.isDeepSearch(),
                             /*offerGoToFolder=*/model.listsPaths(),
                             /*offerFileManagement=*/managesFiles(),
                             /*offerViewChoice=*/offersViewChoice(),
                             /*recentsList=*/model.getMode() == Mode::Recents),
                         [this, fullPath, entry](const ActivityResult& res) {
                           if (res.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto* menuRes = std::get_if<MenuResult>(&res.data);
                           if (!menuRes) {
                             requestUpdate();
                             return;
                           }
                           handleContextMenuAction(menuRes->action, fullPath, entry, menuRes);
                         });
}

// Make a folder in the directory being browsed.
//
// The name goes through the same FAT sanitiser downloads use, so a name the card cannot hold is
// corrected rather than failing at mkdir with nothing to say.
void FileBrowserActivity::createFolderHere() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FOLDER_NAME), "", 48, InputType::Text),
      [this](const ActivityResult& res) {
        const auto* kb = res.isCancelled ? nullptr : std::get_if<KeyboardResult>(&res.data);
        if (kb == nullptr || kb->text.empty()) {
          requestUpdate();
          return;
        }
        char safe[64];
        FsHelpers::sanitizePathComponentForFat32(kb->text.c_str(), safe, sizeof(safe));
        if (safe[0] == '\0') {
          requestUpdate();
          return;
        }
        std::string target = model.path();
        if (target.back() != '/') target += "/";
        target += safe;

        const bool made = Storage.mkdir(target.c_str(), /*pFlag=*/false);
        if (!made) {
          RenderLock lock(*this);
          renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
          GUI.drawPopup(renderer, tr(STR_NEW_FOLDER_FAILED));
        } else {
          model.load();
          const size_t idx = model.findEntry(std::string(safe) + "/");
          resetNavigation((idx < model.entryCount()) ? static_cast<int>(idx) : 0);
        }
        requestUpdate();
      });
}

// Move a file into a folder the reader picks.
//
// A move on a FAT volume is a rename: the bytes never move, so this is instant whatever the size
// of the book and cannot leave half a file behind if power is lost. The cost is that it works
// within the one volume, which is all there is here.
//
// The book browser does not list a book's sidecars, so a book moved from there takes them along:
// left behind, its cover and metadata corrections would be lost to it and invisible where they
// stayed. All files lists them as files of their own, and moves exactly the one selected.
void FileBrowserActivity::moveToFolder(const std::string& fullPath, const std::string& entry) {
  const bool withSidecars = model.getMode() == Mode::Books;
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", std::string{}, Mode::PickFolder),
      [this, fullPath, entry, withSidecars](const ActivityResult& res) {
        const auto* picked = res.isCancelled ? nullptr : std::get_if<FilePathResult>(&res.data);
        if (picked == nullptr) {
          requestUpdate();
          return;
        }
        std::string target = picked->path;
        if (target.empty()) target = "/";
        if (target.back() != '/') target += "/";
        target += entry;

        const char* message = nullptr;
        if (target == fullPath) {
          message = tr(STR_MOVE_SAME_FOLDER);
        } else if (Storage.exists(target.c_str()) || (withSidecars && SidecarFiles::anyTargetTaken(fullPath, target))) {
          // Renaming onto an existing name is not ours to resolve silently, and FAT would not
          // report which of the two survived.
          message = tr(STR_MOVE_NAME_TAKEN);
        } else if (!Storage.rename(fullPath.c_str(), target.c_str())) {
          message = tr(STR_MOVE_FAILED);
        } else if (withSidecars && !SidecarFiles::moveAll(fullPath, target)) {
          // The book is already in its new folder; saying the move failed would be wrong.
          LOG_ERR("FBR", "Moved %s but not all of its sidecars", fullPath.c_str());
        }

        if (message != nullptr) {
          RenderLock lock(*this);
          renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
          GUI.drawPopup(renderer, message);
        } else {
          clearFileMetadata(fullPath);
          bookRows.clear();
          model.load();
          resetNavigation(nav.selected);
        }
        requestUpdate();
      });
}

// True when Confirm should open the entry's menu rather than the entry itself.
//
// Only on a board with no Back or Confirm key. There, Confirm exists solely as a tap -- on a
// capacitive Home key, or on the hint box -- and every route this screen has to its menu is a
// HOLD: of logical Right, or of a hint box. Neither is a gesture that hardware can make, so
// Delete, Info, Move to folder, New Folder and the sort options have no reachable home at all.
// Opening a file still does: a tap on the row selects it and a second tap opens it, through
// activateIndex() rather than through this button.
//
// A board with the keys keeps Confirm as Open, and its menu one hold of Right away.
bool FileBrowserActivity::confirmOpensOptions() const {
  return managesFiles() && !HalCapabilities::hasBackAndConfirmButtons();
}

// The two browsers a reader organises the card from, rather than picks something in. Both get
// New folder, Move to folder and Remove; the pickers keep to what their pick needs.
bool FileBrowserActivity::managesFiles() const {
  return model.getMode() == Mode::Books || model.getMode() == Mode::AllFiles;
}

// Where the reader comes back to: the browser it was opened from, in the mode it was in.
// Off the list, not off the card: a confirmation naming the book, as the Recent Books screen
// asked before it was a list.
void FileBrowserActivity::removeFromRecents(const std::string& bookPath) {
  std::string title;
  {
    RenderLock lock(*this);  // the render task reads the same rows
    title = bookRows.row(bookPath, 0).title;
  }
  if (title.empty()) {
    const size_t slash = bookPath.rfind('/');
    title = getFileName(slash == std::string::npos ? bookPath : bookPath.substr(slash + 1));
  }
  const std::string heading = std::string(tr(STR_REMOVE_FROM_RECENTS)) + "?";
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, title),
                         [this, bookPath](const ActivityResult& res) {
                           if (!res.isCancelled) {
                             RECENT_BOOKS.removeBook(bookPath);
                             RenderLock lock(*this);
                             bookRows.clear();
                             model.load();
                             resetNavigation(nav.selected);
                           }
                           requestUpdate();
                         });
}

// The views are for books: Browse Files and Recent Books list them; the others list files.
bool FileBrowserActivity::offersViewChoice() const {
  return model.getMode() == Mode::Books || model.getMode() == Mode::Recents;
}

ReturnTo FileBrowserActivity::returnTarget() const {
  switch (model.getMode()) {
    case Mode::AllFiles:
      return ReturnTo::AllFiles;
    case Mode::Recents:
      return ReturnTo::RecentBooks;
    default:
      return ReturnTo::FileBrowser;
  }
}

// Asks for a query, then either narrows this folder or walks the whole card for it.
//
// One prompt rather than a live filter: on a folder large enough to be worth searching the model
// is reading names off the card to match them, which is the right price for pressing Search and
// the wrong one for every letter typed.
void FileBrowserActivity::startSearch(const bool everywhere) {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, everywhere ? tr(STR_SEARCH_ALL) : tr(STR_SEARCH),
                                              model.filter(), 64, InputType::Text),
      [this, everywhere](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        if (!everywhere) {
          applyFilter(kb.text);
          return;
        }
        // The walk reads the card and takes a moment on a full one, so say so
        // rather than looking frozen.
        {
          RenderLock lock(*this);
          GUI.drawPopup(renderer, tr(STR_SEARCHING));
          renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
          model.searchEverywhere(kb.text);
          resetNavigation(0);
        }
        requestUpdate();
      });
}

// Applies a query (or "" to clear) and puts the selection somewhere sensible: a search that
// matches nothing still shows the folder's own empty state rather than a stale row.
// Leaves the results and opens the folder the selected one actually lives in, with it selected.
// A card-wide search tells you where a book is; this is how you go and stand there.
void FileBrowserActivity::goToResultFolder() {
  if (!model.listsPaths() || nav.selected < 0) return;
  const std::string folder = model.resultFolder(static_cast<size_t>(nav.selected));
  const std::string full = model.entryFullPath(static_cast<size_t>(nav.selected));
  if (folder.empty()) return;
  const size_t slash = full.rfind('/');
  // Recent Books is not a folder browser: Browse Files opens the folder, the book selected in it.
  if (model.getMode() == Mode::Recents) {
    activityManager.goToFileBrowser(folder, slash == std::string::npos ? full : full.substr(slash + 1));
    return;
  }
  focusName = (slash == std::string::npos) ? full : full.substr(slash + 1);
  mappedInput.flushTouchEvents();
  {
    RenderLock lock(*this);
    model.setPath(folder);  // also ends the search
    model.load();
    const size_t idx = model.findEntry(focusName);
    focusName.clear();
    resetNavigation(idx < model.entryCount() ? static_cast<int>(idx) : 0);
  }
  requestUpdate();
}

void FileBrowserActivity::applyFilter(const std::string& query) {
  {
    RenderLock lock(*this);
    model.setFilter(query);
    resetNavigation(0);
  }
  requestUpdate();
}

void FileBrowserActivity::showBrowserOptionsMenu(const std::string& dirEntry) {
  // Resolved now rather than in the handler: the menu cannot change the selection while it is
  // open, but reading it back afterwards would be a dependency on that staying true.
  const bool isDir = !dirEntry.empty();
  std::string dirPath = model.path();
  if (isDir) {
    if (dirPath.back() != '/') dirPath += "/";
    dirPath += dirEntry.substr(0, dirEntry.length() - 1);
  }
  startActivityForResult(std::make_unique<FileContextMenuActivity>(
                             renderer, mappedInput, "", model.getSortMode(), model.getSortDirection(), isDir,
                             model.isFiltered() || model.isDeepSearch(), /*offerGoToFolder=*/false,
                             /*offerFileManagement=*/managesFiles(),
                             /*offerViewChoice=*/offersViewChoice(),
                             /*recentsList=*/model.getMode() == Mode::Recents),
                         [this, isDir, dirPath, dirEntry](const ActivityResult& res) {
                           if (res.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto* menuRes = std::get_if<MenuResult>(&res.data);
                           if (!menuRes) {
                             requestUpdate();
                             return;
                           }
                           const auto chosen = static_cast<FileContextMenuActivity::Action>(menuRes->action);
                           if (chosen == FileContextMenuActivity::Action::Open && isDir) {
                             activateSelected(false);
                             return;
                           }
                           if (chosen == FileContextMenuActivity::Action::Remove && isDir) {
                             doRemove(dirPath, dirEntry.substr(0, dirEntry.length() - 1), /*isDirectory=*/true);
                             return;
                           }
                           handleContextMenuAction(menuRes->action, "", "", menuRes);
                         });
}

void FileBrowserActivity::handleContextMenuAction(int action, const std::string& fullPath, const std::string& entry,
                                                  const MenuResult* menuRes) {
  using Action = FileContextMenuActivity::Action;
  const Action actionEnum = static_cast<Action>(action);

  if (actionEnum == Action::Search) {
    startSearch(/*everywhere=*/false);
    return;
  }
  if (actionEnum == Action::SearchAll) {
    startSearch(/*everywhere=*/true);
    return;
  }
  if (actionEnum == Action::ClearSearch) {
    {
      RenderLock lock(*this);
      model.clearSearch();
    }
    applyFilter("");
    return;
  }
  if (actionEnum == Action::GoToFolder) {
    goToResultFolder();
    return;
  }
  if (actionEnum == Action::NewFolder) {
    createFolderHere();
    return;
  }
  if (actionEnum == Action::MoveTo) {
    moveToFolder(fullPath, entry);
    return;
  }

  // Display options: apply sort + visibility state returned from the menu.
  if (actionEnum == Action::DisplayOptionsChanged) {
    if (!menuRes) {
      requestUpdate();
      return;
    }
    // Recent Books offers only its view and the extensions toggle: no sort (it is ordered by
    // recency) and no hidden files (it lists books, wherever they are).
    if (model.getMode() == Mode::Recents) {
      APP_STATE.recentBooksView = menuRes->browserView;
      APP_STATE.saveToFile();
      SETTINGS.showFileExtensions = menuRes->showFileExtensions;
      SETTINGS.saveToFile();
      bookRows.clear();
      resetNavigation(nav.selected);
      requestUpdate();
      return;
    }
    model.setSort(static_cast<CrossPointSettings::FILE_SORT_MODE>(menuRes->sortMode),
                  static_cast<CrossPointSettings::FILE_SORT_DIRECTION>(menuRes->sortDirection));

    // Hidden-files visibility changes the set of entries, so reload from disk.
    const bool hiddenChanged = (SETTINGS.showHiddenFiles != menuRes->showHiddenFiles);
    SETTINGS.showHiddenFiles = menuRes->showHiddenFiles;
    SETTINGS.showFileExtensions = menuRes->showFileExtensions;
    // Only Browse Files offers the view; the other browsers hand back what they were given.
    if (model.getMode() == Mode::Books) SETTINGS.fileBrowserView = menuRes->browserView;
    SETTINGS.saveToFile();
    // A row with no title of its own is titled by its filename, whose extension just may have
    // changed.
    bookRows.clear();

    // Re-apply ordering. The SD index is built for a specific sort mode, so when it's
    // active any sort change must re-open it (open() rebuilds on a mode mismatch);
    // loadFiles() does that. Hidden-files visibility changes the entry set, so it also
    // needs a full reload. Only the in-RAM small-folder case can re-sort in place.
    if (hiddenChanged || model.usesIndex()) {
      model.load();  // re-enumerate + (for the index) rebuild/reopen with the new mode
    } else {
      model.resort();
    }
    // Keep the selection in range after a reorder/reload.
    resetNavigation(nav.selected);
    requestUpdate();
    return;
  }

  // File-specific actions (require fullPath)
  switch (actionEnum) {
    case Action::Open: {
      ReturnHint hint;
      hint.target = returnTarget();
      hint.path = model.path();
      hint.selectName = entry;
      activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
      return;
    }
    case Action::FetchAndOpen: {
      if (KOREADER_STORE.hasCredentials() && FsHelpers::hasEpubExtension(fullPath)) {
        auto& sync = APP_STATE.koReaderSyncSession;
        sync.autoPullEpubPath = fullPath;
        sync.postAction = KOReaderSyncPostAction::Reader;
        APP_STATE.saveToFile();
      }
      ReturnHint hint;
      hint.target = returnTarget();
      hint.path = model.path();
      hint.selectName = entry;
      activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
      return;
    }
    case Action::MarkAsRead:
      doMarkAsRead(fullPath);
      return;
    case Action::Info:
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, fullPath),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    case Action::DeleteCache:
      doDeleteCache(fullPath, entry);
      return;
    case Action::SetAsSleepCover:
      doSetAsSleepCover(fullPath);
      return;
    case Action::Remove:
      doRemove(fullPath, entry, false);
      return;
    case Action::RemoveFromRecents:
      removeFromRecents(fullPath);
      return;
    default:
      requestUpdate();
      return;
  }
}

void FileBrowserActivity::doMarkAsRead(const std::string& fullPath) {
  std::string cachePath;
  uint8_t data[7] = {0};
  size_t dataLen = 0;

  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub epub(fullPath, "/.crosspoint");
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
    // 7-byte EPUB progress: spine(2) + page(2) + pageCount(2) + percent(1)
    data[6] = 100;
    dataLen = 7;
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc xtc(fullPath, "/.crosspoint");
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
    // 5-byte XTC progress: page(4) + percent(1)
    data[4] = 100;
    dataLen = 5;
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    Txt txt(fullPath, "/.crosspoint");
    txt.setupCacheDir();
    cachePath = txt.getCachePath();
    // 7-byte TXT progress: page(2) + offset(4) + percent(1)
    data[6] = 100;
    dataLen = 7;
  } else {
    return;
  }

  FsFile f;
  if (!Storage.openFileForWrite("FBR", cachePath + "/progress.bin", f)) {
    LOG_ERR("FBR", "Failed to write progress for mark-as-read: %s", fullPath.c_str());
    return;
  }
  f.write(data, dataLen);
  f.close();
  LOG_INF("FBR", "Marked as read: %s", fullPath.c_str());

  // Series/index/author unknown without loading — findNextBook falls back to alphabetical order.
  const std::string nextBookPath = BookFinished::findNextBookInDirectory(fullPath, {}, {});
  startActivityForResult(std::make_unique<FinishedBookActivity>(renderer, mappedInput, fullPath, nextBookPath),
                         [this, fullPath, nextBookPath](const ActivityResult& result) {
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto& menuResult = std::get<MenuResult>(result.data);
                           if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::GoHome)) {
                             if (SETTINGS.moveFinishedBooksToCompleted) {
                               std::string movedPath;
                               BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                             }
                             if (SETTINGS.removeFinishedBooksFromRecents) {
                               RECENT_BOOKS.removeBook(fullPath);
                             }
                             onGoHome();
                             return;
                           }
                           if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook) &&
                               !nextBookPath.empty()) {
                             if (SETTINGS.moveFinishedBooksToCompleted) {
                               std::string movedPath;
                               BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                             }
                             if (SETTINGS.removeFinishedBooksFromRecents) {
                               RECENT_BOOKS.removeBook(fullPath);
                             }
                             ReturnHint hint;
                             hint.target = returnTarget();
                             hint.path = model.path();
                             activityManager.replaceWithReader(nextBookPath, std::move(hint));
                             return;
                           }
                           // Stay — apply side effects then reload the list (file may have moved to /COMPLETED).
                           if (SETTINGS.moveFinishedBooksToCompleted) {
                             std::string movedPath;
                             BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                           }
                           if (SETTINGS.removeFinishedBooksFromRecents) {
                             RECENT_BOOKS.removeBook(fullPath);
                           }
                           bookRows.clear();  // its row now says Finished
                           model.load();
                           resetNavigation(nav.selected);
                           requestUpdate(true);
                         });
}

void FileBrowserActivity::doSetAsSleepCover(const std::string& fullPath) {
  if (FsHelpers::hasBmpExtension(fullPath)) {
    // BMP: use the shared helper that just does a file copy + settings update.
    const bool success = BmpViewerActivity::setBmpFileAsSleepScreen(fullPath);
    {
      RenderLock lock(*this);
      const char* msg = success ? tr(STR_SLEEP_SCREEN_SET) : tr(STR_FAILED_TO_SET_SLEEP_SCREEN);
      // drawPopup ships the frame; preseed its refresh mode to HALF instead of shipping twice.
      renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
      GUI.drawPopup(renderer, msg);
    }
    requestUpdate();
  } else {
    // JPG/PNG: must render to framebuffer — open the image viewer so the user can use its Set Sleep button.
    ReturnHint hint;
    hint.target = returnTarget();
    hint.path = model.path();
    hint.selectName = model.entryName(static_cast<size_t>(nav.selected));
    activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
  }
}

void FileBrowserActivity::doDeleteCache(const std::string& fullPath, const std::string& entry) {
  startActivityForResult(std::make_unique<ConfirmationActivity>(
                             renderer, mappedInput, tr(STR_DELETE_CACHE) + std::string("?"), utf8NfcNorm(entry)),
                         [this, fullPath](const ActivityResult& res) {
                           if (!res.isCancelled) {
                             clearFileMetadata(fullPath);
                             bookRows.clear();  // its progress went with the cache
                             LOG_INF("FBR", "Cache deleted for: %s", fullPath.c_str());
                           }
                           requestUpdate();
                         });
}

// As with a move: deleting a book from the book browser deletes the sidecars that browser never
// showed, and deleting a file from All files deletes that file alone.
void FileBrowserActivity::doRemove(const std::string& fullPath, const std::string& entry, bool isDirectory) {
  const bool withSidecars = !isDirectory && model.getMode() == Mode::Books;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                                tr(STR_DELETE) + std::string("? "), utf8NfcNorm(entry)),
                         [this, fullPath, isDirectory, withSidecars](const ActivityResult& res) {
                           if (!res.isCancelled) {
                             LOG_DBG("FBR", "Attempting to delete: %s", fullPath.c_str());
                             bool deleted;
                             if (isDirectory) {
                               deleted = removeDirRecursive(fullPath);
                             } else {
                               clearFileMetadata(fullPath);
                               deleted = Storage.remove(fullPath.c_str());
                               if (deleted && withSidecars && !SidecarFiles::removeAll(fullPath)) {
                                 LOG_ERR("FBR", "Deleted %s but not all of its sidecars", fullPath.c_str());
                               }
                             }
                             if (deleted) {
                               LOG_DBG("FBR", "Deleted successfully");
                               bookRows.clear();
                               model.load();
                               resetNavigation(nav.selected);
                               requestUpdate(true);
                             } else {
                               LOG_ERR("FBR", "Failed to delete: %s", fullPath.c_str());
                               requestUpdate();
                             }
                           } else {
                             requestUpdate();
                           }
                         });
}

int FileBrowserActivity::listCount() const {
  return static_cast<int>(std::min(model.entryCount(), static_cast<size_t>(std::numeric_limits<uint16_t>::max())));
}

void FileBrowserActivity::startLibraryBuildIfStale() {
  if (model.getMode() != Mode::Added && model.getMode() != Mode::Authors) return;
  if (libraryBuilder || !LibraryFreshness::stale(model.index())) return;
  LibraryBuilder::Config config;
  config.showHidden = SETTINGS.showHiddenFiles;
  config.isBook = &FileBrowserModel::isBookName;
  config.resolve = &resolveAuthor;
  libraryBuilder = makeUniqueNoThrow<LibraryBuilder>(std::move(config));
  libraryBuildGeneration = Storage.contentGeneration();
}

// One step of the index build, when there is one and nothing more urgent: true while it runs, so the
// covers wait. The builder resets the lent framebuffer at each step, so the cover loader -- its
// other user -- is stopped first; and the model's index is let go before a publish replaces it.
bool FileBrowserActivity::stepLibraryBuild() {
  if (!libraryBuilder) return false;
  if (renderer.isComposingFrame() || mappedInput.hasPendingInput()) return true;
  if (libraryBuilder->needsArena()) {
    coverLoader->reset();
    if (!lendForBackgroundWork()) return true;
  }
  if (libraryBuilder->nextStepPublishes()) {
    RenderLock lock(*this);
    model.releaseIndex();
  }
  HalPowerManager::Lock fullSpeed;
  const LibraryBuilder::Phase phase = libraryBuilder->step(coverScratch.get());
  const bool reload = libraryBuilder->takePublished() || phase == LibraryBuilder::Phase::Failed;
  if (reload) {
    RenderLock lock(*this);
    model.load();
    resetNavigation(std::min(nav.selected.load(), std::max(0, listCount() - 1)));
  }
  if (libraryBuilder->finished()) {
    if (phase == LibraryBuilder::Phase::Done) LibraryFreshness::built(libraryBuildGeneration);
    libraryBuilder.reset();
  }
  if (reload) requestUpdate();
  return true;
}
