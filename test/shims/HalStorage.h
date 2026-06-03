#pragma once
// Minimal stub for host/test builds — only the types needed to compile
// CssParser.h and CssStyle.h without ESP32/SdFat dependencies.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Print.h"
#include "WString.h"

// Minimal FsFile stub — only the methods CssParser calls at runtime are needed.
// Since tests only exercise parseInlineStyle / parseDeclarations (static, no I/O),
// none of these methods will actually be called.
class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() = default;
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush() {}
  size_t getName(char*, size_t) { return 0; }
  size_t size() { return 0; }
  size_t fileSize() { return 0; }
  bool seek(size_t) { return false; }
  bool seekCur(int64_t) { return false; }
  bool seekSet(size_t) { return false; }
  int available() const { return 0; }
  size_t position() const { return 0; }
  int read(void*, size_t) { return -1; }
  int read() { return -1; }
  uint64_t size64() { return 0; }
  uint64_t fileSize64() { return 0; }
  bool seek64(uint64_t) { return false; }
  bool seekSet64(uint64_t) { return false; }
  uint64_t position64() const { return 0; }
  size_t write(const void*, size_t) { return 0; }
  size_t write(uint8_t) override { return 0; }
  bool rename(const char*) { return false; }
  bool getModifyDateTime(uint16_t*, uint16_t*) { return false; }
  bool isDirectory() const { return false; }
  void rewindDirectory() {}
  bool close() { return false; }
  HalFile openNextFile() { return HalFile{}; }
  bool isOpen() const { return false; }
  operator bool() const { return false; }
};

using FsFile = HalFile;

class HalStorage {
 public:
  bool begin() { return false; }
  bool ready() const { return false; }
  std::vector<String> listFiles(const char* = "/", int = 200) { return {}; }
  String readFile(const char*) { return ""; }
  bool readFileToStream(const char*, Print&, size_t = 256) { return false; }
  size_t readFileToBuffer(const char*, char*, size_t, size_t = 0) { return 0; }
  bool writeFile(const char*, const String&) { return false; }
  bool ensureDirectoryExists(const char*) { return false; }
  HalFile open(const char*, int = 0) { return HalFile{}; }
  bool mkdir(const char*, bool = true) { return false; }
  bool exists(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
  bool rmdir(const char*) { return false; }
  bool openFileForRead(const char*, const char*, HalFile&) { return false; }
  bool openFileForRead(const char*, const std::string&, HalFile&) { return false; }
  bool openFileForRead(const char*, const String&, HalFile&) { return false; }
  bool openFileForWrite(const char*, const char*, HalFile&) { return false; }
  bool openFileForWrite(const char*, const std::string&, HalFile&) { return false; }
  bool openFileForWrite(const char*, const String&, HalFile&) { return false; }
  bool removeDir(const char*) { return false; }
  bool copyFile(const char*, const std::string&, const char*) { return false; }
  uint64_t sdTotalBytes() const { return 0; }
  uint64_t sdUsedBytes() { return 0; }
  uint64_t sdFreeBytes() { return 0; }
  static HalStorage& getInstance() {
    static HalStorage inst;
    return inst;
  }
};

#define Storage HalStorage::getInstance()
