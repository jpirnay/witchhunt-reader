#pragma once

#include <FileReplace.h>
#include <HalStorage.h>
#include <HeapFit.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "Bookmark.h"

// What a book's bookmarks.bin and the global index have in common: the record, and how the file is read and replaced.
//
// A count or a length read from a file is data. On the device a std container that cannot get its block aborts
// (-fno-exceptions, HeapFit.h), so each is held to what the file can hold and what the heap can give before it reaches
// reserve() or resize(). Ideas from CrossInk 53aa4f39 (Julia Nguyen).
namespace bookmark_file {

enum class ReadResult : uint8_t {
  Ok,
  Damaged,   // the file ends early or says something impossible: what came before it stands
  NoMemory,  // the file may be fine: it is not to be written over from what could be read
};

inline constexpr size_t kMaxNameBytes = 128;
// spine, page and the length of an empty name.
inline constexpr size_t kMinRecordBytes = 3 * sizeof(uint16_t);

template <typename T>
bool readPod(FsFile& f, T& value) {
  return f.read(reinterpret_cast<uint8_t*>(&value), sizeof(value)) == static_cast<int>(sizeof(value));
}

template <typename T>
bool writePod(FsFile& f, const T& value) {
  return f.write(reinterpret_cast<const uint8_t*>(&value), sizeof(value)) == sizeof(value);
}

// A 16-bit length and that many bytes.
inline ReadResult readString(FsFile& f, std::string& out, const size_t maxBytes) {
  uint16_t length = 0;
  if (!readPod(f, length)) return ReadResult::Damaged;
  out.clear();
  if (length == 0) return ReadResult::Ok;
  if (length > maxBytes || length > f.available()) return ReadResult::Damaged;
  if (!heapHasBlockFor(length + 1u)) return ReadResult::NoMemory;
  out.resize(length);
  return f.read(reinterpret_cast<uint8_t*>(&out[0]), length) == length ? ReadResult::Ok : ReadResult::Damaged;
}

inline bool writeString(FsFile& f, const std::string& s, const size_t maxBytes) {
  const auto length = static_cast<uint16_t>(std::min(s.size(), maxBytes));
  if (!writePod(f, length)) return false;
  return length == 0 || f.write(reinterpret_cast<const uint8_t*>(s.data()), length) == length;
}

inline ReadResult readBookmark(FsFile& f, Bookmark& bookmark, const bool named = true) {
  if (!readPod(f, bookmark.spineIndex) || !readPod(f, bookmark.pageNumber)) return ReadResult::Damaged;
  return named ? readString(f, bookmark.name, kMaxNameBytes) : ReadResult::Ok;
}

inline bool writeBookmark(FsFile& f, const Bookmark& bookmark) {
  return writePod(f, bookmark.spineIndex) && writePod(f, bookmark.pageNumber) &&
         writeString(f, bookmark.name, kMaxNameBytes);
}

// A replace cut off by a power loss leaves the old file aside (FileReplace.h). Without the new one in place that is the
// only copy, and it goes back; with it, the old one only wants removing. Either way the next replace is not refused.
inline void recoverReplace(const std::string& path) {
  const std::string aside = path + FileReplace::kAsideSuffix;
  if (!Storage.exists(aside.c_str())) return;
  if (Storage.exists(path.c_str())) {
    Storage.remove(aside.c_str());
  } else {
    Storage.rename(aside.c_str(), path.c_str());
  }
}

// Writes `path` by way of a new file, so a write that fails, or a card that fills, leaves the old one standing.
template <typename Write>
bool replaceFile(const char* module, const std::string& path, Write&& write) {
  recoverReplace(path);
  const std::string part = path + ".part";
  FsFile f;
  if (!Storage.openFileForWrite(module, part.c_str(), f)) return false;
  const bool written = std::forward<Write>(write)(f);
  const bool closed = f.close();
  if (written && closed && FileReplace::replace(part.c_str(), path.c_str())) return true;
  Storage.remove(part.c_str());
  return false;
}

}  // namespace bookmark_file
