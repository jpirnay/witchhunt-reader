#pragma once
// Real-filesystem shim for host tests that exercise ZipFile or CssParser.
// Implements FsFile on top of stdio so tests can read actual .epub files
// and write real cache files under /tmp.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "Print.h"

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() { close(); }

  HalFile(HalFile&& o) noexcept : fp_(o.fp_) { o.fp_ = nullptr; }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      close();
      fp_ = o.fp_;
      o.fp_ = nullptr;
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool openForRead(const std::string& path) {
    close();
    fp_ = fopen(path.c_str(), "rb");
    return fp_ != nullptr;
  }

  bool openForWrite(const std::string& path) {
    close();
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) return false;
    }
    fp_ = fopen(path.c_str(), "wb");
    return fp_ != nullptr;
  }

  bool close() {
    if (fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
    return true;
  }

  bool isOpen() const { return fp_ != nullptr; }
  explicit operator bool() const { return fp_ != nullptr; }

  int read(void* buf, size_t n) {
    if (!fp_) return -1;
    return static_cast<int>(fread(buf, 1, n, fp_));
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
  size_t write(uint8_t c) override {
    if (!fp_) return 0;
    return fwrite(&c, 1, 1, fp_);
  }
  void flush() {
    if (fp_) fflush(fp_);
  }
  size_t getName(char*, size_t) { return 0; }
  bool rename(const char*) { return false; }
  bool getModifyDateTime(uint16_t*, uint16_t*) { return false; }
  bool isDirectory() const { return false; }
  void rewindDirectory() {}
  HalFile openNextFile() { return HalFile{}; }

 private:
  FILE* fp_ = nullptr;
};

using FsFile = HalFile;

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& f) { return f.openForRead(path); }
  bool openFileForRead(const char*, const char* path, HalFile& f) { return f.openForRead(path); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& f) { return f.openForWrite(path); }
  bool exists(const char* path) { return std::filesystem::exists(path); }
  bool mkdir(const char* path, bool recursive = true) {
    std::error_code ec;
    return recursive ? std::filesystem::create_directories(path, ec) : std::filesystem::create_directory(path, ec);
  }
  bool remove(const char* path) { return std::filesystem::remove(path); }
  std::vector<std::string> listFiles(const char* = "/", int = 200) { return {}; }
  static HalStorage& getInstance() {
    static HalStorage i;
    return i;
  }
};

#define Storage HalStorage::getInstance()
