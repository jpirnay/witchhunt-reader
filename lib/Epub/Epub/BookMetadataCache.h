#pragma once

#include <BufferedFileIO.h>
#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "HashUtils.h"

class BuildArena;

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
    // Not stored in book.bin: its version would have to go up and every book be cached again. Set by
    // a fresh OPF parse and by the metadata sidecar; BookDetailsLookup::parse() loads past book.bin
    // to get them.
    std::string primaryAuthor;
    std::string authorSort;
    std::string isbn;
    std::string asin;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;
    // The item's ZIP central-directory stat, from buildBookBin's size walk: its chapter opens
    // without looking the entry up. In book.bin only, not in the spine temp file. A
    // zipLocalHeaderOffset of 0: the walk did not find it, so it is looked up by name.
    uint16_t zipMethod = 0;
    uint32_t zipCompressedSize = 0;
    uint32_t zipUncompressedSize = 0;
    uint32_t zipLocalHeaderOffset = 0;

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

  // TOC entries find their spine items a table-full at a time, one pass over the spine file per
  // table-full, when the parse that wrote them has ended (getTocMappedCount, endTocPass).
  //
  // They used to look theirs up one by one as the parse wrote them: through an index of every
  // spine href (16 B a spine item, one 27.7 KB block for King's Avatar's 1732), or, when the heap
  // refused that block, through a scan of the spine file resumed after the previous match --
  // about one read of the file for a TOC in reading order, a whole read per entry for any other.
  //
  // Now the entries' hrefs (FNV-1a 64 and length, place in the table) fill a PassTable -- in the
  // lent region when the TOC pass has one (TOC_RESOLVE_MIN_ENTRIES at least), else the heap's --
  // and one pass over the spine file, resumed after the previous table-full's last match, finds
  // them all. Their spine items go to toc.spine.tmp in TOC order; buildBookBin writes them into
  // book.bin's TOC records, which carry -1 until then. Matches trust (hash, length), as ZipFile's
  // batch lookups do; a spine item listed twice resolves to its first.
  struct TocTarget {
    uint64_t hrefHash;
    uint16_t hrefLen;
    uint16_t slot;       // place in the table-full, in TOC order
    int16_t spineIndex;  // -1 until a pass finds it
  };
  static constexpr size_t TOC_RESOLVE_MIN_ENTRIES = 128;
  static constexpr size_t TOC_SPINE_WRITER_BYTES = 512;
  // spineIndex is int16_t (-1 means "no match"), so spine indices must fit in a positive int16_t.
  // spineCount is uint16_t and would silently wrap past this; no real EPUB approaches it, but the
  // limit should be stated rather than latent.
  static constexpr int MAX_ADDRESSABLE_SPINES = 32767;
  FsFile tocSpineFile_;
  std::optional<serialization::BufferedFileWriter> tocSpineWriter_;
  uint16_t tocResolved_ = 0;        // entries whose spine item is in toc.spine.tmp
  uint32_t tocResolvedOffset_ = 0;  // where the first entry still to resolve starts in the TOC file
  int spineResumeIndex_ = 0;        // the spine record just past the last table-full's last match
  uint32_t spineResumeOffset_ = 0;
  BuildArena* tocScratch_ = nullptr;
  bool openTocOutput();
  void resolvePendingTocEntries();
  // The spine file through a buffered view, for the passes and for the filename fallback's scans.
  // A scan for one href starts after the previous match and wraps once: a TOC lists its chapters in
  // reading order, so the entry it wants is usually the next record.
  std::optional<serialization::BufferedFileReader> spineScanReader_;
  int tocScanNextIndex_ = 0;
  uint32_t tocScanNextOffset_ = 0;
  // TOC entries of this pass whose href matched a spine item (spineIndex >= 0). Build-pass only,
  // never written to book.bin: Epub::load compares it between the nav and the NCX.
  uint16_t tocMappedCount_ = 0;
  // The resumed scan: the spine item whose href is exactly `href`, else -1.
  int16_t findSpineByScan(const std::string& href);
  // Last resort for a TOC href no spine href matches exactly (see the definition).
  int16_t findSpineByFilename(const std::string& href);
  void resetTocFilenameFallback();
  // Filename fallback state, reset with every TOC parse. The folder a TOC href missed from and the
  // spine folder its filename was found in, tried first for the next miss from that folder.
  std::string tocMissDir_;
  std::string tocFoundDir_;
  bool tocFolderShiftKnown_ = false;
  std::string tocShiftedHref_;  // scratch for tocFoundDir_ + filename; keeps its capacity
  // Spine records the fallback may still read in full scans this parse: every one on a book of a
  // few hundred spine items, about ten whole reads of a 3000-item spine.
  uint32_t tocFilenameScanBudget_ = 0;
  static constexpr uint32_t TOC_FILENAME_SCAN_BUDGET = 32768;

  // The batch ZIP stat lookup is always better when N is larger than a handful — lower threshold
  // so even moderate books (e.g. 105 spine items) take the O(n·log m) batch path instead of
  // O(n·m) per-item scans.
  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 16;

  // A spine record in book.bin: the href, then BOOK_SPINE_FIXED_BYTES of cumulativeSize, tocIndex
  // and the ZIP stat, read in one call. The spine temp file's records stop after tocIndex.
  static constexpr uint32_t BOOK_SPINE_STAT_BYTES = 14;
  static constexpr uint32_t BOOK_SPINE_FIXED_BYTES = 6 + BOOK_SPINE_STAT_BYTES;
  // findSpineIndexByHref's read window.
  static constexpr size_t SPINE_SCAN_WINDOW_BYTES = 2048;

  uint32_t writeTocEntry(FsFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(FsFile& file) const;
  TocEntry readTocEntry(FsFile& file) const;
  // Out-parameter overloads reuse the caller's string capacity inside hot
  // build loops, eliminating per-iteration std::string allocation churn that
  // would otherwise fragment the heap during book.bin construction.
  void readSpineEntry(FsFile& file, SpineEntry& out) const;  // book.bin's record
  void readTocEntry(FsFile& file, TocEntry& out) const;
  // Buffered counterparts for the build loops. The spine ones are the temp file's record;
  // writeBookSpineEntry writes book.bin's.
  uint32_t writeSpineEntry(serialization::BufferedFileWriter& out, const SpineEntry& entry) const;
  uint32_t writeBookSpineEntry(serialization::BufferedFileWriter& out, const SpineEntry& entry) const;
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
  // `scratch`: a lent region the TOC entries' table may take a block of while their spine items are
  // looked up (between the parses' stream reads); returned as found.
  bool beginTocPass(BuildArena* scratch = nullptr);
  bool resetTocPassOutput();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes. `scratch`: a lent region (first-open indexing
  // lends the secondary framebuffer) that the spine size lookup's table and read window take a
  // block of while it runs, instead of the heap; returned as found.
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata, BuildArena* scratch = nullptr);

  // True when the spine/TOC cache file (book.bin) already exists for this cachePath,
  // i.e. load() can read it instead of rebuilding. Cheap (a single Storage.exists).
  static bool cacheExists(const std::string& cachePath);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  // The first spine item, in spine order, whose href is `target` (or, with `byFilename`, names the
  // same file: spine_href::match); -1 if none. The spine records are read in order through a
  // window: getSpineEntry() for each was half a dozen card calls a spine item. Read mode only.
  int findSpineIndexByHref(std::string_view target, bool byFilename);
  TocEntry getTocEntry(int index);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  // Build-pass only (see tocMappedCount_); 0 in read mode. Looks up the spine items of the entries
  // written since the last call.
  int getTocMappedCount();
  bool isTocReliable() const { return tocReliable; }
  bool isLoaded() const { return loaded; }
  // Mark the cache "loaded" after a METADATA-ONLY OPF parse populated coreMetadata (title, author,
  // series, coverItemHref, ...) without building the spine/TOC book.bin. Lets the isLoaded() gates
  // pass while spineCount/tocCount stay 0 — the caller must NOT use the spine/TOC accessors in this
  // state. Used by Epub::loadForCover() (avoids a full 1732-spine book.bin build just to show a
  // thumbnail) and Epub::loadForMetadata() (same, for a series-sequel folder scan).
  void markCoverMetadataLoaded() { loaded = true; }
};
