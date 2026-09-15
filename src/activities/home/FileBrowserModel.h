#pragma once

#include <FileIndex.h>

#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

// The contents of one directory as the file browser sees them: enumerated, filtered to the
// types the browser can open, and ordered.
//
// Two interchangeable backends behind one interface. A small folder keeps its names and
// metadata in RAM and is sorted here; a folder of INDEX_THRESHOLD entries or more builds an
// SD-backed FileIndex instead, so RAM stays bounded whatever the card holds. Both present the
// same entry-name form -- a trailing '/' marks a directory -- so nothing above this class has
// to know which one is live.
//
// Deliberately free of rendering, input and activity transitions: it answers "what is in this
// directory, in what order", and leaves what a row MEANS to the activity. That is also why the
// path is set rather than walked -- computing the parent or a child path is navigation, and
// navigation stays with the screen.
class FileBrowserModel {
 public:
  // Books = the file types the reader can open; PickFirmware = .bin only.
  enum class Mode { Books, PickFirmware };

  explicit FileBrowserModel(const Mode mode = Mode::Books) : mode(mode) {}

  [[nodiscard]] Mode getMode() const { return mode; }

  // The directory currently enumerated. setPath() only records it; call load() to re-read.
  [[nodiscard]] const std::string& path() const { return basepath; }
  void setPath(std::string newPath) { basepath = newPath.empty() ? "/" : std::move(newPath); }

  // Re-read the directory: filter, then either build the SD index or sort in RAM.
  void load();
  // Release both backends. Called on the way out so a browser sitting on the activity stack
  // is not holding a folder's worth of names, or an open index file.
  void clear();

  // Rows presented, in display order.
  [[nodiscard]] size_t entryCount() const;
  // The row's name in the canonical form: a trailing '/' marks a directory. Out-of-range or an
  // index read failure gives "". Non-const: the SD-index backend streams from the open file.
  std::string entryName(size_t displayIndex);
  // Display index of `name` (canonical form), or entryCount() when it is not present.
  size_t findEntry(const std::string& name);

  [[nodiscard]] CrossPointSettings::FILE_SORT_MODE getSortMode() const { return sortMode; }
  [[nodiscard]] CrossPointSettings::FILE_SORT_DIRECTION getSortDirection() const { return sortDirection; }
  void setSort(const CrossPointSettings::FILE_SORT_MODE mode, const CrossPointSettings::FILE_SORT_DIRECTION direction) {
    sortMode = mode;
    sortDirection = direction;
  }
  // Re-order the rows already in RAM without re-reading the directory. Does nothing while the
  // SD index is live: that one bakes its order in at build time, so a sort change there needs
  // load(). usesIndex() is how the caller tells the two apart.
  void resort();
  [[nodiscard]] bool usesIndex() const { return fileIndex != nullptr; }

 private:
  // At or above this many entries, the folder moves to the SD-backed index.
  static constexpr size_t INDEX_THRESHOLD = 64;

  // What the browser lists, in the mode it was opened in. Both the in-RAM enumeration and the
  // index build/staleness scan go through these, so the two backends cannot disagree about
  // what the folder contains.
  //
  // One function per mode rather than one taking a Mode, because FileIndex::AcceptFn is a bare
  // function pointer with no user data: indexFilter() hands over the one that matches.
  static bool acceptForBooks(const char* name, bool isDir);
  static bool acceptForFirmware(const char* name, bool isDir);
  [[nodiscard]] bool acceptEntry(const char* name, bool isDir) const;
  [[nodiscard]] FileIndex::AcceptFn indexFilter() const;

  void openIndexIfLarge();
  bool indexEntryAt(size_t displayIndex, FileIndex::Entry& out);

  Mode mode = Mode::Books;
  std::string basepath = "/";

  // In-RAM backend: names plus the metadata the date/size sorts need, index-aligned.
  std::vector<std::string> files;
  std::vector<uint32_t> fileSizes;      // cached file sizes (0 for directories)
  std::vector<uint32_t> fileDateTimes;  // cached FAT date/time pairs

  // SD backend: null for small folders, active for INDEX_THRESHOLD+ entries.
  std::unique_ptr<FileIndex> fileIndex;

  // Per-session, not persisted. Visibility toggles (showHiddenFiles / showFileExtensions)
  // live in SETTINGS.
  CrossPointSettings::FILE_SORT_MODE sortMode = CrossPointSettings::SORT_BY_NAME;
  CrossPointSettings::FILE_SORT_DIRECTION sortDirection = CrossPointSettings::SORT_ASCENDING;
};
