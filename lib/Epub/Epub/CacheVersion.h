#pragma once
// The version stamp the book's index caches open with: their first byte (book.bin's
// BOOK_CACHE_VERSION, css_rules.cache's CSS_CACHE_VERSION). A file of another version is rebuilt
// by the load that finds it.
#include <HalStorage.h>

#include <cstdint>
#include <string>

namespace cache_version {

// True when the file at `path` exists and starts with `version`: an open and a one-byte read.
inline bool isCurrent(const std::string& path, const uint8_t version) {
  // A plain open, not openFileForRead: a book never opened has none, and that logs every miss.
  FsFile f = Storage.open(path.c_str());
  if (!f) return false;
  uint8_t stamp = 0;
  const bool current = f.read(&stamp, sizeof(stamp)) == static_cast<int>(sizeof(stamp)) && stamp == version;
  f.close();
  return current;
}

}  // namespace cache_version
