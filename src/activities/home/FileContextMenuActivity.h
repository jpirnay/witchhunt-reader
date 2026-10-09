#pragma once

#include <string>

#include "../MenuListActivity.h"

// Context menu for file browser. Always shows display options (sort mode, sort
// direction, show hidden files, show extensions). When a regular file is
// selected, file-specific actions (Open, Info, Delete, ...) are appended below.
// A file the reader cannot open, which only All files lists, gets Remove and
// Move to folder.
//
// Display options are cycled inline (DynamicEnum) and the resulting state is
// returned to FileBrowserActivity when the menu closes. File actions finish the
// activity immediately with the chosen Action.
//
// Opened from the firmware picker (offerFileManagement == false) the menu keeps only what helps
// pick an image: sorting, searching, and removing an old .bin. Organising the card -- new folders,
// moving files, deleting folders -- is the library browser's business, not the updater's.
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
    Remove,
    MoveTo,
    NewFolder,
    Search,
    SearchAll,
    ClearSearch,
    GoToFolder,
    RemoveFromRecents,
  };

  // The list the menu was opened on. Recents and Index (the Library's New and Authors) have a fixed
  // order and list books from all over the card: no sort, no hidden-files toggle, no search. Only
  // Recents' Remove takes a book off its list rather than off the card.
  enum class ListSource : uint8_t { Folder, Recents, Index };

  explicit FileContextMenuActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& filePath = "",
      CrossPointSettings::FILE_SORT_MODE sortMode = CrossPointSettings::SORT_BY_NAME,
      CrossPointSettings::FILE_SORT_DIRECTION sortDirection = CrossPointSettings::SORT_ASCENDING,
      bool offerDirectoryActions = false, bool searchActive = false, bool offerGoToFolder = false,
      bool offerFileManagement = true, bool offerViewChoice = false, ListSource source = ListSource::Folder,
      uint8_t view = CrossPointSettings::BROWSER_VIEW_FILES);

  void render(RenderLock&&) override;

 private:
  std::string filePath;  // Empty string = browser options mode (no file selected)
  bool isBrowserMode;
  // Browser mode with a DIRECTORY selected: it gets Open and Remove of its own, which the
  // display options alone cannot express.
  bool offerDirectoryActions;
  // Whether a search is currently narrowing the folder, so the menu can offer to clear it.
  bool searchActive;
  // A row from a card-wide search: offer to go to the folder it actually lives in.
  bool offerGoToFolder;
  // New folder, Move to folder and Remove on a directory. Off in the firmware picker.
  bool offerFileManagement;
  // Filenames / Details / Covers. Browse Files and Recent Books: the other browsers list files, not
  // books.
  bool offerViewChoice;
  // Recent Books is ordered by when each book was read, New and Authors by the book index: no sort.
  // They list books from all over the card: no hidden-files toggle and no search. On Recent Books,
  // Remove takes a book off the list rather than off the card -- what the screen's long Left press
  // did before it was a list.
  ListSource source;

  // Display option state, edited inline via DynamicEnum and returned on close.
  // Sort state is per-session (held by FileBrowserActivity); visibility toggles
  // mirror the persistent SETTINGS fields.
  uint8_t sortMode;
  uint8_t sortDirection;
  uint8_t showHiddenFiles;
  uint8_t showFileExtensions;
  uint8_t browserView;

  void buildMenuItems();
  void onActionSelected(int index) override;
  void onBackPressed() override;
  // Long Back commits the display options as Back does; the browser persists them only from this
  // result.
  void homeFromList() override { onBackPressed(); }
  void finishWithDisplayOptions(Action action);
};
