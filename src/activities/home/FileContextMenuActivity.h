#pragma once

#include <string>

#include "../MenuListActivity.h"

// Context menu for file browser: displays display options (sort, hidden files, extensions)
// and file-specific actions (Open, Info, Delete, etc.) based on file type.
// When filePath is empty, acts as a browser options menu (display options only).
class FileContextMenuActivity final : public MenuListActivity {
 public:
  enum class Action {
    None = -1,
    // Display options (shared)
    ChangeSortMode = 0,
    ChangeSortDirection,
    ToggleHiddenFiles,
    ToggleExtensions,
    // File-specific actions
    Open = 10,
    FetchAndOpen,
    MarkAsRead,
    Info,
    DeleteCache,
    SetAsSleepCover,
    FlashFirmware,
    Remove,
  };

  explicit FileContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::string& filePath = "");

  void render(RenderLock&&) override;

 private:
  std::string filePath;  // Empty string = browser options mode
  bool isBrowserMode;

  // Sort state (for DynamicEnum lambdas)
  CrossPointSettings::FILE_SORT_MODE sortMode = CrossPointSettings::SORT_BY_NAME;
  CrossPointSettings::FILE_SORT_DIRECTION sortDirection = CrossPointSettings::SORT_ASCENDING;

  void onActionSelected(int index) override;
  void onBackPressed() override;
  void onSettingToggled(int index) override;
};
