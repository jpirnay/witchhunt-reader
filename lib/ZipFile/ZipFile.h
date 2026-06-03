#pragma once
#include <HalStorage.h>

#include <string>
#include <string_view>
#include <vector>

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  // Target for batch uncompressed size lookup (sorted by hash, then len)
  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length of path for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  FsFile file;
  ZipDetails zipDetails = {0, 0, false};

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();

  // RAII helper: opens the zip if not already open, closes on destruction only if
  // it performed the open. Defined here so template methods in the header can use it.
  class ScopedOpenClose final {
   public:
    [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf_(zf), needsClose_(!zf.isOpen()) {
      if (needsClose_) ok_ = zf_.open();
    }
    ~ScopedOpenClose() {
      if (needsClose_ && ok_) zf_.close();
    }
    ScopedOpenClose(const ScopedOpenClose&) = delete;
    ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
    ScopedOpenClose(ScopedOpenClose&&) = delete;
    ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
    explicit operator bool() const { return ok_ || !needsClose_; }

   private:
    ZipFile& zf_;
    bool needsClose_ = false;
    bool ok_ = true;
  };

 public:
  // Look up a single entry's stat by scanning the central directory sequentially.
  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(const std::vector<SizeTarget>& targets, std::vector<uint32_t>& sizes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize);
  // Read up to maxBytes decompressed bytes from a ZIP entry without extracting the full file.
  // Returns the number of bytes actually written to outBuf (may be less than maxBytes if the
  // entry is smaller). Useful for header-only reads to get image dimensions.
  size_t readBytesFromEntry(const char* filename, uint8_t* outBuf, size_t maxBytes);

  // Stream every filename in the central directory to a callback without building
  // the in-memory stat cache. Uses a fixed 256-byte stack buffer — O(1) heap.
  // Safe for large EPUBs (3000+ entries) where loadAllFileStatSlims() would OOM.
  // Callback signature: void(std::string_view filename).
  template <typename F>
  bool streamCentralDirectoryNames(F&& callback) {
    if (!loadZipDetails()) return false;
    const ScopedOpenClose zip{*this};
    if (!zip) return false;
    file.seek(zipDetails.centralDirOffset);
    char nameBuf[256];
    uint32_t sig;
    while (file.available()) {
      if (file.read(&sig, 4) != 4 || sig != 0x02014b50) break;
      file.seekCur(6);
      uint16_t method;
      file.read(&method, 2);
      file.seekCur(8);
      uint32_t compSz, uncompSz, localOff;
      file.read(&compSz, 4);
      file.read(&uncompSz, 4);
      uint16_t nameLen, extraLen, commentLen;
      file.read(&nameLen, 2);
      file.read(&extraLen, 2);
      file.read(&commentLen, 2);
      file.seekCur(8);
      file.read(&localOff, 4);
      if (nameLen < sizeof(nameBuf)) {
        file.read(nameBuf, nameLen);
        nameBuf[nameLen] = '\0';
        callback(std::string_view{nameBuf, nameLen});
      } else {
        file.seekCur(nameLen);
      }
      file.seekCur(extraLen + commentLen);
    }
    return true;
  }
};
