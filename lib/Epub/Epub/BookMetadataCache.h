#pragma once

#include <BufferedFileIO.h>
#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "HashUtils.h"

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
    std::string series;
    std::string seriesIndex;
    std::string description;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool tocReliable;
  bool loaded;
  bool buildMode;

  FsFile bookFile;
  // Temp file handles during build
  FsFile spineFile;
  FsFile tocFile;
  // Buffered views over the temp files during their write phases (createSpineEntry /
  // createTocEntry stream one small record per manifest itemref / TOC entry — unbuffered,
  // each field is a separate ~1.5 ms FsFile call). Engaged by the begin*Pass methods,
  // flushed and dropped by the matching end*Pass. Degrade internally to pass-through on OOM.
  std::optional<serialization::BufferedFileWriter> spineWriter_;
  std::optional<serialization::BufferedFileWriter> tocWriter_;

  // Index for fast href→spineIndex lookup (used only for large EPUBs).
  // Deque, not vector: ~21 KB at 1732 spines, and a vector would demand that as one contiguous
  // block on a possibly-fragmented heap (bare operator new aborts under -fno-exceptions).
  // Deque's ~512-byte chunks remove the contiguity demand; its random-access iterators keep
  // std::sort / lower_bound working.
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::deque<SpineHrefIndexEntry> spineHrefIndex;
  bool useSpineHrefIndex = false;

  // Batch ZIP size lookup and fast spine-href index are always better when N is
  // larger than a handful — lower threshold so even moderate books (e.g. 105
  // spine items) take the O(n·log m) batch path instead of O(n·m) per-item scans.
  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 16;

  uint32_t writeSpineEntry(FsFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(FsFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(FsFile& file) const;
  TocEntry readTocEntry(FsFile& file) const;
  // Out-parameter overloads reuse the caller's string capacity inside hot
  // build loops, eliminating per-iteration std::string allocation churn that
  // would otherwise fragment the heap during book.bin construction.
  void readSpineEntry(FsFile& file, SpineEntry& out) const;
  void readTocEntry(FsFile& file, TocEntry& out) const;
  // Buffered counterparts for the build loops (identical wire format).
  uint32_t writeSpineEntry(serialization::BufferedFileWriter& out, const SpineEntry& entry) const;
  uint32_t writeTocEntry(serialization::BufferedFileWriter& out, const TocEntry& entry) const;
  void readSpineEntry(serialization::BufferedFileReader& in, SpineEntry& out) const;
  void readTocEntry(serialization::BufferedFileReader& in, TocEntry& out) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)),
        lutOffset(0),
        spineCount(0),
        tocCount(0),
        tocReliable(false),
        loaded(false),
        buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  bool resetTocPassOutput();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // True when the spine/TOC cache file (book.bin) already exists for this cachePath,
  // i.e. load() can read it instead of rebuilding. Cheap (a single Storage.exists).
  static bool cacheExists(const std::string& cachePath);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isTocReliable() const { return tocReliable; }
  bool isLoaded() const { return loaded; }
  // Mark the cache "loaded" after a COVER-ONLY OPF parse populated coreMetadata (coverItemHref etc.)
  // without building the spine/TOC book.bin. Lets cover extraction proceed (isLoaded() gate) while
  // spineCount/tocCount stay 0 — the caller must NOT use the spine/TOC accessors in this state.
  // See Epub::loadForCover(). This avoids the full 1732-spine book.bin build just to show a thumbnail.
  void markCoverMetadataLoaded() { loaded = true; }
};
