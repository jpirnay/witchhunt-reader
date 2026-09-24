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
  // Books = the file types the reader can open; PickFirmware = .bin only;
  // PickFolder = directories only, for choosing a destination to move a file into.
  enum class Mode { Books, PickFirmware, PickFolder };

  explicit FileBrowserModel(const Mode mode = Mode::Books) : mode(mode) {}

  [[nodiscard]] Mode getMode() const { return mode; }

  // The directory currently enumerated. setPath() only records it; call load() to re-read.
  [[nodiscard]] const std::string& path() const { return basepath; }
  // Changing directory ends any search: a filter is about the folder it was run in, and
  // carrying it into the next one silently hides most of what is there.
  void setPath(std::string newPath) {
    basepath = newPath.empty() ? "/" : std::move(newPath);
    filterQuery.clear();
    matches.clear();
    clearDeepSearch();
  }

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

  // Narrow the rows to those whose name contains `query`, case-insensitively. "" clears it.
  //
  // A filter belongs here rather than in the screen because this class already answers "what is
  // in this directory, in what order", and because doing it here means both backends get it: the
  // match list is built through entryName(), so the SD-indexed folders that most need searching
  // are exactly the ones it works on.
  //
  // Applied once, on demand, not per keystroke. On the indexed backend building the list reads
  // every name from the index file, which is fine as the cost of pressing Search and much too
  // much to pay on each letter typed.
  void setFilter(std::string query);
  [[nodiscard]] const std::string& filter() const { return filterQuery; }
  [[nodiscard]] bool isFiltered() const { return !filterQuery.empty(); }
  // Rows the folder holds regardless of the filter. For telling someone their search matched
  // nothing in a folder that is not itself empty.
  [[nodiscard]] size_t unfilteredEntryCount() const;

  // Search every folder under the current path, not just this one.
  //
  // Walks the tree once, on demand, and keeps the paths that match. No index: FAT gives no
  // change notification, so an index would have to re-walk the tree to know whether it was
  // still true -- which is the walk it was meant to save. Walking when asked is always correct
  // and costs nothing when nobody asks.
  //
  // Results are paths relative to the search root, so two books of the same name in different
  // folders can be told apart. Capped: a reader wants to find one book, not enumerate the card.
  void searchEverywhere(const std::string& query);
  [[nodiscard]] bool isDeepSearch() const { return deepSearch; }
  // True when the walk stopped at MAX_DEEP_RESULTS with more still out there.
  [[nodiscard]] bool deepResultsTruncated() const { return deepTruncated; }
  // Absolute path for a row, whichever mode is live. The caller no longer composes it.
  [[nodiscard]] std::string entryFullPath(size_t displayIndex);
  // Ends a card-wide search and returns the browser to the folder it was started from.
  void clearSearch() { clearDeepSearch(); }
  // Folder holding a result row, relative to the search root ("" when it sat at the root).
  [[nodiscard]] std::string resultFolder(size_t displayIndex);

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
  // Ceiling on a single search's match list, so a one-letter query in a huge folder cannot
  // turn into an unbounded allocation. Four bytes each, so this is 4 KB at worst.
  static constexpr size_t MAX_MATCHES = 1024;

  // Backend row indices that match the filter, in display order. Empty and unused when no
  // filter is set, so an unfiltered browser costs nothing.
  std::string filterQuery;
  std::vector<uint32_t> matches;

  // Card-wide search results: paths relative to the search root. Held only while one is on
  // screen; cleared with everything else in clear() and on any directory change.
  static constexpr size_t MAX_DEEP_RESULTS = 64;
  bool deepSearch = false;
  bool deepTruncated = false;
  std::string deepRoot;
  std::vector<std::string> deepResults;
  void rebuildMatches();
  void clearDeepSearch() {
    deepSearch = false;
    deepTruncated = false;
    deepResults.clear();
    deepResults.shrink_to_fit();
    deepRoot.clear();
  }
  // entryName() without the filter indirection: what the live backend holds at that row.
  std::string backendEntryName(size_t backendIndex);

  // What the browser lists, in the mode it was opened in. Both the in-RAM enumeration and the
  // index build/staleness scan go through these, so the two backends cannot disagree about
  // what the folder contains.
  //
  // One function per mode rather than one taking a Mode, because FileIndex::AcceptFn is a bare
  // function pointer with no user data: indexFilter() hands over the one that matches.
  static bool acceptForBooks(const char* name, bool isDir);
  static bool acceptForFirmware(const char* name, bool isDir);
  static bool acceptForFolders(const char* name, bool isDir);
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
