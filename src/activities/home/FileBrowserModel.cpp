#include "FileBrowserModel.h"

#include <CooperativeAbort.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "BookDetails.h"
#include "FolderCountMemo.h"
#include "RecentBooksStore.h"

namespace {

// The part of the filter that does not depend on what the browser is picking: a hidden entry and
// the FAT volume-information folder are never listed, whatever the mode.
bool isListableName(const char* name) {
  if (!SETTINGS.showHiddenFiles && name[0] == '.') return false;
  return strcmp(name, "System Volume Information") != 0;
}

// The books the reader can open. Images are not books: the viewer opens them, but in the book
// browser they are mostly covers saved beside the book they belong to, listed a second time.
bool isReadableBook(const std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename);
}

bool isViewableImage(const std::string_view filename) {
  return FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
         FsHelpers::hasPngExtension(filename);
}

std::string fileExtension(const std::string& name) {
  const char* dot = strrchr(name.c_str(), '.');
  if (!dot || dot == name.c_str() || name.back() == '/') {
    return "";  // directory, no extension, or dot-file
  }
  return std::string(dot + 1);
}

}  // namespace

void FileBrowserModel::load() {
  files.clear();
  fileSizes.clear();
  fileDateTimes.clear();
  if (mode == Mode::Recents) {
    loadRecents();
    return;
  }
  if (mode == Mode::Added) {
    loadAdded();
    return;
  }
  if (mode == Mode::Authors) {
    loadAuthors();
    return;
  }

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    if (!acceptEntry(name, isDir)) {
      file.close();
      continue;
    }

    if (isDir) {
      files.emplace_back(std::string(name) + "/");
      fileSizes.push_back(0);      // directories have size 0
      fileDateTimes.push_back(0);  // will use default date
    } else {
      files.emplace_back(name);
      fileSizes.push_back(static_cast<uint32_t>(file.fileSize()));
      uint16_t fdate = 0, ftime = 0;
      file.getModifyDateTime(&fdate, &ftime);
      uint32_t combined = (static_cast<uint32_t>(fdate) << 16) | ftime;
      fileDateTimes.push_back(combined);
    }
    file.close();
  }
  root.close();

  // Try to use FileIndex for large folders (64+ entries); fall back to in-RAM sort
  openIndexIfLarge();

  // Only sort in-RAM if FileIndex is not in use
  if (!fileIndex) {
    resort();
  }
}

bool FileBrowserModel::acceptForBooks(const char* name, const bool isDir) {
  if (!isListableName(name)) return false;
  if (isDir) return true;  // every folder is worth descending into
  return isReadableBook(std::string_view{name});
}

bool FileBrowserModel::isOpenable(const std::string_view filename) {
  return isReadableBook(filename) || isViewableImage(filename);
}

namespace {

// Folder counts already worked out this session (see FolderCountMemo). One table for every
// browser, so leaving Browse Files and coming back keeps them; 512 bytes, whatever the library.
// The render task reads it (a folder card being drawn) while the loop task's walk writes it.
FolderCountMemo folderCounts;
std::mutex folderCountsMutex;

int rememberedCount(const std::string& folder, const uint32_t stamp) {
  std::lock_guard<std::mutex> guard(folderCountsMutex);
  return folderCounts.find(folder, stamp);
}

void rememberCount(const std::string& folder, const int books, const uint32_t stamp) {
  std::lock_guard<std::mutex> guard(folderCountsMutex);
  folderCounts.store(folder, books, stamp);
}

// What a count depends on besides the folder: what is on the card, and whether hidden entries
// are counted (isListableName).
uint32_t countStamp() { return (Storage.contentGeneration() << 1) | (SETTINGS.showHiddenFiles ? 1u : 0u); }

}  // namespace

int FileBrowserModel::knownBooksBelow(const std::string& dirPath) { return rememberedCount(dirPath, countStamp()); }

