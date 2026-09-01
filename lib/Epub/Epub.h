#pragma once

#include <Print.h>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/EpubImageManifest.h"
#include "Epub/ThumbResult.h"
#include "Epub/TocReliability.h"
#include "Epub/css/CssParser.h"

class ZipFile;
class BuildArena;  // lib/Memory — optional scratch for the extraction inflate ring

enum class OpfCacheMode { Disabled, Enabled };

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // the page-map.xml file (EPUB 2.01 printed page list, separate from NCX <pageList>)
  std::string pageMapItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // Image manifest (dimensions + ZIP stat for every image in the epub)
  std::unique_ptr<EpubImageManifest> imageManifest;
  // CSS files
  std::vector<std::string> cssFiles;
  mutable TocReliability tocReliability = TocReliability::Unknown;
  // Library-level option: app code can override this per-book instance.
  bool syntheticTocFallbackEnabled = false;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, OpfCacheMode cacheMode);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  bool parsePageMapFile() const;
  void parseCssFiles() const;
  void discoverCssFilesFromZip();

  // Session cache for the source ZIP's content fingerprint (one central-directory
  // walk per Epub instance; needsFirstOpenIndexing and load both consult it).
  mutable uint64_t zipFingerprint_ = 0;
  mutable bool zipFingerprintComputed_ = false;
  mutable bool zipFingerprintValid_ = false;
  // Session cache of the archive's EOCD details (raw fields, keeping ZipFile
  // forward-declared), harvested from the first successful ZipFile operation
  // and seeded into every later instance so the per-instance EOCD scan runs
  // once per book instead of once per item read.
  mutable uint32_t zipCentralDirOffset_ = 0;
  mutable uint16_t zipTotalEntries_ = 0;
  mutable bool zipDetailsCached_ = false;

  // Per-book cache of each spine entry's ZIP central-directory stat, resolved in ONE
  // central-directory walk (fillFileStats) the first time any spine stat is requested.
  // Without it, every spine open re-runs a linear central-directory scan — on a book that
  // interleaves ~1600 image entries with the XHTML spines (e.g. King's Avatar) that scan
  // walks hundreds-to-thousands of entries and dominated the compile (~215 ms/spine,
  // measured). Deque (not vector): ~14 B/spine, ~24 KB at 1732 spines — a vector would demand
  // that as one contiguous block and abort on a fragmented heap under -fno-exceptions.
  // Best-effort: on OOM the deque stays empty and getSpineItemStat falls back to a per-call
  // linear scan (correct, just slow). In-memory only — content.bin is the durable cache, so
  // this is cheap to rebuild and never persisted.
  mutable std::deque<ZipFile::FileStatSlim> spineStats_;
  mutable bool spineStatsResolved_ = false;
  mutable bool spineStatsUsable_ = false;
  void ensureSpineStats() const;

 public:
  // Resolve a spine entry's ZIP central-directory stat via the per-book cache (one
  // central-directory walk for the whole book, then O(1) per spine). Falls back to a single
  // linear loadFileStatSlim scan if the cache couldn't be built (OOM) or the spine wasn't
  // matched in the batch walk. Returns false only if the entry can't be found at all.
  bool getSpineItemStat(int spineIndex, ZipFile::FileStatSlim* out) const;

  // Seed a fresh ZipFile over this book with the cached EOCD details (no-op
  // until the first adopt). Public so Section's EntryReader benefits too.
  void primeZip(ZipFile& zip) const;
  // Harvest the details after a successful operation on `zip`.
  void adoptZipDetails(const ZipFile& zip) const;

 private:
  // Compute (once) the ZIP content fingerprint. False when the archive is
  // unreadable — callers then skip fingerprint-based invalidation entirely.
  bool computeZipFingerprint(uint64_t* out) const;
  // fingerprint.bin sidecar in the cache dir: the fingerprint the cache was
  // built from. The cache key is path-derived (see the constructor), so this is
  // what detects a different book dropped onto the same path.
  bool readStoredFingerprint(uint64_t* out) const;
  void writeStoredFingerprint(uint64_t fp) const;

  // Overlay a "<book>.opf" metadata sidecar onto the loaded coreMetadata, if one
  // exists. Called from every load path once coreMetadata is final — including
  // the cached ones, which is the point: the sidecar is never baked into
  // book.bin, so editing it takes effect on the next open rather than waiting
  // for the EPUB's own bytes to change. Cheap when absent (one Storage.exists).
  void applyMetadataSidecar() const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);

  // Lightweight load for COVER thumbnails only: populate the cover image reference WITHOUT building
  // the spine/TOC book.bin. Uses book.bin if it already exists; otherwise does a metadata-only OPF
  // parse (OpfCacheMode::Disabled — no manifest item index, no .items.bin), which for a huge book
  // (e.g. 1732 spines) avoids both the cost and the fragile large-index build of a full load(). After
  // this, only the cover path is valid (getCoverImageCachePath / generateThumbBmp / cover extraction)
  // — the spine/TOC accessors are NOT populated. Returns false if the cover reference can't be found.
  bool loadForCover();

  // Lightweight load for CATALOGUE METADATA only (title/author/series/seriesIndex/language), WITHOUT
  // building the spine/TOC book.bin. Same rationale and mechanism as loadForCover(): uses book.bin if
  // it already exists, otherwise does a metadata-only OPF parse (OpfCacheMode::Disabled — no manifest
  // item index, no .items.bin). After this, ONLY the metadata accessors above are valid; the spine/TOC
  // accessors and the cover path are NOT populated.
  //
  // Exists because scanning a folder for a series sequel must read series metadata from every EPUB in
  // it. Doing that with full load() calls meant one manifest/spine index build per candidate, run from
  // inside the reader with its heap still committed — the finished-book reboot on a large series folder
  // (issue #104). Cheap enough to call in a loop; still not free (one OPF parse per call), so callers
  // scanning many books should bound the candidate set first.
  bool loadForMetadata();

  // True when opening the book will trigger the (multi-second) first-open index
  // build inside load(): the spine/TOC cache (book.bin) or the compiled CSS rules
  // cache is missing. Cheap (only file-existence checks) so callers can decide
  // whether to show a progress popup before calling load().
  bool needsFirstOpenIndexing() const;

  // Path of the Calibre-style metadata sidecar for a book ("/Books/x.epub" ->
  // "/Books/x.opf"), or "" when there is none. Mirrors
  // ReaderActivity::sidecarCoverPath, and the same rule applies: a file beside
  // the book wins over what is embedded in it.
  static std::string metadataSidecarPath(const std::string& bookPath);
  // Sidecars above this are treated as not-a-metadata-file and ignored. A
  // Calibre metadata OPF is a couple of KB; the cap stops a stray large file
  // sharing the book's basename from being read into the heap.
  static constexpr size_t MAX_METADATA_SIDECAR_BYTES = 16384;

  bool clearCache(bool preserveThumbs = false) const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  const std::string& getSeries() const;
  const std::string& getSeriesIndex() const;
  const std::string& getDescription() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  std::string getCoverImageCachePath() const;
  // Returns the raw ZIP entry path for the cover image (empty if none).
  std::string getCoverItemHref() const;
  bool ensureCoverImageCached() const;
  // True if cover.img is already extracted and a supported format (no ZIP inflate).
  bool coverImageCachedValidOnly() const;
  // True if cover.img is on disk with a complete header that is NOT a format we decode.
  // Distinguishes "not extracted yet" (retry) from "extracted, undecodable" (give up):
  // coverImageCachedValidOnly() answers false for both, which would otherwise loop forever.
  bool coverImageCachedButUnsupported() const;
  // True if cover.img is usable now. allowExtract=false never inflates — it only reports
  // whether an already-cached cover.img exists, deferring extraction to a sliced session.
  bool coverImageCachedAndValid(bool allowExtract) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  std::string getThumbBmpPath(int width, int height) const;
  // Write the 0-byte "structurally absent" sentinel at a thumb path (see ThumbResult): the book
  // has no usable cover, so generateThumbBmp() stops retrying. Best-effort; ignores write errors.
  static void writeThumbSentinel(const std::string& thumbPath);
  // allowExtract=true: synchronously inflate the embedded cover.img from the ZIP if it
  // isn't cached yet (can stall for seconds on a large cover — fine for one-off callers
  // like the book-info / finished-book screens). allowExtract=false: decode ONLY an
  // already-cached cover.img and report TransientFail (no sentinel) if it's missing, so a
  // sliced extractor (ReaderActivity::beginCoverExtractSession) can do the inflate off the
  // hot loop path. The home cover loader passes false.
  //
  // A 0-byte sentinel is written ONLY for a StructurallyAbsent outcome (no cover item, or a
  // cover present but in an unsupported format). Every transient failure returns TransientFail
  // WITHOUT a sentinel so the next pass — or the next boot — retries; the caller (HomeActivity)
  // owns a session-scoped counter that promotes a repeatedly-transient book to a sentinel.
  ThumbResult generateThumbBmp(int height, bool allowExtract = true) const;
  ThumbResult generateThumbBmp(int width, int height, bool allowExtract = true) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  // Read up to maxBytes decompressed bytes from a ZIP entry — no SD write, header-only use.
  size_t readItemHeaderBytes(const std::string& itemHref, uint8_t* outBuf, size_t maxBytes) const;
  // Arena bytes one extractItemToFile() needs to keep its inflate ring off the heap:
  // a 1 KB read buffer plus the worst-case 32 KB ring (the entry size is not known until
  // open(), so budget the cap). Callers gate on image_scratch::canServe(this).
  // Staging buffer for an extract's SD writes. See BufferedExtractSink in the .cpp for the
  // device measurement behind it; 4 KB is eight sectors, which is where SdFat's multi-sector
  // path starts paying off, and small enough to come out of the heap when there is no arena.
  static constexpr size_t EXTRACT_WRITE_BUFFER_BYTES = 4 * 1024;
  // Inflate ring (<=32 KB) + the reader's 1 KB chunk + the write buffer above: what an extract
  // needs from the arena to run entirely off the heap.
  static constexpr size_t EXTRACT_ARENA_BYTES = 33 * 1024 + EXTRACT_WRITE_BUFFER_BYTES;

  // Extract a ZIP entry to a local SD file. Used for lazy image extraction at render time.
  //
  // arena (optional): carve the read buffer and the inflate ring out of it rather than the
  // heap. The ring is up to 32 KB CONTIGUOUS and is the first thing to fail on a fragmented
  // heap — measured on X4 at contig=13300, "Failed to init inflate reader" turned into
  // "Lazy extraction failed" and the page rendered with no image at all. Falls back to the
  // heap path if the arena cannot serve the reader, so a tight arena never turns a working
  // extraction into a failed one. See docs/memory-allocation-strategy.md §4 (class D).
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath, BuildArena* arena = nullptr) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  // Byte range of an item's raw data inside the EPUB, valid ONLY when the ZIP stores that entry
  // uncompressed (method 0). Lets a decoder read the entry in place instead of extracting it to
  // SD first -- worth ~3.4 s on an 857 KB cover, which is pure copying at ~255 KB/s.
  //
  // Returns false for a deflated entry, and it must: the bytes there are DEFLATE, not the file,
  // and streaming them into a decoder that inflates again would need two 32 KB uzlib rings live
  // at once -- more contiguous heap than this device has. Already-compressed formats (PNG, JPEG)
  // are commonly stored, which is exactly where the copy hurts most.
  bool getStoredItemRange(const std::string& itemHref, uint32_t* offset, uint32_t* size) const;

  // Drop the loadForCover() memo (see Epub.cpp). Call when a cover-loading burst is over; the
  // memo holds one book's metadata and is otherwise replaced as the next book is queried.
  static void clearCoverMetadataMemo();

 private:
  // Open the cover image for decoding straight out of the EPUB, positioned at its first byte,
  // when the ZIP stores that entry uncompressed. False for a deflated entry (or no cover), which
  // leaves the caller extracting cover.img as before. `offset` receives the entry's position, to
  // rewind to after sniffing the format.
  bool openStoredCoverInPlace(FsFile& out, uint32_t* offset) const;

 public:
  bool getSpineItemInflatedSize(int spineIndex, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  bool hasReliableToc() const;
  void setSyntheticTocFallbackEnabled(bool enabled) { syntheticTocFallbackEnabled = enabled; }
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  // Load (or build) the image manifest. Call after load() when images will be rendered.
  // Skipping this is valid for text-only or placeholder rendering modes.
  void loadImageManifest();
  // Non-const: section indexing resolves + caches new image dimensions through it.
  EpubImageManifest* getImageManifest() { return imageManifest.get(); }
  const EpubImageManifest* getImageManifest() const { return imageManifest.get(); }
  // Flush newly-resolved image dimensions to images.bin (no-op when nothing changed).
  void persistImageManifest() {
    if (imageManifest) imageManifest->persistIfDirty();
  }
  int resolveHrefToSpineIndex(const std::string& href) const;

  // Printed-page list (from NCX <pageList> / EPUB 3 nav page-list / EPUB 2.01 page-map.xml).
  // One entry per printed-page anchor: spine href + fragment id + visible label.
  struct PrintedPageEntry {
    std::string href;
    std::string anchor;
    std::string label;
  };
  // Printed-page navigation accessors, all reading <cachePath>/pagelist.bin. They STREAM the file
  // one entry at a time rather than materialising the list: the full list is ~72 B/entry (~200 KB
  // for a long book), and reserving that as a single contiguous block aborts the firmware (uncaught
  // bad_alloc, -fno-exceptions) when a menu is opened while the heap is fragmented by an in-flight
  // section build — the tag 2.05 "Confirm reboots on a huge chapter" crash. Numeric labels match
  // the reader's parsePrintedPageLabel rule (non-empty, all digits, <= 999999).

  // True when at least one entry has a numeric label. Short-circuits on the first match — used to
  // decide whether the reader menu offers "Go to printed page".
  bool hasNumericPrintedPages() const;
  // Fills [minLabel, maxLabel] with the numeric-label range (inclusive). Returns false and leaves
  // the outputs untouched when the book has no numeric labels.
  bool getPrintedPageLabelRange(int& minLabel, int& maxLabel) const;
  // Returns the first entry whose numeric label equals target (file order), or nullopt.
  std::optional<PrintedPageEntry> findPrintedPageByLabel(int target) const;

 private:
  // One extraction attempt. arena==nullptr is the historical heap path (readItemContentsToStream);
  // non-null streams through ZipFile::EntryReader, which carves its buffers from the arena.
  // Removes a partial destination file on failure, so the caller can simply retry.
  bool extractItemToFileOnce(const std::string& itemHref, const std::string& destPath, BuildArena* arena) const;
  // Drain one ZIP entry into `out` through an arena-backed EntryReader.
  bool readItemContentsToStreamWithArena(const std::string& itemHref, Print& out, BuildArena* arena) const;

  // Streams pagelist.bin at path into visit(href, anchor, label) per entry; visit returns false to
  // stop early. The single place that knows the on-disk format, so no caller reserves the whole
  // list. Silently no-ops when the file is absent or unreadable.
  static void streamPrintedPageEntries(
      const std::string& path,
      const std::function<bool(const std::string&, const std::string&, const std::string&)>& visit);
};
