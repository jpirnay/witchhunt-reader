#pragma once
#include <I18n.h>
#include <PngToBmpConverter.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"

class RecentBooksActivity final : public Activity {
 public:
  // Stored BMP dimensions — shared with FinishedBookActivity so one file serves both.
  // The grid scales this BMP down to the runtime cell size for display (never up).
  static constexpr int GRID_THUMB_WIDTH = 220;
  static constexpr int GRID_THUMB_HEIGHT = 240;
  // Largest cover box the grid will draw: the stored BMP height plus the 1 px frame on each side.
  // A height-bound cover (the usual ~2:3 shape fills the slot's height, not its width) then draws
  // 1:1 instead of being resampled — rescaling a dithered 1-bit image aliases its dither into a
  // visible grid. Raising it means raising GRID_THUMB_HEIGHT, which is part of a cache filename
  // (every cover regenerates) and is bounded by what the label block leaves free.
  //
  // Columns and rows are NOT constants: CoverGridLayout derives both from the panel and the theme
  // metrics, so a higher-resolution device gets more cells instead of a handful of oversized ones.
  // On the 480 px-wide X4/X3 panels that works out to 2x2 — at the previous hard-coded 3 columns a
  // cell was ~136 px and covers drew at roughly 105x158, too small to recognise the artwork.
  static constexpr int GRID_MAX_CELL_HEIGHT = GRID_THUMB_HEIGHT + 2;

 private:
  int selectorIndex = 0;
  int initialFocusIndex = -1;  // applied once in onEnter(), then cleared

  std::vector<RecentBook> recentBooks;
  // Reading-progress percent per recent book (parallel to recentBooks), cached so
  // the grid badge doesn't re-read progress.bin from SD on every cell repaint.
  // -1 = not started / unknown. Refreshed whenever recentBooks is (re)loaded.
  std::vector<int8_t> bookProgress;

  // Lazy cover loading state for grid view
  bool coversLoaded = false;
  bool coversLoading = false;
  bool firstRenderDone = false;
  size_t nextCoverIndex = 0;

  // Phase 1: sliced ZIP extraction of cover.img for large embedded PNG covers
  std::unique_ptr<ReaderActivity::CoverExtractSession> extractSession;

  // Phase 2: sliced PNG decode session (non-null while a PNG cover is being decoded row-by-row)
  std::unique_ptr<PngDecodeSession> pngSession;
  ReaderActivity::PngThumbFiles pngSessionFiles;
  bool pngSessionFailed = false;
  // Throttle for the cover-decode progress log (millis() of the last line emitted).
  uint32_t lastCoverProgressLogMs_ = 0;

  // Partial selection repaint: track previous index so we only redraw two cells
  int prevSelectorIndex = -1;
  bool fullRedrawNeeded = true;

  // Set once Confirm commits to opening a book. Suppresses any further grid
  // selection repaint so the highlight can't visibly jump during the
  // transition into the reader.
  bool openingBook = false;

  void loadRecentBooks();
  // Generates the next missing grid thumbnail (one per call). Returns true when all done.
  bool loadNextCover();

  void switchViewMode(bool grid);
  void removeSelectedBook();
  void showSelectedBookInfo();

  // Draws a single grid cell (used for both full render and partial selection update).
  void renderGridCell(int index, bool selected, int cellX, int cellY, int tw, int th, int labelW);

  void renderListView(RenderLock&&);
  void renderGridView(RenderLock&&);
  // Columns currently on screen — derived from the panel size and theme metrics, not a constant.
  int gridColumns() const;
  // Open the book under the selection. Shared by Confirm and by a tap, so the two agree.
  void openSelectedBook(bool longPress);
  // Tap on a cover (grid) or a row (list): Down moves the selection, Tap opens. Returns true
  // when the touch was consumed. Inert on non-touch boards.
  bool handleBookTouch();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int focusIndex = -1)
      : Activity("RecentBooks", renderer, mappedInput), initialFocusIndex(focusIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
