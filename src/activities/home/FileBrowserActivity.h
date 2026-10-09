#pragma once

#include <BuildArena.h>
#include <FileIndex.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../UiListActivity.h"
#include "BookRowResolver.h"
#include "FileBrowserModel.h"
#include "RecentBooksStore.h"
#include "components/CoverGridLayout.h"
#include "components/themes/BaseTheme.h"

// CoverThumbLoader.h needs ReaderActivity.h, which includes this file: hence a pointer to it.
class CoverThumbLoader;
class LibraryBuilder;

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = the book browser opened from Home; AllFiles = every file, opened from Settings;
  // PickFirmware / PickFolder = pickers that return a path via ActivityResult. Owned by the model,
  // which needs it to filter; aliased here so callers keep naming it FileBrowserActivity::Mode.
  using Mode = FileBrowserModel::Mode;

 private:
  void clearFileMetadata(const std::string& fullPath);
  bool removeDirRecursive(const std::string& fullPath);
  void openContextMenu();
  void handleContextMenuAction(int action, const std::string& fullPath, const std::string& entry,
                               const struct MenuResult* menuRes = nullptr);
  void doMarkAsRead(const std::string& fullPath);
  void doSetAsSleepCover(const std::string& fullPath);
  void doDeleteCache(const std::string& fullPath, const std::string& entry);
  void doRemove(const std::string& fullPath, const std::string& entry, bool isDirectory);

  static constexpr size_t LIST_WINDOW_CAPACITY = 24;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowLabels;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowSubtitles;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowValues;
  std::array<freeink::ui::ListItem, LIST_WINDOW_CAPACITY> windowItems;
  uint16_t windowFirst = 0;
  uint16_t windowCount = 0;

  // What is in the folder and in what order. Everything about enumeration, the SD index and
  // sorting lives there; this class decides what a row means and where a tap goes next.
  FileBrowserModel model;

  std::string focusName;  // entry to select on first load (e.g. the file just returned from)

  // What a book row says instead of its filename, in the Details list and the Covers grid alike.
  BookRowResolver bookRows;

  uint8_t chosenView() const;
  bool detailsView() const;
  bool coversView() const;
  bool offersViewChoice() const;
  std::string rowName(const std::string& entry) const;

  // Covers view: the same rows as a grid of covers, laid out by CoverGridLayout -- one layout for
  // Browse Files and Recent Books, so a book has one grid thumbnail. Up/Down move a row and
  // Left/Right a cover (moveInCoverGrid), and the page on screen is the one holding the selection.
  struct CoverGrid {
    Rect content{};
    int top = 0;  // y of the first row, below the header
    CoverGridLayout::Layout cells{};
    int perPage = 1;
  };
  CoverGrid coverGrid() const;
  void afterUiRender() override;
  void drawCoverGrid();
  void drawCoverCell(int index, int x, int y, const CoverGridLayout::Layout& cells, bool selected);
  enum class CoverThumb : uint8_t { Drawn, NoCover, Missing };
  CoverThumb drawCoverThumb(const std::string& bookPath, int x, int y, int tw, int th, Rect& drawn);
  void drawTitleCard(const std::string& title, const Rect& card);
  void drawFolderCard(const Rect& card, int books);
  bool handleCoverTouch();

  // Making the covers the page on screen lacks. The render task flags a cell drawn without its
  // thumbnail; loop() then works through that page's books one at a time in short bursts, in the
  // borrowed secondary framebuffer as the Home carousel does, and redraws as each cover lands.
  std::unique_ptr<CoverThumbLoader> coverLoader;
  std::atomic<bool> coverWork{false};
  // Books whose cover failed during this visit: not retried until the browser is entered again.
  std::vector<std::string> coverFailed;
  uint8_t* lentRegion = nullptr;
  std::unique_ptr<BuildArena> coverScratch;
  void generateCovers();
  bool lendForBackgroundWork();
  void returnLentBuffer(bool callerHoldsRenderLock);

  // The book index build that New and Authors run when the index is stale (LibraryFreshness). It
  // gets the lent framebuffer after the title parses and before the covers.
  std::unique_ptr<LibraryBuilder> libraryBuilder;
  uint32_t libraryBuildGeneration = 0;
  void startLibraryBuildIfStale();
  bool stepLibraryBuild();

  [[nodiscard]] int listPageSize() const;
  [[nodiscard]] bool listPages() const;
  void pageSelection(int direction);
  bool moveInCoverGrid(MappedInputManager::Button button);
  void createFolderHere();
  void moveToFolder(const std::string& fullPath, const std::string& entry);
  bool confirmOpensOptions() const;
  bool managesFiles() const;
  ReturnTo returnTarget() const;
  void removeFromRecents(const std::string& bookPath);
  void showBrowserOptionsMenu(const std::string& dirEntry = {});
  void activateSelected(bool longPress);
  void resetNavigation(int selected = 0);
  // Search: prompt for a query, then narrow the folder to the names containing it.
  void startSearch(bool everywhere);
  void applyFilter(const std::string& query);
  void goToResultFolder();
  void materializeListWindow();

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               std::string focusName = {}, Mode mode = Mode::Books);
  ~FileBrowserActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  // The borrowed framebuffer goes back before any other screen opens on top of this one.
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  int indexForActionValue(int16_t value) const override { return static_cast<uint16_t>(value); }
  void drawChrome() override;
  void drawFooter() override;
  void navigateButtons() override;
};
