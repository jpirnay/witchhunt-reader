#pragma once
#include <PngToBmpConverter.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "../Activity.h"
#include "./FileBrowserActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
 public:
  enum class MenuAction {
    FileBrowser,
    Recents,
    GlobalBookmarks,
    OpdsBrowser,
    FileTransfer,
    Weather,
    Settings,
  };

 private:
  struct MenuEntry {
    MenuAction action;
    StrId label;
    UIIcon icon;
  };

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int lastCarouselBookIndex = 0;  // remembered position when leaving carousel row
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;
  bool coverBufferStored = false;
  size_t nextRecentCoverIndex = 0;
  size_t nextThumbSizeIndex = 0;  // which thumb size within the current book is next

  // Phase 1: sliced ZIP extraction of cover.img (only needed for large embedded PNG covers)
  std::unique_ptr<ReaderActivity::CoverExtractSession> extractSession;

  // Phase 2: sliced PNG decode session (non-null while a PNG cover is being decoded row-by-row)
  std::unique_ptr<PngDecodeSession> pngSession;
  ReaderActivity::PngThumbFiles pngSessionFiles;  // open FsFiles borrowed by pngSession
  bool pngSessionFailed = false;                  // set on error; triggers empty-path store same as sync failure

  uint8_t* coverBuffer = nullptr;
  size_t coverBufferSize = 0;
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;

  std::vector<RecentBook> recentBooks;
  std::vector<MenuEntry> menuEntries;
  bool menuEntriesDirty = true;

  std::string focusBookPath;
  int focusSelectorIndex = -1;

  void onSelectBook(const std::string& path);
  void dispatchMenuAction(MenuAction action);

  void rebuildMenuEntries();
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string focusBookPath = {},
                        int focusSelectorIndex = -1)
      : Activity("Home", renderer, mappedInput),
        focusBookPath(std::move(focusBookPath)),
        focusSelectorIndex(focusSelectorIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return recentsLoading || extractSession != nullptr || pngSession != nullptr; }
};