// Depth first, one open directory per level -- at most MAX_DEPTH + 1 of them, however wide the
// tree -- so a folder's total is complete when its directory runs out, and is recorded then.
// A folder already in the table is added without being entered, so a count interrupted by a
// button press resumes past everything it finished, and counting a parent after a child (or the
// other way round) walks each folder once.
int FileBrowserModel::countBooksBelow(const std::string& dirPath) {
  constexpr int MAX_DEPTH = 8;  // as deep as anyone files books; a cycle-proof bound besides
  const uint32_t stamp = countStamp();
  if (const int known = rememberedCount(dirPath, stamp); known >= 0) return known;

  struct Level {
    HalFile dir;
    std::string path;
    int books = 0;
    bool complete = true;  // nothing below was cut off by MAX_DEPTH
  };
  std::vector<Level> levels;
  levels.reserve(MAX_DEPTH + 1);  // references into it stay valid while it grows
  {
    auto root = Storage.open(dirPath.c_str());
    if (!root || !root.isDirectory()) return 0;
    root.rewindDirectory();
    levels.push_back(Level{std::move(root), dirPath});
  }
  int counted = 0;  // every book found so far, for the cap
  char name[500];
  while (true) {
    if (CooperativeAbort::shouldAbortLongTask()) return -1;  // the levels close their directories
    if (counted > MAX_COUNTED_BOOKS) {
      // More than the card can say. The folder asked about is recorded as such; the ones still
      // open below it are not finished, so they are not recorded at all.
      rememberCount(dirPath, MAX_COUNTED_BOOKS + 1, stamp);
      return MAX_COUNTED_BOOKS + 1;
    }
    Level& level = levels.back();
    auto entry = level.dir.openNextFile();
    if (!entry) {
      // This folder is done. Its total is recorded when nothing in it was cut off by depth, and
      // always for the folder asked about -- that is the number on its card.
      level.dir.close();
      const int books = level.books;
      const bool complete = level.complete;
      if (complete || levels.size() == 1) rememberCount(level.path, books, stamp);
      levels.pop_back();
      if (levels.empty()) return std::min(books, MAX_COUNTED_BOOKS + 1);
      levels.back().books += books;
      levels.back().complete = levels.back().complete && complete;
      HalSystem::feedWatchdog();
      continue;
    }
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if (!isListableName(name)) continue;
    if (!isDir) {
      if (isReadableBook(std::string_view{name})) {
        ++level.books;
        ++counted;
      }
      continue;
    }
    std::string child = level.path + "/" + name;
    if (const int known = rememberedCount(child, stamp); known >= 0) {
      level.books += known;
      counted += known;
      continue;
    }
    if (levels.size() > MAX_DEPTH) {
      level.complete = false;  // deeper than we look: this total is a lower bound
      continue;
    }
    auto sub = Storage.open(child.c_str());
    if (!sub || !sub.isDirectory()) continue;
    sub.rewindDirectory();
    levels.push_back(Level{std::move(sub), std::move(child)});
  }
}

bool FileBrowserModel::acceptForAllFiles(const char* name, const bool /*isDir*/) { return isListableName(name); }

bool FileBrowserModel::acceptForFirmware(const char* name, const bool isDir) {
  if (!isListableName(name)) return false;
  if (isDir) return true;
  return FsHelpers::checkFileExtension(std::string_view{name}, ".bin");
}

bool FileBrowserModel::acceptForFolders(const char* name, const bool isDir) {
  // Only somewhere a file can be put. Listing the files too would be scenery to scroll past.
  return isListableName(name) && isDir;
}

bool FileBrowserModel::acceptEntry(const char* name, const bool isDir) const {
  switch (mode) {
    case Mode::PickFirmware:
      return acceptForFirmware(name, isDir);
    case Mode::PickFolder:
      return acceptForFolders(name, isDir);
    case Mode::AllFiles:
      return acceptForAllFiles(name, isDir);
    case Mode::Books:
    case Mode::Recents:
      break;
  }
  return acceptForBooks(name, isDir);
}

FileIndex::AcceptFn FileBrowserModel::indexFilter() const {
  switch (mode) {
    case Mode::PickFirmware:
      return &acceptForFirmware;
    case Mode::PickFolder:
      return &acceptForFolders;
    case Mode::AllFiles:
      return &acceptForAllFiles;
    case Mode::Books:
    case Mode::Recents:
      break;
  }
  return &acceptForBooks;
}

void FileBrowserModel::openIndexIfLarge() {
  if (files.size() < INDEX_THRESHOLD) {
    fileIndex = nullptr;
    return;
  }

  fileIndex = std::make_unique<FileIndex>();
  const FileIndex::SortMode indexSortMode = static_cast<FileIndex::SortMode>(sortMode);
  if (!fileIndex->open(basepath.c_str(), indexSortMode, indexFilter())) {
    LOG_ERR("FBR", "FileIndex build failed for %s, falling back to in-RAM sort", basepath.c_str());
    fileIndex = nullptr;
  }
  rebuildMatches();  // the rows were just renumbered
}

