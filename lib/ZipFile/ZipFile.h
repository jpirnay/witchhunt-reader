#pragma once
#include <HalStorage.h>

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

class BuildArena;

#ifdef ZIP_FILE_SCAN_STATS
// Host tests only (firmware never defines it): central-directory walks started and entries read by
// them, across every ZipFile. A lookup's cost on the card is the entries it reads, so this is what
// a test bounds where a host's wall-clock time says little.
struct ZipScanStats {
  size_t walks = 0;
  size_t entries = 0;
};
inline ZipScanStats& zipScanStats() {
  static ZipScanStats stats;
  return stats;
}
#define ZIP_SCAN_STAT_WALK() (++zipScanStats().walks)
#define ZIP_SCAN_STAT_ENTRY() (++zipScanStats().entries)
#else
#define ZIP_SCAN_STAT_WALK() ((void)0)
#define ZIP_SCAN_STAT_ENTRY() ((void)0)
#endif

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

  // A name for a batch walk to find (FNV-1a 64 of the normalised path, and its length), and the
  // central-directory stat the walk found for it. A table of them is sorted by (hash, len) with
  // before() and looked up afterwards with find(). 24 B: buildBookBin keeps one a spine item.
  // localHeaderOffset stays 0 for a name the walk did not find; offset 0 is the archive's first
  // entry, an EPUB's mimetype, never a document.
  struct StatTarget {
    uint64_t hash = 0;
    uint32_t uncompressedSize = 0;
    uint32_t compressedSize = 0;
    uint32_t localHeaderOffset = 0;
    uint16_t len = 0;
    uint16_t method = 0;

    static StatTarget named(std::string_view path);
    static bool before(const StatTarget& a, const StatTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    }
    // The first target of [begin, end), sorted by before(), that names `path`; null if none does.
    static const StatTarget* find(const StatTarget* begin, const StatTarget* end, std::string_view path);
    bool found() const { return localHeaderOffset != 0; }
    FileStatSlim stat() const { return {method, compressedSize, uncompressedSize, localHeaderOffset}; }
  };

  // A few entries' stats a caller noted during a walk it made anyway (contentFingerprint's
  // visitor), which loadFileStatSlim answers from before it walks (seedKnownEntries). Exact names
  // only, up to CAPACITY of them in NAME_BYTES of names: about 1.3 KB, for the handful of package
  // documents a book's first open reads by name.
  class KnownEntries {
   public:
    static constexpr size_t CAPACITY = 16;
    static constexpr size_t NAME_BYTES = 1024;
    // False when there is no room left for `name`; overflowed() from then on.
    bool add(std::string_view name, const FileStatSlim& stat);
    const FileStatSlim* find(std::string_view name) const;
    size_t size() const { return count_; }
    std::string_view name(const size_t i) const { return {names_ + entries_[i].at, entries_[i].len}; }
    // Some entry it was offered is not in it: it does not hold every name of its kind.
    bool overflowed() const { return overflowed_; }

   private:
    struct Entry {
      uint16_t at = 0;
      uint16_t len = 0;
      FileStatSlim stat = {};
    };
    Entry entries_[CAPACITY];
    char names_[NAME_BYTES] = {};
    size_t count_ = 0;
    size_t used_ = 0;
    bool overflowed_ = false;
  };

 private:
  const std::string& filePath;
  FsFile file;
  ZipDetails zipDetails = {0, 0, false};

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;
  // Where a lookup by name takes its read window (setLookupScratch); null: the heap.
  BuildArena* lookupScratch_ = nullptr;
  // Answers loadFileStatSlim has without a walk (seedKnownEntries); not owned.
  const KnownEntries* knownEntries_ = nullptr;

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
  // Look up a single entry's stat by scanning the central directory sequentially, from just past
  // the previous match (wrapping once), through a read window: 4 KB from the lookup scratch when
  // it has room, else 1 KB of heap, else straight from the file. Names compare in place (slashes
  // normalised, case ignored as a fallback); nothing is allocated per entry.
  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  // A region lookups by name may take their window from, a block at a time, released before
  // they return. For a caller holding a lent region (first-open indexing); null by default.
  void setLookupScratch(BuildArena* scratch) { lookupScratch_ = scratch; }
  // Entries loadFileStatSlim finds by exact name without walking; null: none. Must outlive the
  // lookups.
  void seedKnownEntries(const KnownEntries* known) { knownEntries_ = known; }
  // The lookup cursor, an offset into the archive (0: none). A caller that makes many short-lived
  // ZipFiles over one book hands it from one to the next, so a lookup for a name stored after the
  // previous one does not start again at the first entry.
  uint32_t lookupCursor() const { return lastCentralDirPosValid ? lastCentralDirPos : 0; }
  void seedLookupCursor(const uint32_t cursor) {
    if (cursor == 0 || lastCentralDirPosValid) return;
    lastCentralDirPos = cursor;
    lastCentralDirPosValid = true;
  }
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool getInflatedFileSize(const char* filename, size_t* size);

  // Absolute byte offset and length of an entry's data within the archive, and true, ONLY when
  // that entry is STORED (method 0). A caller can then read the entry straight out of the .zip
  // with no decompression and no temporary copy.
  //
  // False for a deflated entry -- there is no such range: those bytes are a DEFLATE stream, not
  // the file. Also false if the entry is missing or its local header does not check out.
  bool getStoredEntryRange(const char* filename, uint32_t* offset, uint32_t* size);
  // Batch lookup: ONE walk of the central directory fills in the stat of every target whose
  // (hash, len) names an entry, and leaves the others alone. `targets` must be sorted by
  // StatTarget::before; a caller with more names than it has table for walks once per table-full.
  // The walk reads through `window` (null: straight from the file, a dozen card calls per entry),
  // which the caller owns so that several walks share one. Returns the number of targets matched;
  // the walk stops once that reaches `count`.
  int fillFileStats(StatTarget* targets, size_t count, uint8_t* window, size_t windowBytes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, const bool* stop = nullptr);
  // Read up to maxBytes decompressed bytes from a ZIP entry without extracting the full file.
  // Returns the number of bytes actually written to outBuf (may be less than maxBytes if the
  // entry is smaller). Useful for header-only reads to get image dimensions.
  size_t readBytesFromEntry(const char* filename, uint8_t* outBuf, size_t maxBytes);

  // Same as readBytesFromEntry but for a caller that already holds the entry's central-dir
  // stat (e.g. from a prior loadFileStatSlim), avoiding a second central-directory scan.
  // `scratch`: when given, the inflate ring and read buffer come from a block reserved in it and
  // released before returning (heap when it has no room). The section build passes its lent
  // arena, so an image-header probe mid-parse costs the heap nothing.
  size_t readBytesFromStat(const FileStatSlim& fileStat, uint8_t* outBuf, size_t maxBytes,
                           BuildArena* scratch = nullptr);

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
    // arena (optional): carve the read buffer AND the inflate ring from the
    // given BuildArena instead of malloc. The reader reserves an arena block at
    // open() and releases it on close()/reset(). Any nested caller block must
    // be released before closing the reader.
    // Budget per open: chunkSize + InflateReader::ringSizeFor(entrySize) + alignment.
    explicit EntryReader(ZipFile& zf, size_t chunkSize = 1024, BuildArena* arena = nullptr);
    ~EntryReader();
    EntryReader(EntryReader&&) noexcept;
    EntryReader& operator=(EntryReader&&) noexcept;
    EntryReader(const EntryReader&) = delete;
    EntryReader& operator=(const EntryReader&) = delete;

    // Open a named entry for reading. Looks up the central-dir stat and seeks
    // to the data start. Returns false if the entry is not found or on I/O error.
    // Closes any previously open entry first.
    bool open(const char* filename);

    // Same, but for a caller that already holds the entry's central-dir stat
    // (e.g. from Epub's cached per-spine stat table) — skips the linear
    // central-directory scan loadFileStatSlim performs. Closes any previously
    // open entry first.
    bool open(const FileStatSlim& fileStat);
    // Same, reading at most `outputCap` bytes of the entry (0 = all of it): step() reports done at
    // the cap. A deflate back-reference never reaches further back than the bytes produced so far,
    // so the inflate ring is sized to min(entry, cap) instead of the entry -- a header walk that
    // stops 16 KB into a 400 KB JPEG costs a 16 KB ring, not 32 KB. Budget per open:
    // chunkSize + InflateReader::ringSizeFor(min(entry, cap)) + alignment.
    bool open(const FileStatSlim& fileStat, size_t outputCap);

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

  // EOCD scan-cache plumbing: every ZipFile instance normally re-runs the
  // (backward-scanning, ~4 KB-read) EOCD search on first use. Callers that
  // construct many short-lived instances over the same archive (Epub does, per
  // item read) can harvest details() after a successful operation and seed the
  // next instance, eliminating the repeated scans (~15 per book-open observed).
  const ZipDetails& details() const { return zipDetails; }
  void seedDetails(const ZipDetails& d) {
    if (d.isSet && !zipDetails.isSet) zipDetails = d;
  }

  // Content fingerprint of the archive: FNV-1a 64 over every central-directory
  // entry's name bytes, CRC-32, uncompressed size, compression method and
  // local-header offset, plus the entry count. One sequential walk of the
  // central directory, O(1) heap (256-byte stack buffer). Deliberately EXCLUDES
  // the per-entry DOS mod time/date so a byte-identical re-zip (same content,
  // new timestamps) keeps the same fingerprint. Any content or structural
  // change flips the CRC/size/offset mix. Used to detect a book replaced
  // in place at the same path (the cache key is path-derived). `visit`, when given, is shown each
  // entry whose name fits 255 bytes, with its stat, as the walk passes it.
  bool contentFingerprint(uint64_t* out,
                          void (*visit)(void* ctx, std::string_view name, const FileStatSlim& stat) = nullptr,
                          void* ctx = nullptr);

  // Stream every filename in the central directory to a callback without building
  // the in-memory stat cache: one walk through a 4 KB window (straight from the file,
  // a dozen card calls per entry, if the heap refuses the window), names in a 256-byte
  // stack buffer. Safe for large EPUBs (3000+ entries) where loadAllFileStatSlims()
  // would OOM. Callback signature: void(std::string_view filename); the view is valid
  // for the call only.
  template <typename F>
  bool streamCentralDirectoryNames(F&& callback) {
    using Callback = std::remove_reference_t<F>;
    return streamCentralDirectoryNames(
        [](void* ctx, const std::string_view name) { (*static_cast<Callback*>(ctx))(name); },
        const_cast<void*>(static_cast<const void*>(&callback)));
  }
  bool streamCentralDirectoryNames(void (*visit)(void* ctx, std::string_view name), void* ctx);
};
