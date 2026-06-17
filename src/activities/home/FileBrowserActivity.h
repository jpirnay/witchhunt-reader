#pragma once

#include <FileIndex.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

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

  ButtonNavigator buttonNavigator;

  int selectorIndex = 0;

  Mode mode = Mode::Books;

  // Files state (small folders use in-RAM vector; large folders use SD index)
  std::string basepath = "/";
  std::string focusName;  // entry to select on first load (e.g. the file just returned from)
  std::vector<std::string> files;
  std::vector<uint32_t> fileSizes;       // cached file sizes (0 for directories)
  std::vector<uint32_t> fileDateTimes;   // cached FAT date/time pairs
  std::unique_ptr<FileIndex> fileIndex;  // null for small folders, active for 64+ entries

  // Threshold: use FileIndex for folders with 64+ entries (bounded RAM always)
  static constexpr size_t FILE_INDEX_THRESHOLD = 64;

  // Sorting state (per-session, not persisted). Visibility toggles
  // (showHiddenFiles / showFileExtensions) live in SETTINGS.
  CrossPointSettings::FILE_SORT_MODE sortMode = CrossPointSettings::SORT_BY_NAME;
  CrossPointSettings::FILE_SORT_DIRECTION sortDirection = CrossPointSettings::SORT_ASCENDING;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;
  void sortFileList();
  uint32_t getFileDateTime(const std::string& filePath) const;
  uint32_t getFileSize(const std::string& filePath) const;
  std::string getFileExtension(const std::string& name) const;
  void showBrowserOptionsMenu();

  // FileIndex backend: filter for index scanning/building
  static bool acceptFileForBrowser(const char* name, bool isDir);
  void tryOpenFileIndex();
  bool useFileIndexForEntry(size_t displayIndex, FileIndex::Entry& out);
  size_t getDisplayEntryCount() const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               std::string focusName = {}, Mode mode = Mode::Books)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        focusName(std::move(focusName)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