size_t FileBrowserModel::unfilteredEntryCount() const { return fileIndex ? fileIndex->totalCount() : files.size(); }

size_t FileBrowserModel::entryCount() const {
  if (mode == Mode::Authors) return atAuthorList() ? bookIndex.header().authorCount : authorBooks.size();
  if (listsPaths()) return deepResults.size();
  return isFiltered() ? matches.size() : unfilteredEntryCount();
}

bool FileBrowserModel::indexEntryAt(const size_t displayIndex, FileIndex::Entry& out) {
  if (!fileIndex) return false;
  const bool desc = (sortDirection == CrossPointSettings::SORT_DESCENDING);
  return fileIndex->entryAt(displayIndex, desc, out);
}

// Returns the row's name in the canonical browser form: a trailing '/' marks a
// directory. For the in-RAM backend `files` already stores this form; for the SD
// index we reconstruct it from the Entry. Out-of-range / index-read failure → "".
std::string FileBrowserModel::entryName(const size_t displayIndex) {
  if (mode == Mode::Authors) return atAuthorList() ? authorRowName(displayIndex) : authorBookName(displayIndex);
  if (listsPaths()) {
    return displayIndex < deepResults.size() ? deepResults[displayIndex] : "";
  }
  if (isFiltered()) {
    if (displayIndex >= matches.size()) return "";
    return backendEntryName(matches[displayIndex]);
  }
  return backendEntryName(displayIndex);
}

std::string FileBrowserModel::backendEntryName(const size_t displayIndex) {
  FileIndex::Entry e;
  if (indexEntryAt(displayIndex, e)) {
    std::string name(e.name);
    if (e.isDir) name += '/';
    return name;
  }
  if (fileIndex || displayIndex >= files.size()) return "";  // index read failed, or OOR
  return files[displayIndex];
}

size_t FileBrowserModel::findEntry(const std::string& name) {
  if (mode == Mode::Authors) {
    const size_t count = entryCount();
    for (size_t i = 0; i < count; i++)
      if (entryName(i) == name) return i;
    return count;
  }
  if (listsPaths()) {
    for (size_t i = 0; i < deepResults.size(); i++)
      if (deepResults[i] == name) return i;
    return deepResults.size();
  }
  if (isFiltered()) {
    for (size_t i = 0; i < matches.size(); i++)
      if (backendEntryName(matches[i]) == name) return i;
    return matches.size();
  }
  if (fileIndex) {
    // The index stores names without the trailing '/'; strip it for the lookup.
    std::string bare = name;
    if (!bare.empty() && bare.back() == '/') bare.pop_back();
    const bool desc = (sortDirection == CrossPointSettings::SORT_DESCENDING);
    const size_t row = fileIndex->findRowByName(bare.c_str(), desc);
    return (row == SIZE_MAX) ? entryCount() : row;
  }
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return files.size();
}

void FileBrowserModel::setFilter(std::string query) {
  // Trim: a stray space from the keyboard should not be the reason nothing matches.
  while (!query.empty() && query.front() == ' ') query.erase(query.begin());
  while (!query.empty() && query.back() == ' ') query.pop_back();
  if (query == filterQuery) return;
  filterQuery = std::move(query);
  rebuildMatches();
}

