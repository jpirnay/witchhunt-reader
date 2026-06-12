#pragma once

#include <Txt.h>

#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"

// Shared scaffolding for the line-oriented file readers (TXT, Markdown).
// Owns the document handle, page-offset index, viewport layout, reading
// progress persistence and status-bar freshness tracking, plus the common
// enter/exit lifecycle (orientation, recents, reading-stats session) and the
// loop() input handling (back/page buttons, tilt page turns, finished-book
// flow). Subclasses provide the format-specific parsing and page rendering,
// and hook in via onReaderEnter()/onReaderExit() for extras such as bookmarks
// or headings, onConfirmShortPress() for their navigation overlay, and
// onPageChanged() for per-page bookkeeping.
class LineReaderActivity : public Activity {
 protected:
  std::unique_ptr<Txt> txt;
  // Module tag used for storage/log calls so TXT and MD keep distinct logs.
  const char* logTag;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  ReaderUtils::InputDrainGuard inputDrainGuard;
  bool drainWasActive = false;

  // Streaming reader state: file offset for the start of each page.
  std::vector<size_t> pageOffsets;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for index-cache validation (font/margin/alignment changes
  // require re-indexing) and oriented layout for rendering.
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  // See EpubReaderActivity for rationale — suppresses no-op minute-tick re-renders.
  mutable int lastStatusBarPage = -1;
  mutable int lastStatusBarBattery = -1;
  mutable int lastStatusBarClockMinute = -1;

  LineReaderActivity(const char* name, const char* logTag, GfxRenderer& renderer, MappedInputManager& mappedInput,
                     std::unique_ptr<Txt> txt)
      : Activity(name, renderer, mappedInput), txt(std::move(txt)), logTag(logTag) {}

  // Called from onEnter() after the cache dir is set up, before recents and
  // the stats session are touched (TXT loads bookmarks here).
  virtual void onReaderEnter() {}
  // Called from onExit() after the stats session is flushed, before the
  // document is torn down (TXT syncs bookmarks, subclasses clear their lines).
  virtual void onReaderExit() {}
  // Called from loop() on a Confirm short press. Return true when handled
  // (TXT opens starred pages, MD opens the ToC); false lets the press fall
  // through unhandled.
  virtual bool onConfirmShortPress() { return false; }
  // Called after currentPage changed through the shared page-turn handling
  // (MD recomputes the current heading here).
  virtual void onPageChanged() {}

  // Page-turn helpers used by loop(); going forward past the last page
  // launches the finished-book flow.
  void goToPreviousPage();
  void goToNextPage();
  void launchFinishedBookFlow();

  // Reads the current font/margin/alignment settings and fills the cached
  // layout fields (oriented margins, viewportWidth, linesPerPage).
  void computeViewportLayout();

  // Records what the status bar showed so shouldSkipPeriodicUpdate() can
  // suppress no-op minute-tick re-renders. Call at the end of renderStatusBar().
  void noteStatusBarRendered() const;

  // progress.bin, 7-byte format: page(2 LE) + file offset(4 LE) + percent(1).
  // The offset lets drawCurrentPageToBuffer render without requiring index.bin.
  void saveProgress() const;
  void loadProgress();

 public:
  // Oriented text layout shared by the readers and their static sleep-overlay
  // renderers, which must reproduce the exact same wrapping geometry.
  struct TextLayout {
    int marginTop = 0;
    int marginRight = 0;
    int marginBottom = 0;
    int marginLeft = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    int linesPerPage = 1;
  };
  static TextLayout computeTextLayout(const GfxRenderer& renderer, int fontId, uint8_t screenMargin);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool isReaderActivity() const override { return true; }
  bool shouldSkipPeriodicUpdate() const override;
};
