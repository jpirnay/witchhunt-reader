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
  static constexpr int GRID_COLS = 3;
  // Cell display size: how tall each thumbnail cell appears on screen.
  static constexpr int GRID_CELL_HEIGHT = 160;
  static constexpr int GRID_THUMB_MARGIN = 10;
  static constexpr int GRID_LABEL_HEIGHT = 36;  // two small-font lines below each thumbnail
  // Stored BMP dimensions — shared with FinishedBookActivity so one file serves both.
  // The grid scales this BMP down to the runtime cell width for display.
  static constexpr int GRID_THUMB_WIDTH = 220;
  static constexpr int GRID_THUMB_HEIGHT = 240;

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

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int focusIndex = -1)
      : Activity("RecentBooks", renderer, mappedInput), initialFocusIndex(focusIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
