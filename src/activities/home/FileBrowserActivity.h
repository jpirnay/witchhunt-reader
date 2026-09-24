#pragma once

#include <FileIndex.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../UiListActivity.h"
#include "FileBrowserModel.h"
#include "RecentBooksStore.h"

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via
  // ActivityResult. Owned by the model, which needs it to filter; aliased here so callers keep
  // naming it FileBrowserActivity::Mode.
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
  void doFlashFirmware(const std::string& fullPath);

  static constexpr size_t LIST_WINDOW_CAPACITY = 24;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowLabels;
  std::array<freeink::ui::ListItem, LIST_WINDOW_CAPACITY> windowItems;
  uint16_t windowFirst = 0;
  uint16_t windowCount = 0;

  // What is in the folder and in what order. Everything about enumeration, the SD index and
  // sorting lives there; this class decides what a row means and where a tap goes next.
  FileBrowserModel model;

  std::string focusName;  // entry to select on first load (e.g. the file just returned from)

  [[nodiscard]] int listPageSize() const;
  [[nodiscard]] bool listPages() const;
  void pageSelection(int direction);
  void createFolderHere();
  void moveToFolder(const std::string& fullPath, const std::string& entry);
  bool confirmOpensOptions() const;
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
                               std::string focusName = {}, Mode mode = Mode::Books)
      : UiListActivity("FileBrowser", renderer, mappedInput), model(mode), focusName(std::move(focusName)) {
    model.setPath(std::move(initialPath));
  }
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  // Back and Confirm belong to handleCustomInput() alone, which classifies them through
  // ButtonEventManager because this screen means different things by a short and a long press
  // (Back: up one folder vs. leave the browser; Confirm: open vs. sync-then-open). The base
  // implementation acts on the raw press-down EDGE, which arrives a tick before the Short event
  // that the same physical press later produces -- so leaving it in place ran BOTH handlers for
  // one press: Confirm entered a folder on the press and then activated row 0 of the folder it
  // had just entered on the release, and Back finished the activity before the short-press
  // up-one-folder branch could ever be reached.
  bool handleButtons() override { return false; }
  int indexForActionValue(int16_t value) const override { return static_cast<uint16_t>(value); }
  void drawChrome() override;
  void drawFooter() override;
  void navigateButtons() override;
};