void FileBrowserModel::rebuildMatches() {
  matches.clear();
  if (filterQuery.empty()) {
    matches.shrink_to_fit();  // an unfiltered browser should not keep the capacity around
    return;
  }
  // ASCII-folded substring match. Deliberately not a full Unicode fold: filenames on these cards
  // are overwhelmingly ASCII, and a UTF-8 case table costs more flash than the feature does.
  std::string needle = filterQuery;
  for (char& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

  const size_t total = unfilteredEntryCount();
  for (size_t i = 0; i < total && matches.size() < MAX_MATCHES; i++) {
    std::string name = backendEntryName(i);
    if (name.empty()) continue;
    if (name.back() == '/') name.pop_back();  // match on the name, not the marker
    for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (name.find(needle) != std::string::npos) matches.push_back(static_cast<uint32_t>(i));
  }
}

std::string FileBrowserModel::entryFullPath(const size_t displayIndex) {
  const std::string name = entryName(displayIndex);
  if (name.empty()) return "";
  const std::string& base = listsPaths() ? deepRoot : basepath;
  std::string full = base;
  if (full.empty() || full.back() != '/') full += '/';
  full += name;
  if (!full.empty() && full.back() == '/') full.pop_back();  // directories carry a marker
  return full;
}

uint32_t FileBrowserModel::entrySize(const size_t displayIndex) {
  if (listsPaths()) return 0;
  size_t backendIndex = displayIndex;
  if (isFiltered()) {
    if (displayIndex >= matches.size()) return 0;
    backendIndex = matches[displayIndex];
  }
  FileIndex::Entry e;
  if (indexEntryAt(backendIndex, e)) return e.isDir ? 0 : e.size;
  if (fileIndex || backendIndex >= fileSizes.size()) return 0;
  return fileSizes[backendIndex];
}

std::string FileBrowserModel::resultFolder(const size_t displayIndex) {
  if (!listsPaths() || displayIndex >= entryCount()) return "";
  const std::string rel = entryName(displayIndex);
  const size_t slash = rel.rfind('/');
  if (slash == std::string::npos) return deepRoot;  // it sat in the search root
  std::string folder = deepRoot;
  if (folder.empty() || folder.back() != '/') folder += '/';
  folder += rel.substr(0, slash);
  return folder;
}

void FileBrowserModel::searchEverywhere(const std::string& query) {
  clearDeepSearch();
  if (query.empty()) return;

  std::string needle = query;
  for (char& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

  deepRoot = basepath;
  deepSearch = true;

  // Explicit stack, not recursion: the depth of a card is not ours to choose, and the reader's
  // task stack is not the place to find out. Same shape the browser's recursive delete uses.
  std::vector<std::string> pending;
  pending.push_back(basepath);

  char name[500];
  while (!pending.empty() && deepResults.size() < MAX_DEEP_RESULTS) {
    const std::string dirPath = std::move(pending.back());
    pending.pop_back();

    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      const bool isDir = entry.isDirectory();
      entry.close();
      // "." and ".." always; anything else beginning with a dot only when the browser is
      // showing hidden files, so a search sees exactly what browsing would.
      if (name[0] == '.') {
        const bool dotdot = (name[1] == '\0') || (name[1] == '.' && name[2] == '\0');
        if (dotdot || !SETTINGS.showHiddenFiles) continue;
      }
      std::string child = dirPath;
      if (child.empty() || child.back() != '/') child += '/';
      child += name;
      if (isDir) {
        pending.push_back(std::move(child));
        continue;
      }
      if (!acceptEntry(name, false)) continue;
      std::string folded = name;
      for (char& c : folded) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (folded.find(needle) == std::string::npos) continue;
      if (deepResults.size() >= MAX_DEEP_RESULTS) {
        deepTruncated = true;
        break;
      }
      // Stored relative to the root, so the row shows which folder it came from.
      deepResults.push_back(child.substr(deepRoot.size() + (deepRoot.back() == '/' ? 0 : 1)));
    }
    dir.close();
  }
  if (!pending.empty() && deepResults.size() >= MAX_DEEP_RESULTS) deepTruncated = true;
}

void FileBrowserModel::resort() {
  if (fileIndex || listsPaths() || mode == Mode::Authors) return;  // ordered at build time / by the index
  // Whatever happens below renumbers the rows, so the match list is rebuilt at the end.
  // Create index array to preserve metadata array alignment
  std::vector<size_t> indices(files.size());
  for (size_t i = 0; i < files.size(); ++i) indices[i] = i;

  std::sort(indices.begin(), indices.end(), [this](size_t idx_a, size_t idx_b) {
    const std::string& a = files[idx_a];
    const std::string& b = files[idx_b];
    const bool isDir_a = a.back() == '/';
    const bool isDir_b = b.back() == '/';

    // Directories always sort first
    if (isDir_a != isDir_b) return isDir_a;

    // Both are directories or both are files; apply sort mode
    const char* name_a = a.c_str();
    const char* name_b = b.c_str();

    int cmp = 0;  // -1 if a < b, 0 if equal, +1 if a > b

    switch (sortMode) {
      case CrossPointSettings::SORT_BY_NAME:
        cmp = FsHelpers::naturalCompare(name_a, name_b);
        break;

      case CrossPointSettings::SORT_BY_DATE: {
        // Use cached metadata (no file opens)
        uint32_t dt_a = (idx_a < fileDateTimes.size()) ? fileDateTimes[idx_a] : 0;
        uint32_t dt_b = (idx_b < fileDateTimes.size()) ? fileDateTimes[idx_b] : 0;
        if (dt_a < dt_b) {
          cmp = -1;
        } else if (dt_a > dt_b) {
          cmp = 1;
        } else {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      case CrossPointSettings::SORT_BY_SIZE: {
        // Use cached metadata (no file opens)
        uint32_t size_a = (idx_a < fileSizes.size()) ? fileSizes[idx_a] : 0;
        uint32_t size_b = (idx_b < fileSizes.size()) ? fileSizes[idx_b] : 0;
        if (size_a < size_b) {
          cmp = -1;
        } else if (size_a > size_b) {
          cmp = 1;
        } else {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      case CrossPointSettings::SORT_BY_TYPE: {
        std::string ext_a = fileExtension(a);
        std::string ext_b = fileExtension(b);
        // Case-insensitive extension comparison
        std::transform(ext_a.begin(), ext_a.end(), ext_a.begin(), ::tolower);
        std::transform(ext_b.begin(), ext_b.end(), ext_b.begin(), ::tolower);
        cmp = FsHelpers::naturalCompare(ext_a.c_str(), ext_b.c_str());
        if (cmp == 0) {
          cmp = FsHelpers::naturalCompare(name_a, name_b);  // Tie: use name
        }
        break;
      }

      default:
        cmp = 0;
    }

    // Apply sort direction
    if (sortDirection == CrossPointSettings::SORT_DESCENDING) {
      cmp = -cmp;
    }

    return cmp < 0;
  });

  // Reorder files vector and metadata arrays based on sorted indices
  std::vector<std::string> sorted_files(files.size());
  std::vector<uint32_t> sorted_sizes(fileSizes.size());
  std::vector<uint32_t> sorted_dateTimes(fileDateTimes.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    size_t idx = indices[i];
    sorted_files[i] = files[idx];
    if (idx < fileSizes.size()) sorted_sizes[i] = fileSizes[idx];
    if (idx < fileDateTimes.size()) sorted_dateTimes[i] = fileDateTimes[idx];
  }
  files = std::move(sorted_files);
  fileSizes = std::move(sorted_sizes);
  fileDateTimes = std::move(sorted_dateTimes);
  rebuildMatches();  // the rows were just renumbered
}

// The recent-books list, newest first, as paths relative to "/" -- the shape a card-wide search's
// results already have, so every row accessor serves both. Books no longer on the card are dropped
// from the list for good, as the Recent Books screen always did on the way in.
void FileBrowserModel::loadRecents() {
  clearDeepSearch();
  // One load of the list for the prune, its save and the read below; the paths are copied out, so
  // nothing here outlives the hold (RecentBooksStore::Hold).
  const RecentBooksStore::Hold recents;
  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  deepRoot = "/";
  const auto& books = RECENT_BOOKS.getBooks();
  deepResults.reserve(books.size());
  for (const auto& book : books) {
    if (book.path.size() > 1 && book.path.front() == '/') deepResults.push_back(book.path.substr(1));
  }
}

void FileBrowserModel::clear() {
  files.clear();
  fileSizes.clear();
  fileDateTimes.clear();
  filterQuery.clear();
  matches.clear();
  matches.shrink_to_fit();
  clearDeepSearch();
  bookIndex.close();
  openAuthorRow = -1;
  authorBooks.clear();
  authorBooks.shrink_to_fit();
  if (fileIndex) fileIndex->close();
  fileIndex = nullptr;
}

bool FileBrowserModel::isBookName(const char* name) { return isReadableBook(std::string_view{name}); }

void FileBrowserModel::releaseIndex() { bookIndex.close(); }

// The index's newest books, as paths from the root like Recents' rows. A book gone since the index
// was built is left out.
void FileBrowserModel::loadAdded() {
  clearDeepSearch();
  deepRoot = "/";
  if (!bookIndex.isOpen() && !bookIndex.open(library::INDEX_PATH)) return;
  std::string path;
  library::BookRecord record{};
  for (uint16_t rank = 0; rank < bookIndex.header().newCount; ++rank) {
    uint16_t index = 0;
    if (!bookIndex.newBook(rank, index) || !bookIndex.book(index, record) || !bookIndex.blobString(record.pathOff, path)) {
      continue;
    }
    if (path.size() > 1 && path.front() == '/' && Storage.exists(path.c_str())) deepResults.push_back(path.substr(1));
  }
}

// The author list, or -- when an author was open -- that author's books again, found by hash: a
// rebuilt index may have moved its row.
void FileBrowserModel::loadAuthors() {
  clearDeepSearch();
  deepRoot = "/";
  if (!bookIndex.isOpen()) bookIndex.open(library::INDEX_PATH);
  if (openAuthorRow < 0) return;
  openAuthorRow = -1;
  authorBooks.clear();
  library::AuthorRecord author{};
  for (uint16_t row = 0; row < bookIndex.header().authorCount; ++row) {
    if (bookIndex.author(row, author) && author.hash == openAuthorHash) {
      openAuthor(row);
      return;
    }
  }
}

bool FileBrowserModel::openAuthor(const size_t row) {
  library::AuthorRecord author{};
  if (!atAuthorList() || row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return false;
  authorBooks.clear();
  authorBooks.reserve(author.count);
  for (uint32_t slot = author.firstBook; slot < uint32_t{author.firstBook} + author.count; ++slot) {
    uint16_t record = 0;
    if (bookIndex.authorBook(slot, record)) authorBooks.push_back(record);
  }
  openAuthorRow = static_cast<int>(row);
  openAuthorHash = author.hash;
  deepRoot = "/";
  orderAuthorBooks();
  return true;
}

size_t FileBrowserModel::closeAuthor() {
  const int row = openAuthorRow;
  openAuthorRow = -1;
  authorBooks.clear();
  authorBooks.shrink_to_fit();
  return row < 0 ? 0 : static_cast<size_t>(row);
}

bool FileBrowserModel::authorAt(const size_t row, uint16_t& books, uint32_t& hash) {
  library::AuthorRecord author{};
  if (row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return false;
  books = author.count;
  hash = author.hash;
  return true;
}

// An author row, marked as a folder: opening it lists the author's books.
std::string FileBrowserModel::authorRowName(const size_t row) {
  library::AuthorRecord author{};
  std::string name;
  if (row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return "";
  if (author.hash == library::AUTHOR_PENDING) {
    name = tr(STR_NOT_YET_INDEXED);
  } else if (author.hash == library::AUTHOR_UNKNOWN || !bookIndex.authorName(author, name) || name.empty()) {
    name = tr(STR_UNKNOWN_AUTHOR);
  }
  return name + '/';
}

std::string FileBrowserModel::authorBookName(const size_t index) {
  library::BookRecord record{};
  std::string path;
  if (index >= authorBooks.size() || !bookIndex.book(authorBooks[index], record) ||
      !bookIndex.blobString(record.pathOff, path) || path.size() < 2) {
    return "";
  }
  return path.substr(1);
}

// An author's books by series, then series index, then title, as the book lists show them; books in
// no series after the series. Past MAX_ORDERED books the index's order stands: the details reads
// would take seconds.
void FileBrowserModel::orderAuthorBooks() {
  constexpr size_t MAX_ORDERED = 200;
  if (authorBooks.size() < 2 || authorBooks.size() > MAX_ORDERED) return;
  struct Key {
    std::string series;
    float index;
    std::string title;
    uint16_t record;
  };
  std::vector<Key> keys;
  keys.reserve(authorBooks.size());
  library::BookRecord record{};
  std::string path;
  for (const uint16_t r : authorBooks) {
    Key key{{}, 0.0f, {}, r};
    if (bookIndex.book(r, record) && bookIndex.blobString(record.pathOff, path)) {
      BookDetails details;
      if (BookDetailsLookup::cached(path, 0, details)) {
        key.series = std::move(details.series);
        key.index = strtof(details.seriesIndex.c_str(), nullptr);
        key.title = std::move(details.title);
      }
      if (key.title.empty()) key.title = path.substr(path.rfind('/') + 1);
    }
    keys.push_back(std::move(key));
  }
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
    if (a.series.empty() != b.series.empty()) return !a.series.empty();
    if (a.series != b.series) return FsHelpers::naturalCompare(a.series.c_str(), b.series.c_str()) < 0;
    if (a.index != b.index) return a.index < b.index;
    return FsHelpers::naturalCompare(a.title.c_str(), b.title.c_str()) < 0;
  });
  for (size_t i = 0; i < keys.size(); ++i) authorBooks[i] = keys[i].record;
}
