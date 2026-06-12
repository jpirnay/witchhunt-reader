#pragma once

#include <Txt.h>

#include <vector>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "LineReaderActivity.h"

class TxtReaderActivity final : public LineReaderActivity {
  // Bookmarks (starred pages)
  BookmarkStore bookmarkStore;

  std::vector<std::string> currentPageLines;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  // Consume a persisted bookmark-jump request (from GlobalBookmarksActivity) for
  // this TXT file. Rewrites progress.bin before initializeReader() reads it.
  void applyPendingBookmarkJump();

 protected:
  void onReaderEnter() override;
  void onReaderExit() override;
  // Opens the starred-pages overlay when bookmarks exist.
  bool onConfirmShortPress() override;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : LineReaderActivity("TxtReader", "TRS", renderer, mappedInput, std::move(txt)) {}
  void render(RenderLock&&) override;
  void onButtonAction(CrossPointSettings::BUTTON_ACTION action) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
};
