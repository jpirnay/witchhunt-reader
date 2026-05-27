#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// Host-build stub: satisfies BookMetadataCache.h's FsFile usage.
// No methods are ever called in benchmark/test builds.
class HalFile {
 public:
  HalFile() = default;
  ~HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush() {}
  size_t size() { return 0; }
  size_t fileSize() { return 0; }
  bool seek(size_t) { return false; }
  bool seekCur(int64_t) { return false; }
  bool seekSet(size_t) { return false; }
  int available() const { return 0; }
  size_t position() const { return 0; }
  int read(void*, size_t) { return 0; }
  int read() { return -1; }
  uint64_t size64() { return 0; }
  bool seek64(uint64_t) { return false; }
  bool seekSet64(uint64_t) { return false; }
  uint64_t position64() const { return 0; }
  size_t write(const void*, size_t) { return 0; }
  size_t write(uint8_t) { return 0; }
  bool close() { return false; }
  bool isOpen() const { return false; }
  explicit operator bool() const { return false; }
};

using FsFile = HalFile;
