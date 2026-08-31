#pragma once
// Real-filesystem shim for host tests that exercise ZipFile or CssParser.
// Implements FsFile on top of stdio so tests can read actual .epub files
// and write real cache files under /tmp.

#include <fcntl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Device HalStorage.h transitively provides the Arduino core (millis, ESP);
// mirror that so TUs which only include HalStorage.h still compile.
#include "Arduino.h"
#include "Print.h"
#include "WString.h"

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() { closeQuiet(); }

  HalFile(HalFile&& o) noexcept
      : fp_(o.fp_),
        hasImpl_(o.hasImpl_),
        isDir_(o.isDir_),
        hasName_(o.hasName_),
        name_(std::move(o.name_)),
        entries_(std::move(o.entries_)),
        next_(o.next_) {
    o.fp_ = nullptr;
    o.hasImpl_ = false;
    o.isDir_ = false;
    o.hasName_ = false;
    o.next_ = 0;
  }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      closeQuiet();
      fp_ = o.fp_;
      hasImpl_ = o.hasImpl_;
      isDir_ = o.isDir_;
      hasName_ = o.hasName_;
      name_ = std::move(o.name_);
      entries_ = std::move(o.entries_);
      next_ = o.next_;
      o.fp_ = nullptr;
      o.hasImpl_ = false;
      o.isDir_ = false;
      o.hasName_ = false;
      o.next_ = 0;
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool openForRead(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    fp_ = fopen(path.c_str(), "rb");
    return fp_ != nullptr;
  }

  bool openForWrite(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) return false;
    }
    fp_ = fopen(path.c_str(), "wb");
    return fp_ != nullptr;
  }

  // Mirrors HalStorage::openFileForUpdate: existing file, read+write, no truncation.
  bool openForUpdate(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    fp_ = fopen(path.c_str(), "r+b");
    return fp_ != nullptr;
  }

  // Device parity, deliberately fatal. HalStorage gives every handle it opens an Impl — even when
  // the open FAILED — so on device close() is fine on an opened-and-failed handle but asserts on
  // one that was never opened at all (or was moved from). stdio cannot tell those apart, so
  // hasImpl_ models it: without this, closing a never-opened handle is silently fine on the host
  // and aborts in the reader's hands. Not assert(): host tests build with NDEBUG.
  bool close() {
    if (!hasImpl_) {
      std::fprintf(stderr, "HalFile::close() on a handle that was never opened — this aborts on device\n");
      std::abort();
    }
    return closeQuiet();
  }

  // Directory handle, so tests can exercise code that walks the SD card
  // (DictionaryRegistry scans /dictionaries/*/ for index stems). Entries are
  // snapshotted and sorted at open time: std::filesystem iteration order is
  // unspecified, and device SdFat order is not reproducible either, so a test
  // that depends on it would be flaky rather than wrong.
  bool openDir(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) return false;
    isDir_ = true;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
      entries_.push_back({e.path().filename().string(), e.is_directory()});
    }
    std::sort(entries_.begin(), entries_.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return true;
  }

  bool isOpen() const { return fp_ != nullptr; }
  explicit operator bool() const { return fp_ != nullptr || isDir_ || hasName_; }

  int read(void* buf, size_t n) {
    if (!fp_) return -1;
    return static_cast<int>(fread(buf, 1, n, fp_));
  }
  // Single-byte read, SdFat-style: returns the byte or -1 (used by Bitmap.cpp).
  int read() {
    if (!fp_) return -1;
    return fgetc(fp_);
  }

  bool seek(size_t pos) { return fp_ && fseek(fp_, static_cast<long>(pos), SEEK_SET) == 0; }
  bool seekSet(size_t pos) { return seek(pos); }
  bool seekCur(int64_t offset) { return fp_ && fseek(fp_, static_cast<long>(offset), SEEK_CUR) == 0; }
  bool seek64(uint64_t pos) { return fp_ && fseek(fp_, static_cast<long>(pos), SEEK_SET) == 0; }
  bool seekSet64(uint64_t pos) { return seek64(pos); }

  size_t position() const { return fp_ ? static_cast<size_t>(ftell(fp_)) : 0; }
  uint64_t position64() const { return position(); }
  int available() const { return fp_ ? 1 : 0; }

  size_t size() { return fileSize(); }
  size_t fileSize() {
    if (!fp_) return 0;
    const long cur = ftell(fp_);
    fseek(fp_, 0, SEEK_END);
    const long sz = ftell(fp_);
    fseek(fp_, cur, SEEK_SET);
    return static_cast<size_t>(sz);
  }
  uint64_t size64() { return fileSize(); }
  uint64_t fileSize64() { return fileSize(); }

  size_t write(const uint8_t* data, size_t size) {
    if (!fp_) return 0;
    return fwrite(data, 1, size, fp_);
  }
  // Device SdFat accepts void*; Page.cpp writes char arrays through this.
  size_t write(const void* data, size_t size) {
    if (!fp_) return 0;
    return fwrite(data, 1, size, fp_);
  }
  size_t write(uint8_t c) override {
    if (!fp_) return 0;
    return fwrite(&c, 1, 1, fp_);
  }
  void flush() {
    if (fp_) fflush(fp_);
  }
  size_t getName(char* out, size_t len) {
    if (!out || len == 0) return 0;
    const size_t n = std::min(name_.size(), len - 1);
    std::memcpy(out, name_.data(), n);
    out[n] = '\0';
    return n;
  }
  bool rename(const char*) { return false; }
  bool getModifyDateTime(uint16_t*, uint16_t*) { return false; }
  bool isDirectory() const { return isDir_; }
  void rewindDirectory() { next_ = 0; }
  // One entry per call, exhausted-directory returns a falsy handle. Only the
  // name and the directory flag are populated — that is all a scan reads.
  HalFile openNextFile() {
    HalFile f;
    if (next_ >= entries_.size()) return f;
    const DirEntry& e = entries_[next_++];
    f.hasImpl_ = true;
    f.hasName_ = true;
    f.name_ = e.name;
    f.isDir_ = e.isDir;
    return f;
  }

 private:
  // close() without the device-parity check: for the destructor and the open helpers, which must
  // both tolerate a handle that was never opened.
  bool closeQuiet() {
    if (fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
    isDir_ = false;
    entries_.clear();
    next_ = 0;
    return true;
  }

  struct DirEntry {
    std::string name;
    bool isDir = false;
  };

  FILE* fp_ = nullptr;
  bool hasImpl_ = false;
  bool isDir_ = false;
  bool hasName_ = false;
  std::string name_;
  std::vector<DirEntry> entries_;
  size_t next_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& f) { return f.openForRead(path); }
  bool openFileForRead(const char*, const char* path, HalFile& f) { return f.openForRead(path); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& f) { return f.openForWrite(path); }
  bool openFileForWrite(const char*, const char* path, HalFile& f) { return f.openForWrite(path); }
  bool openFileForUpdate(const char*, const std::string& path, HalFile& f) { return f.openForUpdate(path); }
  bool openFileForUpdate(const char*, const char* path, HalFile& f) { return f.openForUpdate(path); }
  bool exists(const char* path) { return std::filesystem::exists(path); }
  // Mirrors the device HalStorage: stream a whole file into a Print sink in
  // chunks. Epub::applyMetadataSidecar() feeds ContentOpfParser through this,
  // so the host pipeline exercises the same code path the device runs.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256) {
    HalFile f;
    if (!f.openForRead(path)) return false;
    std::vector<uint8_t> buf(chunkSize ? chunkSize : 256);
    for (;;) {
      const int n = f.read(buf.data(), buf.size());
      if (n < 0) return false;
      if (n == 0) break;
      if (out.write(buf.data(), static_cast<size_t>(n)) != static_cast<size_t>(n)) return false;
    }
    return true;
  }
  bool mkdir(const char* path, bool recursive = true) {
    std::error_code ec;
    return recursive ? std::filesystem::create_directories(path, ec) : std::filesystem::create_directory(path, ec);
  }
  bool remove(const char* path) { return std::filesystem::remove(path); }
  bool removeDir(const char* path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
  }
  using oflag_t = int;
  HalFile open(const char* path, oflag_t = O_RDONLY) {
    HalFile f;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
      f.openDir(path);
    } else {
      f.openForRead(path);
    }
    return f;
  }
  // Sorted for determinism: std::filesystem iteration order is unspecified and
  // the pipeline harness requires byte-identical behavior across runs.
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
      if (e.is_regular_file()) names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    if (names.size() > static_cast<size_t>(maxFiles)) names.resize(maxFiles);
    std::vector<String> out;
    out.reserve(names.size());
    for (auto& n : names) out.push_back(String(std::move(n)));
    return out;
  }
  static HalStorage& getInstance() {
    static HalStorage i;
    return i;
  }
};

#define Storage HalStorage::getInstance()
