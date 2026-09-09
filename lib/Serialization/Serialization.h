#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {
template <typename T>
[[maybe_unused]] static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
[[maybe_unused]] static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
[[maybe_unused]] static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
[[maybe_unused]] static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

[[maybe_unused]] static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

[[maybe_unused]] static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

// Copies `bytes` from `in` at its current position into `out` at its current position, through a
// small fixed buffer.
//
// Two caches now park a table in a scratch file while the pass that produced it overwrites where
// it used to live, and splice it back afterwards: the section cache's anchor map and the footnote
// preview store's hash index. Both do it for the same reason -- the table scales with the book
// and must not be resident -- and both run at the end of a parse, where contiguous heap is at its
// lowest, which is why the buffer is a stack array and not an allocation.
constexpr size_t COPY_CHUNK_BYTES = 512;
[[maybe_unused]] static bool copyBytes(FsFile& in, FsFile& out, uint32_t bytes) {
  uint8_t chunk[COPY_CHUNK_BYTES];
  while (bytes > 0) {
    const size_t want = bytes < COPY_CHUNK_BYTES ? static_cast<size_t>(bytes) : COPY_CHUNK_BYTES;
    if (in.read(chunk, want) != static_cast<int>(want)) return false;
    if (out.write(chunk, want) != want) return false;
    bytes -= static_cast<uint32_t>(want);
  }
  return true;
}

constexpr uint32_t MAX_STRING_LENGTH = 4096;

[[maybe_unused]] static bool readString(std::istream& is, std::string& s) {
  uint32_t len = 0;
  readPod(is, len);
  if (!is) return false;
  if (len > MAX_STRING_LENGTH) {
    is.seekg(len, std::ios::cur);  // skip payload to keep stream aligned
    return false;
  }
  s.resize(len);
  is.read(&s[0], len);
  return true;
}

[[maybe_unused]] static bool readString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len)) return false;
  if (len > MAX_STRING_LENGTH) {
    if (!file.seekCur(static_cast<int64_t>(len))) {  // skip payload to keep file position aligned
      return false;
    }
    return false;
  }
  s.resize(len);
  file.read(reinterpret_cast<uint8_t*>(&s[0]), len);
  return true;
}
}  // namespace serialization
