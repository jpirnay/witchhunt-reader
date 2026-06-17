#pragma once

#include <string>

#include "../MenuListActivity.h"

// Context menu for file browser. Always shows display options (sort mode, sort
// direction, show hidden files, show extensions). When a regular file is
// selected, file-specific actions (Open, Info, Delete, ...) are appended below.
// For directories and unsupported types, only the display options are shown.
//
// Display options are cycled inline (DynamicEnum) and the resulting state is
// returned to FileBrowserActivity when the menu closes. File actions finish the
// activity immediately with the chosen Action.
class FileContextMenuActivity final : public MenuListActivity {
 public:
  enum class Action {
    None = -1,
    // Display options changed (state carried back in MenuResult)
    DisplayOptionsChanged = 0,
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

  explicit FileContextMenuActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& filePath = "",
      CrossPointSettings::FILE_SORT_MODE sortMode = CrossPointSettings::SORT_BY_NAME,
      CrossPointSettings::FILE_SORT_DIRECTION sortDirection = CrossPointSettings::SORT_ASCENDING);

  void render(RenderLock&&) override;

 private:
  std::string filePath;  // Empty string = browser options mode (no file selected)
  bool isBrowserMode;

  // Display option state, edited inline via DynamicEnum and returned on close.
  // Sort state is per-session (held by FileBrowserActivity); visibility toggles
  // mirror the persistent SETTINGS fields.
  uint8_t sortMode;
  uint8_t sortDirection;
  uint8_t showHiddenFiles;
  uint8_t showFileExtensions;

  void buildMenuItems();
  void onActionSelected(int index) override;
  void onBackPressed() override;
  void finishWithDisplayOptions(Action action);
};
