#pragma once
#include <HalStorage.h>

#include <memory>
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

  // Same as readBytesFromEntry but for a caller that already holds the entry's central-dir
  // stat (e.g. from a prior loadFileStatSlim), avoiding a second central-directory scan.
  size_t readBytesFromStat(const FileStatSlim& fileStat, uint8_t* outBuf, size_t maxBytes);

  // Resumable reader for a single ZIP entry. Holds the file handle and inflate
  // state alive across calls so the caller can feed decompressed bytes in small
  // slices without consuming the whole entry in one shot.
  //
  // Usage:
  //   ZipFile::EntryReader reader(zipFile);
  //   if (!reader.open("OEBPS/chapter.xhtml")) { /* error */ }
  //   uint8_t buf[1024];
  //   size_t produced; bool done;
  //   while (!done) {
  //     if (!reader.step(buf, sizeof(buf), &produced, &done)) { /* error */ break; }
  //     sink.write(buf, produced);
  //   }
  //
  // The parent ZipFile may be used concurrently for other operations (stat
  // lookups etc.) since EntryReader opens its own file handle.
  // Non-copyable; movable.
  class EntryReader {
   public:
    explicit EntryReader(ZipFile& zf, size_t chunkSize = 1024);
    ~EntryReader();
    EntryReader(EntryReader&&) noexcept;
    EntryReader& operator=(EntryReader&&) noexcept;
    EntryReader(const EntryReader&) = delete;
    EntryReader& operator=(const EntryReader&) = delete;

    // Open a named entry for reading. Looks up the central-dir stat and seeks
    // to the data start. Returns false if the entry is not found or on I/O error.
    // Closes any previously open entry first.
    bool open(const char* filename);

    // Decompress up to `cap` bytes into `out`. Sets `*produced` to the number
    // of bytes written and `*done` to true when the entry is exhausted.
    // Returns false on decompression error. Must not be called after done=true
    // or after a previous false return.
    bool step(uint8_t* out, size_t cap, size_t* produced, bool* done);

    // Close the entry and release the file handle / inflate state.
    // Safe to call repeatedly or on an already-closed reader.
    void close();

    bool isOpen() const;
    // Total uncompressed size reported in the ZIP header; valid after open().
    size_t inflatedSize() const;
    // Bytes decompressed so far across all step() calls.
    size_t bytesProduced() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };

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
