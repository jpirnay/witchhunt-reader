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

bool FileBrowserModel::acceptEntry(const char* name, const bool isDir) const {
  return mode == Mode::PickFirmware ? acceptForFirmware(name, isDir) : acceptForBooks(name, isDir);
}

FileIndex::AcceptFn FileBrowserModel::indexFilter() const {
  return mode == Mode::PickFirmware ? &acceptForFirmware : &acceptForBooks;
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
}

size_t FileBrowserModel::entryCount() const { return fileIndex ? fileIndex->totalCount() : files.size(); }

bool FileBrowserModel::indexEntryAt(const size_t displayIndex, FileIndex::Entry& out) {
  if (!fileIndex) return false;
  const bool desc = (sortDirection == CrossPointSettings::SORT_DESCENDING);
  return fileIndex->entryAt(displayIndex, desc, out);
}

// Returns the row's name in the canonical browser form: a trailing '/' marks a
// directory. For the in-RAM backend `files` already stores this form; for the SD
// index we reconstruct it from the Entry. Out-of-range / index-read failure → "".
std::string FileBrowserModel::entryName(const size_t displayIndex) {
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

void FileBrowserModel::resort() {
  if (fileIndex) return;  // the index is ordered at build time; see the header.
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
}

void FileBrowserModel::clear() {
  files.clear();
  fileSizes.clear();
  fileDateTimes.clear();
  if (fileIndex) fileIndex->close();
  fileIndex = nullptr;
}
