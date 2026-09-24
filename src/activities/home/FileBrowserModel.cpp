#include "FileBrowserModel.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace {

// The part of the filter that does not depend on what the browser is picking: a hidden entry and
// the FAT volume-information folder are never listed, whatever the mode.
bool isListableName(const char* name) {
  if (!SETTINGS.showHiddenFiles && name[0] == '.') return false;
  return strcmp(name, "System Volume Information") != 0;
}

// The file types the reader can open.
bool isReadableBook(const std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
         FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
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
    case Mode::Books:
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
    case Mode::Books:
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
  if (deepSearch) return deepResults.size();
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
  if (deepSearch) {
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
  const std::string& base = deepSearch ? deepRoot : basepath;
  std::string full = base;
  if (full.empty() || full.back() != '/') full += '/';
  full += name;
  if (!full.empty() && full.back() == '/') full.pop_back();  // directories carry a marker
  return full;
}

std::string FileBrowserModel::resultFolder(const size_t displayIndex) {
  if (!deepSearch || displayIndex >= deepResults.size()) return "";
  const std::string& rel = deepResults[displayIndex];
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
  if (fileIndex) return;  // the index is ordered at build time; see the header.
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

void FileBrowserModel::clear() {
  files.clear();
  fileSizes.clear();
  fileDateTimes.clear();
  filterQuery.clear();
  matches.clear();
  matches.shrink_to_fit();
  clearDeepSearch();
  if (fileIndex) fileIndex->close();
  fileIndex = nullptr;
}
