#include "Section.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#ifdef BENCH_EXTRACT_PROFILE
#include <esp_timer.h>
#endif

// The build's scratch buffers are carved from a preallocated BuildArena
// (docs/compiled-book-pipeline-plan.md Phase 2), device-validated 2026-07-18
// (X3: highWater=9216/10240, failedAlloc=0, build time neutral). The former
// per-site new/realloc path has been removed.
#include <BufferedFileIO.h>
#include <BuildArena.h>
#include <InflateReader.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include "Epub/css/CssParser.h"
#include "FootnotePreviews.h"
#include "Page.h"
#include "content/BlockStreamReader.h"  // content.bin read-back (Stage-2 replay)
#include "content/ContentBinWriter.h"   // content.bin write (Stage-1 persist)
#include "content/LayoutSink.h"         // complete type for the parser's unique_ptr<LayoutSink> member
#include "content/Stage1Config.h"       // EPUB_STAGE1 gate
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 60;  // bumped: TextBlock word data now serialized as one flat arena
                                              // (offset table + NUL-terminated text blob) instead of
                                              // length-prefixed strings and per-field arrays (v60)
                                              // v59: FontSizeLadder residual dead zone (±3% renders native) changes
                                              // near-rung block metrics from v58
                                              // (v58: block sizes snap to the FontSizeLadder, uniform spans fold;
                                              //  v57: sup/sub scaling moved into the per-word size channel)

namespace header {
constexpr uint32_t kVersion = 0;
constexpr uint32_t kFontId = kVersion + sizeof(uint8_t);
constexpr uint32_t kLineCompression = kFontId + sizeof(int);
constexpr uint32_t kExtraParagraphSpacing = kLineCompression + sizeof(float);
constexpr uint32_t kParagraphAlignment = kExtraParagraphSpacing + sizeof(bool);
constexpr uint32_t kViewportWidth = kParagraphAlignment + sizeof(uint8_t);
constexpr uint32_t kViewportHeight = kViewportWidth + sizeof(uint16_t);
constexpr uint32_t kHyphenationEnabled = kViewportHeight + sizeof(uint16_t);
constexpr uint32_t kEmbeddedStyle = kHyphenationEnabled + sizeof(bool);
constexpr uint32_t kBionicReadingEnabled = kEmbeddedStyle + sizeof(bool);
constexpr uint32_t kImageRendering = kBionicReadingEnabled + sizeof(bool);
constexpr uint32_t kParseComplete = kImageRendering + sizeof(uint8_t);
constexpr uint32_t kPageCount = kParseComplete + sizeof(bool);
constexpr uint32_t kPageLut = kPageCount + sizeof(uint16_t);
constexpr uint32_t kAnchorMap = kPageLut + sizeof(uint32_t);
constexpr uint32_t kPageBreakMap = kAnchorMap + sizeof(uint32_t);
constexpr uint32_t kParagraphLut = kPageBreakMap + sizeof(uint32_t);
constexpr uint32_t kSize = kParagraphLut + sizeof(uint32_t);
}  // namespace header

// On-disk paragraph LUT entry: u32 xhtmlByteOffset + u16 paragraphIndex + u16 listItemIndex.
// listItemIndex is the running <li> count at page-break time; together with
// paragraphIndex it lets KOReader-supplied <p>- and <li>-anchored XPaths snap to
// the exact page on download.
constexpr uint32_t PARAGRAPH_LUT_ENTRY_SIZE = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t);
inline uint32_t paragraphLutEntryOffset(uint32_t lutStart, uint16_t page) {
  return lutStart + page * PARAGRAPH_LUT_ENTRY_SIZE;
}
}  // namespace

#include <algorithm>

namespace {
constexpr uint32_t FNV_PRIME = 0x01000193;         // 16777619
constexpr uint32_t FNV_OFFSET_BASIS = 0x811C9DC5;  // 2166136261

// On constrained targets, parsing with embedded CSS adds heap pressure and increases
// parse truncation risk. Allow compile-time override for tuning.
//
// Floor sizing (measured, X3, 2026-06-11): since the sparse disk-backed CSS cache the
// resident cost is small — selector index ≈ ruleCount × 16 B (~4.6 KB for a measured
// 290-rule book, 24 KB at the 1500-rule cap), plus the bounded hot/negative caches
// (≤ ~32 KB absolute worst, typically ~10 KB). CssParser::resolveStyle additionally
// self-protects below CSS_MIN_FREE_HEAP_FOR_CSS (40 KB) by skipping disk lookups.
// The original 96 KB predates trusting those bounds and made background (Background-B)
// CSS builds impossible (~68 KB free while reading). Build telemetry (lowHeapSkips →
// Section::isCssLowHeapDegraded) lets callers discard a degraded result instead.
#ifndef SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES
#define SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES (56 * 1024)
#endif

// Contiguous floor: the only sizable contiguous CSS allocation is the selector index
// (CssParser::CSS_INDEX_BYTES_PER_RULE = 8 B/rule, i.e. ~2.3 KB at 290 rules and
// 12 KB at the 1500-rule cap), which heapAllowsEmbeddedStyle() adds dynamically from
// the actual rule count. This define is just the baseline below the dynamic term. The
// old static 36 KB predated the sparse disk-backed cache and refused builds the heap
// could easily serve (measured X3: contig 26.6 KB post-indexing, small actual need).
#ifndef SCT_EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES
#define SCT_EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES (12 * 1024)
#endif

constexpr uint32_t EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES = SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES;
constexpr uint32_t EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES = SCT_EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES;

// Visitor-feed chunk for phase (b): the granularity at which runBuildParse checks its time budget
// between visitor writes. Kept small so a build slice yields promptly and input stays responsive.
constexpr size_t PARSE_CHUNK_BYTES = 1024;

// Physical SD read buffer for phase (b): the temp XHTML is read through a BufferedFileReader sized
// here, so pulling PARSE_CHUNK_BYTES logical chunks costs one SD read per this many bytes, not per
// chunk. A 570 KB single spine (Small Gods) otherwise spent ~1.1 s in 570×1 KB reads. Heap-backed
// (nothrow, degrades to direct reads on OOM), independent of the 10 KB build arena.
constexpr size_t PARSE_READ_BUFFER_BYTES = 8192;

// Output chunk for phase (a) extraction (inflate -> temp SD file). Extraction is SD-write bound with
// no layout between writes, so a larger buffer means far fewer, larger (multi-sector) SD writes for a
// big single-file spine. Applied ONLY after the inflate ring is allocated (see runBuildParse), so it
// never competes with the ~32 KB ring for a contiguous block, and it is shrunk back to
// PARSE_CHUNK_BYTES before phase (b). The grow is best-effort: on failure the 1 KB buffer is kept.
constexpr size_t EXTRACT_CHUNK_BYTES = 8192;

// Parse-scratch arena budget (Phase 2 of docs/compiled-book-pipeline-plan.md).
// One up-front allocation backing the build's scratch buffers: chunk feed buffer
// (PARSE_CHUNK_BYTES base + EXTRACT_CHUNK_BYTES extraction scope) + alignment.
constexpr size_t SCT_PARSE_ARENA_BYTES = 10 * 1024;

// Bump when preview expansion semantics change. This is hashed only for preview-enabled
// variants, leaving the much more common preview-off section caches untouched.
constexpr uint8_t INLINE_FOOTNOTE_PREVIEW_LAYOUT_VERSION = 2;

uint32_t fnv1a(const uint8_t* data, size_t length) {
  uint32_t hash = FNV_OFFSET_BASIS;
  for (size_t i = 0; i < length; ++i) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}
}  // namespace

uint32_t Section::calculatePropertyHash(int fontId, float lineCompression, bool extraParagraphSpacing,
                                        uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                                        bool hyphenationEnabled, bool embeddedStyle, bool bionicReadingEnabled,
                                        bool inlineFootnotePreviews, uint8_t imageRendering) {
  uint8_t buffer[64];
  size_t offset = 0;

  auto append = [&](const void* ptr, size_t size) {
    memcpy(buffer + offset, ptr, size);
    offset += size;
  };

  append(&fontId, sizeof(fontId));
  append(&lineCompression, sizeof(lineCompression));
  append(&extraParagraphSpacing, sizeof(extraParagraphSpacing));
  append(&paragraphAlignment, sizeof(paragraphAlignment));
  append(&viewportWidth, sizeof(viewportWidth));
  append(&viewportHeight, sizeof(viewportHeight));
  append(&hyphenationEnabled, sizeof(hyphenationEnabled));
  append(&embeddedStyle, sizeof(embeddedStyle));
  append(&bionicReadingEnabled, sizeof(bionicReadingEnabled));
  append(&inlineFootnotePreviews, sizeof(inlineFootnotePreviews));
  if (inlineFootnotePreviews) {
    append(&INLINE_FOOTNOTE_PREVIEW_LAYOUT_VERSION, sizeof(INLINE_FOOTNOTE_PREVIEW_LAYOUT_VERSION));
  }
  append(&imageRendering, sizeof(imageRendering));

  return fnv1a(buffer, offset);
}

std::string Section::getSectionFilePath(uint32_t propertyHash) const {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d_%08x", spineIndex, propertyHash);
  return epub->getCachePath() + "/sections/" + buf + ".bin";
}

std::string Section::getSectionHtmlCachePath() const {
  char buf[32];
  snprintf(buf, sizeof(buf), "html_%d", spineIndex);
  return epub->getCachePath() + "/sections/" + buf + ".bin";
}

void Section::removeHtmlCache() const {
  const std::string p = getSectionHtmlCachePath();
  if (!p.empty() && Storage.exists(p.c_str())) Storage.remove(p.c_str());
}

std::string Section::getImageBasePath(uint32_t propertyHash) const {
  char buf[32];
  snprintf(buf, sizeof(buf), "img_%d_%08x_", spineIndex, propertyHash);
  return epub->getCachePath() + "/" + buf;
}

struct SectionVariant {
  std::string filename;
  uint16_t date;
  uint16_t time;
};

void Section::evictOldVariants() const {
  constexpr size_t MAX_VARIANTS = 5;  // keep up to 5 recent variants to avoid re-parsing

  std::string sectionsDir = epub->getCachePath() + "/sections";
  auto files = Storage.listFiles(sectionsDir.c_str(), 100);

  std::vector<SectionVariant> variants;

  // Find all cache variants belonging to this spineIndex
  char prefix[16];
  snprintf(prefix, sizeof(prefix), "%d_", spineIndex);

  for (const auto& file : files) {
    if (!file.startsWith(prefix) || !file.endsWith(".bin")) continue;
    uint16_t md = 0, mt = 0;
    HalFile hf = Storage.open((sectionsDir + "/" + file.c_str()).c_str(), O_RDONLY);
    if (hf) hf.getModifyDateTime(&md, &mt);
    variants.push_back({file.c_str(), md, mt});
  }

  if (variants.size() <= MAX_VARIANTS) return;

  // Sort descending by modified date and time
  std::sort(variants.begin(), variants.end(), [](const SectionVariant& a, const SectionVariant& b) {
    if (a.date != b.date) return a.date > b.date;
    return a.time > b.time;
  });

  // Delete everything after MAX_VARIANTS limit
  for (size_t i = MAX_VARIANTS; i < variants.size(); ++i) {
    std::string targetPath = sectionsDir + "/" + variants[i].filename;
    Storage.remove(targetPath.c_str());
    LOG_DBG("SCT", "Evicted old section cache: %s", targetPath.c_str());

    // Extract the hash to also clean up associated images
    // Filename format: spineIndex_hash.bin
    size_t underscore = variants[i].filename.find('_');
    size_t dot = variants[i].filename.find('.');
    if (underscore != std::string::npos && dot != std::string::npos && dot > underscore) {
      std::string hashStr = variants[i].filename.substr(underscore + 1, dot - underscore - 1);
      uint32_t parsedHash = strtoul(hashStr.c_str(), nullptr, 16);
      if (parsedHash != 0 || hashStr == "00000000") {
        std::string imgBasePath = getImageBasePath(parsedHash);
        // Find and delete matching images
        auto rootFiles = Storage.listFiles(epub->getCachePath().c_str(), 100);
        size_t lastSlash = imgBasePath.find_last_of('/');
        std::string imgPrefix = (lastSlash != std::string::npos) ? imgBasePath.substr(lastSlash + 1) : imgBasePath;

        for (const auto& rf : rootFiles) {
          if (rf.startsWith(imgPrefix.c_str())) {
            Storage.remove((epub->getCachePath() + "/" + rf.c_str()).c_str());
            LOG_DBG("SCT", "Evicted old image cache: %s", rf.c_str());
          }
        }
      }
    }
  }
}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  if (pageCount % 10 == 0) {
    LOG_DBG("SCT", "Page %d processed", pageCount);
  }

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const bool bionicReadingEnabled,
                                     const uint8_t imageRendering) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(header::kSize == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                     sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) +
                                     sizeof(viewportWidth) + sizeof(viewportHeight) + sizeof(hyphenationEnabled) +
                                     sizeof(embeddedStyle) + sizeof(bionicReadingEnabled) + sizeof(imageRendering) +
                                     sizeof(bool) + sizeof(pageCount) + sizeof(uint32_t) + sizeof(uint32_t) +
                                     sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, bionicReadingEnabled);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, false);      // Placeholder for parseComplete (patched later)
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file,
                          static_cast<uint32_t>(0));  // Placeholder for page break label map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const bool bionicReadingEnabled, const bool inlineFootnotePreviews,
                              const uint8_t imageRendering) {
  truncatedCache = false;
  embeddedStyleFallback = false;
  uint32_t propertyHash = calculatePropertyHash(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment,
                                                viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle,
                                                bionicReadingEnabled, inlineFootnotePreviews, imageRendering);
  filePath = getSectionFilePath(propertyHash);

  bool usingEmbeddedStyleFallback = false;
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    // Fallback: allow loading a no-CSS cache variant when embedded CSS is enabled.
    if (embeddedStyle) {
      const uint32_t fallbackHash = calculatePropertyHash(
          fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
          hyphenationEnabled, false, bionicReadingEnabled, inlineFootnotePreviews, imageRendering);
      const std::string fallbackPath = getSectionFilePath(fallbackHash);
      if (Storage.openFileForRead("SCT", fallbackPath, file)) {
        filePath = fallbackPath;
        usingEmbeddedStyleFallback = true;
        embeddedStyleFallback = true;
        LOG_INF("SCT", "Using no-CSS section cache fallback: %s", filePath.c_str());
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();  // closes file before removal
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    bool fileBionicReadingEnabled;
    uint8_t fileImageRendering;
    bool fileParseComplete;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileBionicReadingEnabled);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileParseComplete);

    const bool embeddedStyleMatches =
        (embeddedStyle == fileEmbeddedStyle) || (usingEmbeddedStyleFallback && !fileEmbeddedStyle);
    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || !embeddedStyleMatches ||
        bionicReadingEnabled != fileBionicReadingEnabled || imageRendering != fileImageRendering) {
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();  // closes file before removal
      return false;
    }

    truncatedCache = !fileParseComplete;
  }

  serialization::readPod(file, pageCount);

  // Sanity check: same upper bound used by TextBlock::deserialize for word count
  if (pageCount > 10000) {
    LOG_ERR("SCT", "Deserialization failed: page count %u exceeds maximum", pageCount);
    clearCache();
    return false;
  }

  // Load LUT into memory (file is now positioned at the lutOffset field)
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  lut.resize(pageCount);
  if (!file.seek(lutOffset)) {
    LOG_ERR("SCT", "Deserialization failed: seek to LUT offset %u failed", lutOffset);
    clearCache();
    return false;
  }
  for (uint32_t& pos : lut) {
    serialization::readPod(file, pos);
    if (pos < header::kSize || pos >= lutOffset) {
      LOG_ERR("SCT", "Deserialization failed: LUT entry %u out of range [%u, %u)", pos, header::kSize, lutOffset);
      clearCache();
      return false;
    }
  }
  // Build TOC boundaries by scanning anchor data from the still-open file,
  // matching only the TOC anchors we need (avoids loading all anchors into memory).
  buildTocBoundariesFromFile(file);
  buildPageBreakLabelsFromFile(file);

  // File is intentionally left open; subsequent loadPageFromSectionFile() calls
  // seek within this handle instead of re-opening the file each time.
  LOG_DBG("SCT", "Deserialization succeeded: %d pages, LUT cached", pageCount);
  return true;
}

bool Section::clearCache() {
  file.close();  // Must be closed before removal on FAT32
  lut.clear();
  tocBoundaries.clear();
  pageCount = 0;
  currentPage = 0;
  truncatedCache = false;

  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

// Live state of an in-progress section build. Holds exactly the locals that span build
// phases; see docs/epubreader-control-flow-refactor.md §2.7. Held by Section as a
// unique_ptr so its address (and thus visitor's completePageFn lut capture) is stable
// across phase calls and, later, across loop ticks.
struct Section::BuildState {
  BuildParams params;
  std::function<void(int)> progressFn;
  uint32_t propertyHash = 0;
  std::string localPath;
  std::string contentBase;
  std::string imageBasePath;
  size_t inflatedSize = 0;
  // The spine entry's ZIP central-directory stat, resolved once in setup via Epub's per-book
  // cache (one central-dir walk for the whole book) so the parse's EntryReader::open skips the
  // per-spine linear central-directory scan that dominated the compile. Valid when statValid.
  ZipFile::FileStatSlim spineStat = {};
  bool statValid = false;
  CssParser* cssParser = nullptr;
  std::vector<uint32_t> lut;
  std::unique_ptr<ChapterHtmlSlimParser> visitor;
  // Live ZIP-side state of the parse, created on the first runBuildParse call and
  // released the moment the compressed stream is exhausted. zip must outlive reader
  // (reader holds a reference to it). chunkBuf (PARSE_CHUNK_BYTES) is the feed buffer —
  // heap because it must survive across slices. The inflate ring inside the reader is
  // sized to the entry (≤32 KB).
  // Active scratch arena: either ownedArena (heap-backed, resident builds) or an
  // external region supplied by the caller — the borrowed secondary framebuffer
  // (see Section::setExternalBuildScratch). Set once by initArena(). Declared
  // BEFORE zip/reader: members destroy in reverse order, and the reader's
  // teardown releases into the arena, so owned storage must outlive both.
  std::unique_ptr<BuildArena> ownedArena;
  BuildArena* arena = nullptr;
  // External region is large enough to also host the reader's readBuf + ring
  // directly (no separate entry-sized zipArena needed).
  bool sharedZipScope = false;
  bool initArena(BuildArena* external) {
    if (external && external->valid()) {
      external->reset();  // fresh build: reclaim any previous build's cursor
      arena = external;
    } else {
      ownedArena = makeUniqueNoThrow<BuildArena>(SCT_PARSE_ARENA_BYTES);
      if (!ownedArena || !ownedArena->valid()) return false;
      arena = ownedArena.get();
    }
    return true;
  }
  // Entry-sized arena hosting the EntryReader's readBuf + inflate ring during
  // phase (a) only, when the main arena is too small to host them (heap-backed
  // resident builds). Created at reader-open time — the ring is entry-sized, so
  // a fixed prealloc would demand a ~44 KB contiguous block that resident
  // builds (contig ~29 KB while reading) cannot provide.
  std::unique_ptr<BuildArena> zipArena;
  // Peak use of the (destroyed-by-log-time) zipArena, for the done-telemetry.
  uint32_t zipArenaHighWater = 0;
  std::unique_ptr<ZipFile> zip;
  std::unique_ptr<ZipFile::EntryReader> reader;
  // Raw view of the feed buffer, backed by the build arena. Managed exclusively
  // through the lifecycle methods below.
  uint8_t* chunkBuf = nullptr;
  BuildArena::Block chunkBlock;
  BuildArena::Block extractGrowBlock;
  uint8_t* baseChunkBuf = nullptr;
  void dropZipArena() {
    if (!zipArena) return;
    zipArenaHighWater = static_cast<uint32_t>(zipArena->highWater());
    zipArena.reset();
  }
  // Capacity of chunkBuf during phase (a). Grown to EXTRACT_CHUNK_BYTES once the inflate ring is
  // allocated (see runBuildParse); stays PARSE_CHUNK_BYTES if that grow fails or on the reused-HTML
  // path. Phase (b) always feeds PARSE_CHUNK_BYTES and chunkBuf is shrunk back before it runs.
  size_t extractCap = PARSE_CHUNK_BYTES;

  // --- chunk feed-buffer lifecycle ---
  // Base allocation (PARSE_CHUNK_BYTES). False on OOM (arena invalid).
  bool allocChunk() {
    chunkBlock = arena->reserveBlock();
    chunkBuf = static_cast<uint8_t*>(arena->alloc(PARSE_CHUNK_BYTES));
    baseChunkBuf = chunkBuf;
    extractCap = PARSE_CHUNK_BYTES;
    return chunkBuf != nullptr;
  }
  // Best-effort grow for phase (a): on success chunkBuf/extractCap switch to the
  // larger buffer; on failure the base buffer is kept (extraction just runs slower).
  void growChunkForExtract() {
    // Preallocated arena: the grow consumes no heap, so no free-heap gate needed.
    extractGrowBlock = arena->reserveBlock();
    if (auto* grown = static_cast<uint8_t*>(arena->alloc(EXTRACT_CHUNK_BYTES))) {
      chunkBuf = grown;
      extractCap = EXTRACT_CHUNK_BYTES;
    } else {
      arena->release(extractGrowBlock);
    }
  }
  // Shrink back to PARSE_CHUNK_BYTES before phase (b) by releasing the extraction
  // scope; fails only if the block is no longer the newest live reservation.
  bool shrinkChunkAfterExtract() {
    if (extractCap == PARSE_CHUNK_BYTES) return true;
    if (!arena->release(extractGrowBlock)) return false;
    chunkBuf = baseChunkBuf;
    extractCap = PARSE_CHUNK_BYTES;
    return true;
  }
  void dropChunk() {
    if (extractGrowBlock.valid()) arena->release(extractGrowBlock);
    arena->release(chunkBlock);
    baseChunkBuf = nullptr;
    chunkBuf = nullptr;
  }
  ~BuildState() {
    if (extractGrowBlock.valid()) arena->release(extractGrowBlock);
    reader.reset();
    if (chunkBlock.valid()) arena->release(chunkBlock);
  }
  bool parseStarted = false;
  // Two-phase sliced parse, latched at the first parse call (so a build started in the
  // background stays on this path when the foreground resumes it with budget 0):
  //   (a) extract — EntryReader slices inflate the entry to tempPath on SD, then ALL
  //       ZIP state (ring + scratch + handles) is released;
  //   (b) parse — slices read tempPath and feed the visitor with no ZIP memory resident.
  // This keeps the inflate ring and the parser's layout working set from ever being
  // live at the same time, which is what made background builds impossible on ~68 KB
  // of reading heap. Blocking builds (budget 0 from the start) stream directly instead:
  // they run with the secondary framebuffer released and don't need the split or the
  // extra SD round-trip.
  bool useTempExtract = false;
  bool extractDone = false;
  // True when tempPath is the book-keyed HTML cache opened for READ (reused, not produced this
  // build): phase (a) is skipped and the cache is kept on cleanup rather than deleted.
  bool reusedHtml = false;
  FsFile tempFile;
  std::string tempPath;
  size_t tempBytesFed = 0;
  // Phase (b) buffers the temp-file reads: the parser is fed in small logical chunks, but pulling
  // them straight off SdFat is one physical read per chunk — a 570 KB single spine (Small Gods)
  // spent ~1.1 s in 570×1 KB reads. This batches the underlying SD reads to the buffer size while
  // the parser still gets small chunks. Lazily constructed on the first phase-(b) read and lives in
  // BuildState so a sliced parse keeps its window across yields. Heap-backed + nothrow (degrades to
  // pass-through on OOM), independent of the 10 KB build arena.
  std::optional<serialization::BufferedFileReader> tempReader;
  // Disk-backed book-level footnote preview lookup (footnotes.bin), opened at setup when
  // the build wants inline previews. Owned here so the visitor's non-owning pointer stays
  // valid across build slices.
  std::unique_ptr<FootnotePreviews::Lookup> footnotePreviewLookup;
  // Property hash of the params as requested by the caller, before any low-heap CSS
  // downgrade. Lets stepSectionBuild detect a stale partial build (variant changed)
  // without a heap-forced no-CSS build reading as a mismatch against its own request.
  uint32_t requestedHash = 0;
  // Parse-result flags, set by runBuildParse and consumed by runBuildFinalize.
  bool streamOk = false;
  bool finalizeOk = false;
  bool parserStreamOk = false;
  // Coarse timing for the summary log; per-phase wall clock (parseMs accumulates across slices).
  uint32_t totalStartMs = 0;
  uint32_t setupMs = 0;
  uint32_t parseMs = 0;
};

// Live state of an in-progress content.bin READ-BACK (Increment E consumer), owned across
// stepReadBackFromContentBin() calls. Holds exactly what must survive a mid-spine yield: the open
// content.bin (its BlockStreamReader cursor), the LayoutSink (accumulating pages/anchors/labels),
// the page-offset LUT, the replay cursor, and the property hash so a variant change can be detected.
// The section file is `Section::file` (opened at setup, streamed to by the sink's completePageFn).
struct Section::ReadBackState {
  compiled::SpineReplayCursor cursor;          // per-spine replay position
  FsFile binFile;                             // content.bin, kept open (reader points into it)
  compiled::BlockStreamReader reader;
  std::unique_ptr<compiled::LayoutSink> sink;  // heap-owned: its completePageFn captures &lut
  std::vector<uint32_t> lut;                    // page byte-offsets, grown as pages complete
  uint32_t propertyHash = 0;                    // variant key; a change discards the partial
  uint32_t startMs = 0;
};

// Out-of-line (see header): both need the complete BuildState/ReadBackState types, and the dtor must
// not leave a partially written cache file behind when a Section dies with a build in
// flight (book close, activity exit) — its header was never patched with offsets.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub), spineIndex(spineIndex), renderer(renderer) {}
Section::~Section() {
  abortSectionBuild();
  abortReadBack();
}

Section::BuildPhaseResult Section::runBuildSetup(BuildState& st) {
  const BuildParams& p = st.params;
  st.propertyHash = calculatePropertyHash(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment,
                                          p.viewportWidth, p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle,
                                          p.bionicReadingEnabled, p.inlineFootnotePreviews, p.imageRendering);
  filePath = getSectionFilePath(st.propertyHash);

  st.localPath = epub->getSpineItem(spineIndex).href;
  LOG_INF("SCT", "createSectionFile spine=%d start: %s (free=%lu)", spineIndex, st.localPath.c_str(),
          esp_get_free_heap_size());

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Get inflated size up-front so the parser can choose progress granularity. Resolve the
  // spine's ZIP stat once here (via Epub's per-book cache) and reuse it for the parse's
  // EntryReader::open, so we scan the central directory once per book instead of once per
  // spine open (the compile's dominant cost, measured). uncompressedSize doubles as the
  // inflated size — no separate getSpineItemInflatedSize scan.
  const uint32_t phaseSetupStart = millis();
  st.inflatedSize = 0;
  st.statValid = epub->getSpineItemStat(spineIndex, &st.spineStat);
  if (st.statValid) {
    st.inflatedSize = st.spineStat.uncompressedSize;
  } else if (!epub->getSpineItemInflatedSize(spineIndex, &st.inflatedSize)) {
    LOG_ERR("SCT", "Failed to get inflated size for %s", st.localPath.c_str());
    return BuildPhaseResult::Failed;
  }

  // Reset build state — createSectionFile may be called on a Section that previously
  // loaded a cache (e.g. fallback no-CSS file). pageCount must start at 0 so that
  // onPageComplete() numbering and paragraphLutPerPage stay in lockstep.
  // Guard the close: on a cold build (no pre-existing section cache to load) `file` was never
  // opened, and HalFile::close() asserts on a null impl. Only reached via the whole-book
  // compileBookToContentBin path — the reader's incremental path always has an open file here.
  if (file.isOpen()) file.close();
  pageCount = 0;
  this->lut.clear();
  cssLowHeapDegraded_ = false;

  // Content-only compile (fresh reader's background content.bin compiler): produce NO section-cache
  // file at all — the parser tees blocks straight to content.bin and emits no pages, so a section
  // file would be an empty 0-page artifact that clutters the cache (thousands for a big book). Skip
  // opening + header; writeSectionTail is likewise skipped in finalize.
  if (!contentBinContentOnly_) {
    if (!Storage.openFileForWrite("SCT", filePath, file)) {
      return BuildPhaseResult::Failed;
    }
    writeSectionFileHeader(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
                           p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle, p.bionicReadingEnabled,
                           p.imageRendering);
  }
  st.lut.clear();

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = st.localPath.find_last_of('/');
  st.contentBase = (lastSlash != std::string::npos) ? st.localPath.substr(0, lastSlash + 1) : "";
  st.imageBasePath = getImageBasePath(st.propertyHash);

  st.cssParser = nullptr;
  if (p.embeddedStyle) {
    st.cssParser = epub->getCssParser();
    if (st.cssParser) {
      // Phase-2: a build running in the BORROWED secondary framebuffer (external arena) has
      // ~52 KB less heap than a released build, so resolve CSS out of the arena instead of the
      // heap — resident {hash,style} ruleset when it fits, else an arena-backed index — with the
      // hot cache off and a lower floor, so the resolver doesn't self-degrade and force a
      // released rebuild. Set deterministically (not just when external) so the shared per-epub
      // parser never carries a stale lean flag into an owned/heap-backed build.
      const bool externalArena = st.arena && st.arena != st.ownedArena.get();
      st.cssParser->setIndexArena(externalArena ? st.arena : nullptr);
      st.cssParser->setLeanResolve(externalArena);
      if (!st.cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
      st.cssParser->resetResolveStats();
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  // Load printed-page list entries (NCX <pageList> or EPUB 3 nav page-list) for this
  // chapter's href, if any. Format: u16 count, then per entry: writeString(href),
  // writeString(anchor), writeString(label).
  std::vector<std::pair<std::string, std::string>> externalPageBreakAnchors;
  {
    const auto pageListPath = epub->getCachePath() + "/pagelist.bin";
    FsFile pageListFile;
    if (Storage.exists(pageListPath.c_str()) && Storage.openFileForRead("SCT", pageListPath, pageListFile)) {
      uint16_t count = 0;
      serialization::readPod(pageListFile, count);
      for (uint16_t i = 0; i < count; i++) {
        std::string href, anchor, label;
        serialization::readString(pageListFile, href);
        serialization::readString(pageListFile, anchor);
        serialization::readString(pageListFile, label);
        if (href == st.localPath) {
          externalPageBreakAnchors.emplace_back(std::move(anchor), std::move(label));
        }
      }
      pageListFile.close();
    }
  }

  // The visitor's completePageFn captures &st.lut: BuildState lives in a stable unique_ptr,
  // so this reference is valid for the visitor's whole lifetime, including across slices.
  st.visitor = std::make_unique<ChapterHtmlSlimParser>(
      epub, renderer, p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
      p.viewportHeight, p.hyphenationEnabled, p.bionicReadingEnabled,
      [this, &st](std::unique_ptr<Page> page) { st.lut.emplace_back(this->onPageComplete(std::move(page))); },
      p.embeddedStyle, st.contentBase, st.imageBasePath, p.imageRendering, std::move(tocAnchors), st.progressFn,
      st.cssParser, epub->getImageManifest());
  st.visitor->setExternalPageBreakAnchors(std::move(externalPageBreakAnchors));
  st.visitor->setFontSizeLadder(p.fontSizeLadder);
  // null unless a Stage-1 compile is driving this build. Tee mode (Increment F) emits content.bin
  // ALONGSIDE the section-cache pages from one walk; content-only mode replaces the layout sink.
  if (stage1SinkTee_) {
    st.visitor->setStage1TeeSink(stage1Sink_);
  } else {
    st.visitor->setStage1Sink(stage1Sink_);
  }
  Hyphenator::setPreferredLanguage(epub->getLanguage());

  // Inline footnote previews come from the book-level footnotes.bin gathered up front
  // (foreground, EpubReaderActivity). Missing/empty cache: build proceeds without
  // previews — the reader guarantees the gather ran before any preview-enabled build.
  if (p.inlineFootnotePreviews) {
    st.footnotePreviewLookup = makeUniqueNoThrow<FootnotePreviews::Lookup>();
    if (st.footnotePreviewLookup && st.footnotePreviewLookup->open(epub->getCachePath(), epub.get(), spineIndex)) {
      st.visitor->setInlineFootnotePreviews(st.footnotePreviewLookup.get());
    } else {
      LOG_DBG("SCT", "Footnote preview cache unavailable for spine %d; building without previews", spineIndex);
      st.footnotePreviewLookup.reset();
    }
  }

  if (!st.visitor->setup(st.inflatedSize)) {
    LOG_ERR("SCT", "Failed to set up chapter parser");
    file.close();
    Storage.remove(filePath.c_str());
    if (st.cssParser) {
      st.cssParser->clear();
    }
    return BuildPhaseResult::Failed;
  }
  st.setupMs = millis() - phaseSetupStart;
  LOG_INF("SCT", "createSectionFile spine=%d setup done: %ums (inflatedSize=%u free=%lu)", spineIndex, st.setupMs,
          static_cast<uint32_t>(st.inflatedSize), esp_get_free_heap_size());
  return BuildPhaseResult::Ok;
}

Section::BuildPhaseResult Section::runBuildParse(BuildState& st, const uint32_t budgetMs) {
  const uint32_t sliceStart = millis();
  const auto overBudget = [&] { return budgetMs != 0 && millis() - sliceStart >= budgetMs; };
  const auto yieldSlice = [&] {
    st.parseMs += millis() - sliceStart;
    return BuildPhaseResult::More;
  };
  bool streamFailed = false;

  if (!st.parseStarted) {
    st.parseStarted = true;
    // The parse always feeds the visitor from a temp SD file (phase b); chunkBuf is its read
    // buffer, so allocate it whether the cached HTML is reused or produced now.
    if (!st.allocChunk()) {
      LOG_ERR("SCT", "Failed to allocate parse chunk buffer (free=%lu)", esp_get_free_heap_size());
      streamFailed = true;
    }

    // Book-keyed unzipped-HTML cache (adapted from crosspoint-reader PR #2452 by GitHub user
    // itsthisjustin): the spine's
    // inflated XHTML keyed on the spine alone, NOT on render properties. If a valid one exists,
    // read it directly and skip the (multi-second on a big spine) ZIP inflation — the win across
    // settings changes and rebuilds. A size mismatch means it is stale/partial (e.g. interrupted
    // by power loss); drop it and re-inflate.
    const std::string htmlCachePath = getSectionHtmlCachePath();
    if (!streamFailed && st.inflatedSize > 0 && Storage.exists(htmlCachePath.c_str()) &&
        Storage.openFileForRead("SCT", htmlCachePath, st.tempFile)) {
      if (st.tempFile.size() == st.inflatedSize) {
        st.useTempExtract = true;
        st.extractDone = true;  // phase (a) inflation skipped
        st.reusedHtml = true;   // keep the cache on cleanup rather than deleting it
        st.tempPath = htmlCachePath;
        LOG_INF("SCT", "createSectionFile spine=%d reusing cached HTML (%u bytes, free=%lu)", spineIndex,
                static_cast<uint32_t>(st.inflatedSize), esp_get_free_heap_size());
      } else {
        st.tempFile.close();
        Storage.remove(htmlCachePath.c_str());
      }
    }

    // No usable cache: inflate the entry to the book-level HTML cache (phase a) so it is produced
    // once and reused thereafter. Always two-phase now (even the blocking path) so every build
    // populates the cache; the inflate ring is released before parsing, so the blocking path —
    // which runs with the secondary framebuffer released — stays within heap.
    if (!streamFailed && !st.reusedHtml) {
      st.useTempExtract = true;
      // Heap, not stack: must survive across slices. Ring (≤32 KB, sized to the entry) +
      // PARSE_CHUNK_BYTES scratch — net-neutral vs the old one-shot path.
      st.zip.reset(new (std::nothrow) ZipFile(epub->getPath()));
      if (st.zip) {
        epub->primeZip(*st.zip);  // reuse the book's cached EOCD details (skip the rescan)
        const size_t zipArenaBytes =
            PARSE_CHUNK_BYTES + InflateReader::ringSizeFor(st.inflatedSize) + 2 * alignof(std::max_align_t);
        // External region (borrowed framebuffer) with room for the ZIP scope:
        // host readBuf + ring directly in the main arena — the whole extract
        // phase then touches the heap not at all.
        if (st.arena && st.arena != st.ownedArena.get() &&
            st.arena->capacity() - st.arena->used() >= zipArenaBytes + EXTRACT_CHUNK_BYTES) {
          st.sharedZipScope = true;
          st.reader.reset(new (std::nothrow) ZipFile::EntryReader(*st.zip, PARSE_CHUNK_BYTES, st.arena));
        } else {
          // Heap-backed: one entry-sized block for the reader's readBuf + ring,
          // alive only through phase (a) — a single scope the reader releases.
          st.zipArena = makeUniqueNoThrow<BuildArena>(zipArenaBytes);
          if (st.zipArena && st.zipArena->valid()) {
            st.reader.reset(new (std::nothrow) ZipFile::EntryReader(*st.zip, PARSE_CHUNK_BYTES, st.zipArena.get()));
          } else {
            LOG_ERR("SCT", "Failed to allocate ZIP arena (%u bytes, free=%lu)", static_cast<uint32_t>(zipArenaBytes),
                    esp_get_free_heap_size());
            st.zipArena.reset();
          }
        }
      }
      if (!st.reader) {
        LOG_ERR("SCT", "Failed to allocate entry reader (%u bytes scratch, free=%lu)",
                static_cast<uint32_t>(PARSE_CHUNK_BYTES), esp_get_free_heap_size());
        streamFailed = true;
      } else {
        // Prefer the stat resolved in setup (skips the per-spine central-directory scan);
        // fall back to the by-name open when the cache was unavailable for this spine.
        const bool opened = st.statValid ? st.reader->open(st.spineStat)
                                         : st.reader->open(FsHelpers::normalisePath(st.localPath).c_str());
        if (!opened) {
          streamFailed = true;  // EntryReader::open already logged the cause
        } else {
          // The inflate ring is now allocated (open() took its ~32 KB contiguous block first). Only
          // now grow the extraction feed buffer, so a bigger buffer draws from the remainder and can
          // never starve the ring (the OOM->blocking regression this replaced). Best-effort: on
          // failure the 1 KB buffer is kept and extraction runs slower. Shrunk back after phase (a).
          // Arena scoping lives inside the method.
          st.growChunkForExtract();
        }
      }
      if (!streamFailed) {
        st.tempPath = htmlCachePath;
        if (!Storage.openFileForWrite("SCT", st.tempPath, st.tempFile)) {
          streamFailed = true;
        }
      }
    }
  }

  // Phase (a), sliced path only: inflate the entry to a temp SD file, then release all
  // ZIP state. Keeps the ring and the parser's layout working set temporally disjoint.
  if (!streamFailed && st.useTempExtract && !st.extractDone) {
    bool done = false;
#ifdef BENCH_EXTRACT_PROFILE
    LOG_INF("SCT", "spine=%d EXTRACTPROF prelude=%ums (zip open + entry seek + file open)", spineIndex,
            static_cast<uint32_t>(millis() - sliceStart));
    uint32_t inflateUs = 0, writeUs = 0;
#endif
    while (!done) {
      size_t produced = 0;
#ifdef BENCH_EXTRACT_PROFILE
      const int64_t ti = esp_timer_get_time();
#endif
      if (!st.reader->step(st.chunkBuf, st.extractCap, &produced, &done)) {
        streamFailed = true;
        break;
      }
#ifdef BENCH_EXTRACT_PROFILE
      inflateUs += static_cast<uint32_t>(esp_timer_get_time() - ti);
      const int64_t tw = esp_timer_get_time();
#endif
      if (produced > 0 && st.tempFile.write(st.chunkBuf, produced) != produced) {
        LOG_ERR("SCT", "Failed to write extracted XHTML to %s", st.tempPath.c_str());
        streamFailed = true;
        break;
      }
#ifdef BENCH_EXTRACT_PROFILE
      writeUs += static_cast<uint32_t>(esp_timer_get_time() - tw);
#endif
      if (!done && overBudget()) {
        return yieldSlice();
      }
    }
#ifdef BENCH_EXTRACT_PROFILE
    LOG_INF("SCT", "spine=%d EXTRACTPROF inflate=%ums sd_write=%ums", spineIndex, inflateUs / 1000, writeUs / 1000);
    const int64_t tClose = esp_timer_get_time();
#endif
    if (!streamFailed && st.reader->bytesProduced() != st.inflatedSize) {
      LOG_ERR("SCT", "Decompressed size mismatch (expected %u, got %u)", static_cast<uint32_t>(st.inflatedSize),
              static_cast<uint32_t>(st.reader->bytesProduced()));
      streamFailed = true;
    }
    // Release the extraction scope before closing the nested ZIP block, so the
    // reader's arena block stays the newest live reservation (LIFO release order).
    if (!st.shrinkChunkAfterExtract()) {
      LOG_ERR("SCT", "Failed to release extraction buffer block");
      streamFailed = true;
    }
    st.reader.reset();
    st.zip.reset();
    st.dropZipArena();
    st.tempFile.flush();
    st.tempFile.close();
    st.extractDone = true;
    if (!streamFailed) {
      if (!Storage.openFileForRead("SCT", st.tempPath, st.tempFile)) {
        streamFailed = true;
      } else {
#ifdef BENCH_EXTRACT_PROFILE
        LOG_INF("SCT", "spine=%d EXTRACTPROF fileops=%ums (flush/close/reopen)", spineIndex,
                static_cast<uint32_t>((esp_timer_get_time() - tClose) / 1000));
#endif
        LOG_INF("SCT", "createSectionFile spine=%d extracted %u bytes to temp (free=%lu)", spineIndex,
                static_cast<uint32_t>(st.inflatedSize), esp_get_free_heap_size());
        if (overBudget()) {
          return yieldSlice();
        }
      }
    }
  }

  // Phase (b): feed the visitor — from the temp file (sliced path, no ZIP state live)
  // or straight from the inflate stream (blocking path). Inline footnote previews need
  // no prepass here: the visitor resolves them against the book-level footnotes.bin
  // lookup opened in runBuildSetup.
  if (!streamFailed) {
    if (st.useTempExtract) {
      // Batch the physical SD reads (8 KB) while still feeding the parser PARSE_CHUNK_BYTES at a
      // time — cuts a giant spine's per-1 KB-read cost by ~8x. Lazily built + owned in BuildState so
      // the window survives parse slices. On OOM the reader degrades to pass-through (direct reads).
      if (!st.tempReader) {
        st.tempReader.emplace(st.tempFile, PARSE_READ_BUFFER_BYTES);
      }
#ifdef BENCH_EXTRACT_PROFILE
      uint32_t sdReadUs = 0, visitorUs = 0;
#endif
      while (true) {
#ifdef BENCH_EXTRACT_PROFILE
        const int64_t tr = esp_timer_get_time();
#endif
        const size_t n = st.tempReader->readSome(st.chunkBuf, PARSE_CHUNK_BYTES);
#ifdef BENCH_EXTRACT_PROFILE
        sdReadUs += static_cast<uint32_t>(esp_timer_get_time() - tr);
#endif
        if (n == 0) {
          break;
        }
        st.tempBytesFed += n;
        // A short write means the parser failed mid-stream (it returns 0 after an
        // internal error) — same abort the one-shot readFileToStream path performed.
#ifdef BENCH_EXTRACT_PROFILE
        const int64_t tv = esp_timer_get_time();
#endif
        const size_t wrote = st.visitor->write(st.chunkBuf, n);
#ifdef BENCH_EXTRACT_PROFILE
        visitorUs += static_cast<uint32_t>(esp_timer_get_time() - tv);
#endif
        if (wrote != n) {
          streamFailed = true;
          break;
        }
        if (overBudget()) {
          return yieldSlice();
        }
      }
#ifdef BENCH_EXTRACT_PROFILE
      LOG_INF("SCT", "spine=%d PARSEPROF sd_read=%ums visitor=%ums", spineIndex, sdReadUs / 1000, visitorUs / 1000);
#endif
      if (!streamFailed && st.tempBytesFed != st.inflatedSize) {
        LOG_ERR("SCT", "Extracted size mismatch (expected %u, fed %u)", static_cast<uint32_t>(st.inflatedSize),
                static_cast<uint32_t>(st.tempBytesFed));
        streamFailed = true;
      }
    } else {
      bool done = false;
      while (!done) {
        size_t produced = 0;
        if (!st.reader->step(st.chunkBuf, PARSE_CHUNK_BYTES, &produced, &done)) {
          streamFailed = true;
          break;
        }
        if (produced > 0 && st.visitor->write(st.chunkBuf, produced) != produced) {
          streamFailed = true;
          break;
        }
      }
      if (!streamFailed && st.reader->bytesProduced() != st.inflatedSize) {
        LOG_ERR("SCT", "Decompressed size mismatch (expected %u, got %u)", static_cast<uint32_t>(st.inflatedSize),
                static_cast<uint32_t>(st.reader->bytesProduced()));
        streamFailed = true;
      }
    }
  }

  // Stream exhausted or failed — wrap up the phase exactly as the one-shot path did.
  // Release the ZIP-side state (no-ops on the sliced path, which dropped it after
  // extraction) and the temp file before the visitor finalizes.
  if (!st.shrinkChunkAfterExtract()) streamFailed = true;
  st.reader.reset();
  st.zip.reset();
  st.dropZipArena();
  st.dropChunk();
  // Drop the buffered reader before closing tempFile — it holds a reference to it.
  st.tempReader.reset();
  if (st.tempFile) {
    st.tempFile.close();
  }
  if (!st.tempPath.empty()) {
    // tempPath is the book-keyed HTML cache. Keep it so later rebuilds skip inflation: when we
    // reused it, and when we produced a complete one (stream OK). Only delete a cache WE produced
    // if the stream failed — it may be partial/corrupt (a leftover is otherwise caught by the
    // size check on the next reuse, so it would just be re-inflated).
    if (!st.reusedHtml && streamFailed) {
      Storage.remove(st.tempPath.c_str());
    }
    st.tempPath.clear();
  }
  st.streamOk = !streamFailed;
#ifdef BENCH_EXTRACT_PROFILE
  const int64_t tFin = esp_timer_get_time();
#endif
  st.finalizeOk = st.visitor->finalize();
#ifdef BENCH_EXTRACT_PROFILE
  LOG_INF("SCT", "spine=%d EXTRACTPROF finalize=%ums", spineIndex,
          static_cast<uint32_t>((esp_timer_get_time() - tFin) / 1000));
#endif
  st.parserStreamOk = st.visitor->streamSucceeded();
  if (st.cssParser) {
    st.cssParser->logResolveStats(st.localPath.c_str());
    // Latch before Finalize clears the parser (which resets its stats): lowHeapSkips
    // means pages were cached with styles silently missing.
    cssLowHeapDegraded_ = st.cssParser->getResolveStats().lowHeapSkips > 0;
  }
  st.parseMs += millis() - sliceStart;
  LOG_INF("SCT", "createSectionFile spine=%d parse done: %ums pages=%u (stream=%d finalize=%d parser=%d free=%lu)",
          spineIndex, st.parseMs, pageCount, st.streamOk ? 1 : 0, st.finalizeOk ? 1 : 0, st.parserStreamOk ? 1 : 0,
          esp_get_free_heap_size());
  return BuildPhaseResult::Ok;
}

template <typename LutEntry>
bool Section::writeSectionTail(const std::vector<uint32_t>& lut,
                               const std::vector<std::pair<std::string, uint16_t>>& anchors,
                               const std::vector<std::pair<uint16_t, std::string>>& pageBreakLabelsIn,
                               const std::vector<LutEntry>& paragraphLut, uint16_t pageCountIn, bool parseComplete) {
  const auto fail = [&](const char* msg) {
    LOG_ERR("SCT", "%s", msg);
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  };

  // Page-offset LUT. 0 = a failed onPageComplete; 0xFFFFFFFF = a broken handle — neither may reach
  // the file, where it would only surface later as a failed seek at page-load time.
  const uint32_t lutOffset = file.position();
  for (const uint32_t& pos : lut) {
    if (pos == 0 || pos == UINT32_MAX) return fail("Failed to write LUT due to invalid page positions");
    serialization::writePod(file, pos);
  }

  // Anchor → page map (TOC + footnote targets).
  const uint32_t anchorMapOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  // Printed-page label map (EPUB pagebreak markers).
  const uint32_t pageBreakMapOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(pageBreakLabelsIn.size()));
  for (const auto& [page, label] : pageBreakLabelsIn) {
    serialization::writePod(file, page);
    serialization::writeString(file, label);
  }

  // Per-page paragraph LUT: {xhtmlByteOffset(u32), paragraphIndex(u16), listItemIndex(u16)}.
  const uint32_t paragraphLutOffset = file.position();
  if (paragraphLut.size() != static_cast<size_t>(pageCountIn)) {
    return fail("Paragraph LUT size mismatch vs pageCount");
  }
  serialization::writePod(file, static_cast<uint16_t>(paragraphLut.size()));
  for (const auto& entry : paragraphLut) {
    serialization::writePod(file, entry.xhtmlByteOffset);
    serialization::writePod(file, entry.paragraphIndex);
    serialization::writePod(file, entry.listItemIndex);
  }

  // Patch the header with final parseComplete/pageCount + section offsets.
  const size_t headerPatchStart = header::kParseComplete;
  if (!file.seek(headerPatchStart)) return fail("Failed to seek to section header patch offset");
  serialization::writePod(file, parseComplete);
  serialization::writePod(file, pageCountIn);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, pageBreakMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  file.flush();

  const size_t expectedEnd = headerPatchStart + sizeof(parseComplete) + sizeof(pageCountIn) + sizeof(lutOffset) +
                             sizeof(anchorMapOffset) + sizeof(pageBreakMapOffset) + sizeof(paragraphLutOffset);
  if (file.position() != expectedEnd) return fail("Section header patch write failed");
  return true;
}

// Explicit instantiations: the parse finalize uses ChapterHtmlSlimParser::ParagraphLutEntry; the
// content.bin read-back build uses compiled::LayoutLutEntry. Both have xhtmlByteOffset /
// paragraphIndex / listItemIndex.
template bool Section::writeSectionTail<ChapterHtmlSlimParser::ParagraphLutEntry>(
    const std::vector<uint32_t>&, const std::vector<std::pair<std::string, uint16_t>>&,
    const std::vector<std::pair<uint16_t, std::string>>&,
    const std::vector<ChapterHtmlSlimParser::ParagraphLutEntry>&, uint16_t, bool);
template bool Section::writeSectionTail<compiled::LayoutLutEntry>(
    const std::vector<uint32_t>&, const std::vector<std::pair<std::string, uint16_t>>&,
    const std::vector<std::pair<uint16_t, std::string>>&, const std::vector<compiled::LayoutLutEntry>&, uint16_t, bool);

Section::BuildPhaseResult Section::runBuildFinalize(BuildState& st) {
  ChapterHtmlSlimParser& visitor = *st.visitor;
  const bool parseComplete = st.streamOk && st.finalizeOk && st.parserStreamOk;
  bool success = parseComplete;
  const bool hasParsedPages = pageCount > 0;
  // streamMs is no longer a separate phase (SD-write of temp file is gone); keep the
  // log breakdown stable by reporting it as 0.
  constexpr uint32_t streamMs = 0;

  const uint32_t phaseFinalizeStart = millis();
  if (!success) {
    // If parsing fails mid-stream due low memory but some pages were already serialized,
    // keep the partial section cache so the chapter remains readable instead of failing hard.
    if (hasParsedPages) {
      LOG_ERR("SCT", "Parse incomplete; keeping partial section cache with %u pages (stream=%d finalize=%d parser=%d)",
              pageCount, st.streamOk ? 1 : 0, st.finalizeOk ? 1 : 0, st.parserStreamOk ? 1 : 0);
      success = true;
    } else if (st.params.embeddedStyle) {
      LOG_ERR("SCT",
              "Parse failed with embedded CSS enabled; retrying section creation with embeddedStyle=0 "
              "(stream=%d finalize=%d parser=%d)",
              st.streamOk ? 1 : 0, st.finalizeOk ? 1 : 0, st.parserStreamOk ? 1 : 0);
      if (file.isOpen()) file.close();  // content-only mode never opened it
      Storage.remove(filePath.c_str());
      if (st.cssParser) {
        st.cssParser->clear();
      }
      // Ask the entry function to restart the whole build with embeddedStyle disabled.
      return BuildPhaseResult::RetryNoCss;
    } else {
      LOG_ERR("SCT", "Failed to parse XML and build pages (stream=%d finalize=%d parser=%d)", st.streamOk ? 1 : 0,
              st.finalizeOk ? 1 : 0, st.parserStreamOk ? 1 : 0);
      if (file.isOpen()) file.close();  // content-only mode never opened it
      Storage.remove(filePath.c_str());
      if (st.cssParser) {
        st.cssParser->clear();
      }
      return BuildPhaseResult::Failed;
    }
  }
  const uint32_t fileSize = static_cast<uint32_t>(st.inflatedSize);

  // The section-file tail + header patch — shared with the content.bin read-back build. Skipped in
  // content-only mode: no section file was opened (see runBuildSetup), so there is nothing to tail.
  const auto& anchors = visitor.getAnchors();
  if (!contentBinContentOnly_ &&
      !writeSectionTail(st.lut, anchors, visitor.getPageBreakLabels(), visitor.getParagraphLutPerPage(),
                        static_cast<uint16_t>(pageCount), parseComplete)) {
    return BuildPhaseResult::Failed;  // writeSectionTail already closed+removed the file
  }

  if (st.cssParser) {
    st.cssParser->clearCaches();
  }

  buildTocBoundaries(anchors);

  // Populate in-memory pageBreakLabels from the just-completed parse so the status bar
  // can show printed-page labels without having to reload the section cache from disk.
  // Without this, labels only appear after a subsequent open (via buildPageBreakLabelsFromFile).
  this->pageBreakLabels.clear();
  for (const auto& entry : visitor.getPageBreakLabels()) {
    this->pageBreakLabels.emplace_back(entry.first, entry.second);
  }

  // Content-only mode has no section file: skip the close + reopen-for-read entirely (the LUT is
  // empty, there are no pages to serve). content.bin was written via the tee; commit happens in the
  // caller on a clean Done.
  if (!contentBinContentOnly_) {
    file.close();
    // Cache the LUT in memory and open the file for reading so that
    // subsequent loadPageFromSectionFile() calls can seek directly without re-opening.
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      LOG_ERR("SCT", "Failed to open section file for reading after creation");
      return BuildPhaseResult::Failed;
    }
  }
  truncatedCache = !parseComplete;
  this->lut = std::move(st.lut);
  const uint32_t finalizeMs = millis() - phaseFinalizeStart;
  const uint32_t totalMs = millis() - st.totalStartMs;
  LOG_INF("SCT",
          "createSectionFile spine=%d done: total=%ums (stream=%u setup=%u parse=%u finalize=%u) pages=%u bytes=%u",
          spineIndex, totalMs, streamMs, st.setupMs, st.parseMs, finalizeMs, pageCount, fileSize);
  // Arena telemetry: how much of the budgets a build actually used. zipHW is the
  // entry-sized phase-(a) arena's peak (0 = reused-HTML path, no ZIP state).
  LOG_INF("SCT", "createSectionFile spine=%d arena: cap=%u highWater=%u failedAlloc=%u zipHW=%u", spineIndex,
          st.arena ? static_cast<uint32_t>(st.arena->capacity()) : 0,
          st.arena ? static_cast<uint32_t>(st.arena->highWater()) : 0,
          st.arena ? static_cast<uint32_t>(st.arena->failedAllocSize()) : 0, st.zipArenaHighWater);
  return BuildPhaseResult::Done;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const bool bionicReadingEnabled, const bool inlineFootnotePreviews,
                                const uint8_t imageRendering, const std::function<void(int)>& progressFn,
                                const bool skipEviction, const FontSizeLadder& fontSizeLadder) {
  if (!skipEviction) {
    evictOldVariants();
  }

  BuildParams params;
  params.fontId = fontId;
  params.lineCompression = lineCompression;
  params.extraParagraphSpacing = extraParagraphSpacing;
  params.paragraphAlignment = paragraphAlignment;
  params.viewportWidth = viewportWidth;
  params.viewportHeight = viewportHeight;
  params.hyphenationEnabled = hyphenationEnabled;
  params.embeddedStyle = embeddedStyle;
  params.bionicReadingEnabled = bionicReadingEnabled;
  params.inlineFootnotePreviews = inlineFootnotePreviews;
  params.imageRendering = imageRendering;
  params.fontSizeLadder = fontSizeLadder;

  // Run-to-completion path: pump the incremental build with no time budget. budgetMs == 0
  // never yields mid-parse, so the only More this loop sees is the restart after a
  // RetryNoCss downgrade — foreground behaviour is unchanged. If a background build for
  // this exact variant is already in flight, the pump resumes it instead of starting over.
  while (true) {
    switch (stepSectionBuild(params, /*budgetMs=*/0, progressFn, skipEviction)) {
      case BuildStep::Done:
        return true;
      case BuildStep::Failed:
        return false;
      default:
        continue;
    }
  }
}

void Section::abortReadBack() {
  if (!readBackState_) return;
  // Close+remove the half-written section file so a partial read-back never survives as a valid
  // cache. The BlockStreamReader/LayoutSink/binFile drop with the state.
  file.close();
  if (!filePath.empty()) Storage.remove(filePath.c_str());
  pageCount = 0;
  this->lut.clear();
  readBackState_.reset();
}

bool Section::buildSectionFromContentBin(const BuildParams& p, const bool skipEviction) {
#if !EPUB_STAGE1
  (void)p;
  (void)skipEviction;
  return false;
#else
  // Run-to-completion wrapper: pump the sliced read-back with no budget (never yields mid-spine).
  for (;;) {
    switch (stepReadBackFromContentBin(p, /*budgetMs=*/0, skipEviction)) {
      case ReadBackStep::Done:
        return true;
      case ReadBackStep::Failed:
      case ReadBackStep::NotAvailable:
        return false;
      case ReadBackStep::More:
        continue;  // budgetMs==0 only yields on the initial setup boundary, if at all
    }
  }
#endif
}

// Number of logical blocks replayed per inner slice before the ms-budget is re-checked. Small enough
// that one slice's layout work overshoots the budget by at most a handful of blocks, large enough to
// keep the per-slice overhead (the millis() check) negligible. Mirrors the parser's ~1 KB feed chunk.
static constexpr uint32_t kReadBackBlocksPerSlice = 8;

Section::ReadBackStep Section::stepReadBackFromContentBin(const BuildParams& p, const uint32_t budgetMs,
                                                         const bool skipEviction) {
#if !EPUB_STAGE1
  (void)p;
  (void)budgetMs;
  (void)skipEviction;
  return ReadBackStep::NotAvailable;
#else
  const uint32_t propertyHash = calculatePropertyHash(
      p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth, p.viewportHeight,
      p.hyphenationEnabled, p.embeddedStyle, p.bionicReadingEnabled, p.inlineFootnotePreviews, p.imageRendering);

  // A live read-back for a DIFFERENT variant (settings changed mid-build) is stale — discard it.
  if (readBackState_ && readBackState_->propertyHash != propertyHash) abortReadBack();

  // ---- Setup (first call for this variant) ----
  if (!readBackState_) {
    const std::string binPath = epub->getCachePath() + "/content.bin";
    auto st = std::make_unique<ReadBackState>();
    st->propertyHash = propertyHash;
    st->startMs = millis();
    if (!Storage.openFileForRead("SCT", binPath, st->binFile)) return ReadBackStep::NotAvailable;
    if (!st->reader.open(st->binFile)) {
      LOG_INF("SCT", "content.bin present but unreadable/stale for spine %d; falling back to parse", spineIndex);
      return ReadBackStep::NotAvailable;
    }
    uint64_t bookFp = 0;
    if (st->reader.fingerprint() != 0 && epub->zipContentFingerprint(&bookFp) &&
        bookFp != st->reader.fingerprint()) {
      LOG_INF("SCT", "content.bin fingerprint mismatch for spine %d; falling back to parse", spineIndex);
      return ReadBackStep::NotAvailable;
    }
    // Spine must be present AND committed (frontier: a producer may not have reached it yet).
    if (static_cast<uint32_t>(spineIndex) >= st->reader.spineCount()) return ReadBackStep::NotAvailable;
    if (!st->reader.spineAvailable(static_cast<uint32_t>(spineIndex))) return ReadBackStep::NotAvailable;

    if (!skipEviction) evictOldVariants();

    filePath = getSectionFilePath(propertyHash);
    { const auto sectionsDir = epub->getCachePath() + "/sections"; Storage.mkdir(sectionsDir.c_str()); }
    file.close();
    pageCount = 0;
    this->lut.clear();
    if (!Storage.openFileForWrite("SCT", filePath, file)) return ReadBackStep::Failed;
    writeSectionFileHeader(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
                           p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle, p.bionicReadingEnabled,
                           p.imageRendering);

    // Drive a LayoutSink from the streaming reader; pages stream to the section file via
    // onPageComplete (identical to the parser's completePageFn), so RAM stays ~one page + one block.
    compiled::LayoutParams lp;
    lp.fontId = p.fontId;
    lp.lineCompression = p.lineCompression;
    lp.extraParagraphSpacing = p.extraParagraphSpacing;
    lp.paragraphAlignment = p.paragraphAlignment;
    lp.viewportWidth = p.viewportWidth;
    lp.viewportHeight = p.viewportHeight;
    lp.hyphenationEnabled = p.hyphenationEnabled;
    lp.bionicReadingEnabled = p.bionicReadingEnabled;
    lp.embeddedStyle = p.embeddedStyle;
    lp.fontSizeLadder = p.fontSizeLadder;
    lp.imageBasePath = getImageBasePath(propertyHash);
    lp.epubFilePath = epub->getPath();
    Hyphenator::setPreferredLanguage(epub->getLanguage());

    ReadBackState* raw = st.get();  // stable across ticks (heap-owned); the sink captures raw->lut
    st->sink = std::make_unique<compiled::LayoutSink>(
        renderer, std::move(lp),
        [this, raw](std::unique_ptr<Page> page) { raw->lut.emplace_back(onPageComplete(std::move(page))); });
    if (!compiled::replaySpineBegin(st->reader, static_cast<uint32_t>(spineIndex), st->cursor)) {
      LOG_ERR("SCT", "content.bin openSpine failed for spine %d; falling back to parse", spineIndex);
      file.close();
      Storage.remove(filePath.c_str());
      return ReadBackStep::NotAvailable;
    }
    readBackState_ = std::move(st);
  }

  // ---- Replay slices ----
  ReadBackState& st = *readBackState_;
  const uint32_t sliceStart = millis();
  const auto overBudget = [&] { return budgetMs != 0 && millis() - sliceStart >= budgetMs; };
  while (compiled::replaySpineStep(st.reader, *st.sink, kReadBackBlocksPerSlice, st.cursor)) {
    if (overBudget()) return ReadBackStep::More;  // more blocks remain; resume next call
  }
  if (!st.cursor.ok) {
    LOG_ERR("SCT", "content.bin replay failed for spine %d; removing partial + falling back", spineIndex);
    abortReadBack();
    return ReadBackStep::Failed;
  }

  // ---- Finalize (spine fully replayed: onSpineEnd fired) ----
  const auto& anchors = st.sink->anchors();
  if (!writeSectionTail(st.lut, anchors, st.sink->pageBreakLabels(), st.sink->paragraphLutPerPage(),
                        static_cast<uint16_t>(pageCount), /*parseComplete=*/true)) {
    readBackState_.reset();  // writeSectionTail already closed+removed the file
    return ReadBackStep::Failed;
  }
  buildTocBoundaries(anchors);
  this->pageBreakLabels.clear();
  for (const auto& e : st.sink->pageBreakLabels()) this->pageBreakLabels.emplace_back(e.first, e.second);

  file.close();
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    readBackState_.reset();
    return ReadBackStep::Failed;
  }
  truncatedCache = false;
  this->lut = std::move(st.lut);
  LOG_INF("SCT", "buildSectionFromContentBin spine=%d done: %ums pages=%u (read-back, no parse)", spineIndex,
          millis() - st.startMs, pageCount);
  readBackState_.reset();
  return ReadBackStep::Done;
#endif
}

bool Section::compileBookToContentBin(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer,
                                      const BuildParams& params) {
#if !EPUB_STAGE1
  (void)epub;
  (void)renderer;
  (void)params;
  return false;
#else
  const uint32_t t0 = millis();
  const int spineCount = epub->getSpineItemsCount();
  const std::string binPath = epub->getCachePath() + "/content.bin";
  { const auto sectionsDir = epub->getCachePath(); Storage.mkdir(sectionsDir.c_str()); }

  uint64_t fingerprint = 0;
  epub->zipContentFingerprint(&fingerprint);

  FsFile binFile;
  if (!Storage.openFileForWrite("SCT", binPath, binFile)) {
    LOG_ERR("SCT", "compileBookToContentBin: cannot open %s for write", binPath.c_str());
    return false;
  }
  compiled::ContentBinWriter writer;
  if (!writer.begin(binFile, static_cast<uint32_t>(spineCount), fingerprint)) {
    binFile.close();
    Storage.remove(binPath.c_str());
    return false;
  }

  bool ok = true;
  for (int i = 0; i < spineCount && ok; ++i) {
    writer.beginSpine();
    Section section(epub, i, renderer);
    section.setStage1Sink(&writer);  // content-only compile: parser drives the writer, no pages
    // Run-to-completion (budgetMs=0). skipEviction: we are not touching the section variant cache.
    if (!section.createSectionFile(params.fontId, params.lineCompression, params.extraParagraphSpacing,
                                   params.paragraphAlignment, params.viewportWidth, params.viewportHeight,
                                   params.hyphenationEnabled, params.embeddedStyle, params.bionicReadingEnabled,
                                   params.inlineFootnotePreviews, params.imageRendering, {}, /*skipEviction=*/true,
                                   params.fontSizeLadder)) {
      LOG_ERR("SCT", "compileBookToContentBin: spine %d compile failed", i);
      ok = false;
    }
  }
  ok = ok && writer.finish();
  binFile.close();
  if (!ok) {
    Storage.remove(binPath.c_str());
    return false;
  }
  LOG_INF("SCT", "compileBookToContentBin: %d spines -> content.bin in %ums (free=%lu)", spineCount, millis() - t0,
          esp_get_free_heap_size());
  return true;
#endif
}

bool Section::heapAllowsEmbeddedStyle(const size_t cssRuleCount) {
  // Contig need is dominated by the selector index vector (CSS_INDEX_BYTES_PER_RULE)
  // plus slack for file buffers; everything else (hot/negative caches) allocates in
  // small nodes. Deliberately silent: callers decide whether a refusal is worth a
  // log line — background gates re-check this often and must not spam.
  const uint32_t requiredContig =
      std::max<uint32_t>(EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES,
                         static_cast<uint32_t>(cssRuleCount * CssParser::CSS_INDEX_BYTES_PER_RULE) + 8 * 1024);
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  return freeHeap >= EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES && contigHeap >= requiredContig;
}

bool Section::startBuild(const BuildParams& params, const std::function<void(int)>& progressFn,
                         const uint32_t requestedHash) {
  BuildParams p = params;
  const CssParser* css = epub->getCssParser();
  if (p.embeddedStyle && !heapAllowsEmbeddedStyle(css ? css->ruleCount() : 0)) {
    LOG_INF("SCT", "Low heap for embedded CSS (free=%lu contig=%lu); building no-CSS section cache",
            esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
    p.embeddedStyle = false;
  }

  buildState_.reset(new (std::nothrow) BuildState());
  if (!buildState_) {
    LOG_ERR("SCT", "Failed to allocate build state (free=%lu)", esp_get_free_heap_size());
    return false;
  }
  if (!buildState_->initArena(externalScratch_)) {
    LOG_ERR("SCT", "Failed to allocate build arena (free=%lu)", esp_get_free_heap_size());
    buildState_.reset();
    return false;
  }
  buildState_->params = p;
  buildState_->progressFn = progressFn;
  buildState_->requestedHash = requestedHash;
  buildState_->totalStartMs = millis();

  // Increment F: if a content.bin tee writer is attached, emit THIS spine to content.bin alongside
  // the section-cache build. beginSpineAt starts a fresh spine section at the writer's current file
  // position — so a no-CSS RETRY (startBuild called again) correctly begins a new spine (the failed
  // attempt's partial is orphaned but harmless: its slot was never committed). setStage1TeeSink makes
  // runBuildSetup wire the parser's tee (pages + content.bin from one walk).
  if (contentBinTeeWriter_) {
    contentBinTeeWriter_->beginSpineAt(contentBinTeeSpine_);
    stage1Sink_ = contentBinTeeWriter_;
    // TEE mode (default): section pages + content.bin from one walk. CONTENT-ONLY
    // (setContentBinContentOnly): content.bin only, no section-cache pages.
    stage1SinkTee_ = !contentBinContentOnly_;
  }

  if (runBuildSetup(*buildState_) != BuildPhaseResult::Ok) {
    buildState_.reset();
    return false;
  }
  return true;
}

Section::BuildStep Section::stepSectionBuild(const BuildParams& params, const uint32_t budgetMs,
                                             const std::function<void(int)>& progressFn, const bool skipEviction) {
  const uint32_t requestedHash = calculatePropertyHash(
      params.fontId, params.lineCompression, params.extraParagraphSpacing, params.paragraphAlignment,
      params.viewportWidth, params.viewportHeight, params.hyphenationEnabled, params.embeddedStyle,
      params.bionicReadingEnabled, params.inlineFootnotePreviews, params.imageRendering);
  // A live partial build is only resumable for the exact variant it was started for;
  // when the request changed (font/margins/...) the partial cache is the wrong file.
  if (buildState_ && buildState_->requestedHash != requestedHash) {
    LOG_INF("SCT", "stepSectionBuild spine=%d: params changed, discarding partial build", spineIndex);
    abortSectionBuild();
  }

  if (!buildState_) {
    if (!skipEviction) {
      evictOldVariants();
    }
    if (!startBuild(params, progressFn, requestedHash)) {
      return BuildStep::Failed;
    }
  }

  // The CSS fallback (parse-failed-with-CSS) downgrades to a no-CSS build by restarting
  // from setup. Loop rather than recurse; at most one downgrade ever happens.
  while (true) {
    if (runBuildParse(*buildState_, budgetMs) == BuildPhaseResult::More) {
      return BuildStep::More;
    }

    const BuildPhaseResult fin = runBuildFinalize(*buildState_);
    if (fin == BuildPhaseResult::RetryNoCss) {
      BuildParams retryParams = params;
      retryParams.embeddedStyle = false;
      const std::function<void(int)> keepProgress = std::move(buildState_->progressFn);
      // Finalize already closed and removed the failed cache file — just drop the state.
      buildState_.reset();
      if (!startBuild(retryParams, keepProgress, requestedHash)) {
        return BuildStep::Failed;
      }
      // The restart's setup consumed this slice; resume parsing on the next tick. The
      // blocking path (budgetMs == 0) keeps going.
      if (budgetMs != 0) {
        return BuildStep::More;
      }
      continue;
    }

    buildState_.reset();
    const bool done = (fin == BuildPhaseResult::Done);
    // Increment F: publish this spine to content.bin ONLY on a clean completion — a build that
    // CSS-degraded (styles skipped under low heap) or truncated produced wrong/partial records, so
    // its already-written (uncommitted) content.bin spine must NOT be advertised. commitSpine
    // publishes the slot; a bad build leaves it 0 (unavailable), and the orphaned records are dead
    // weight until content.bin is next rewritten.
    if (contentBinTeeWriter_) {
      if (done && !cssLowHeapDegraded_ && !truncatedCache) {
        contentBinTeeWriter_->commitSpine(contentBinTeeSpine_);
      } else {
        LOG_INF("SCT", "content.bin tee spine=%d NOT committed (done=%d cssDegraded=%d truncated=%d)",
                contentBinTeeSpine_, done ? 1 : 0, cssLowHeapDegraded_ ? 1 : 0, truncatedCache ? 1 : 0);
      }
    }
    return done ? BuildStep::Done : BuildStep::Failed;
  }
}

int Section::activeBuildPercent() const {
  if (!buildState_ || !buildState_->parseStarted || buildState_->inflatedSize == 0) {
    return 0;
  }
  const BuildState& st = *buildState_;
  if (st.useTempExtract) {
    // Extraction is cheap next to layout: report it as the first 10%, parsing as the rest.
    if (!st.extractDone) {
      const size_t produced = st.reader ? st.reader->bytesProduced() : 0;
      return static_cast<int>(produced * 10 / st.inflatedSize);
    }
    return static_cast<int>(10 + st.tempBytesFed * 90 / st.inflatedSize);
  }
  if (!st.reader) {
    return 100;  // stream consumed; only Finalize remains
  }
  return static_cast<int>(st.reader->bytesProduced() * 100 / st.inflatedSize);
}

void Section::abortSectionBuild() {
  if (!buildState_) {
    return;
  }
  // Drop the extraction temp file alongside the partial cache file. tempPath is the book-keyed
  // HTML cache: delete it only if WE were producing it (a partial inflate). When it was reused
  // (read-only), it is a complete valid cache — keep it so the next build still skips inflation.
  if (buildState_->tempFile) {
    buildState_->tempFile.close();
  }
  if (!buildState_->reusedHtml && !buildState_->tempPath.empty()) {
    Storage.remove(buildState_->tempPath.c_str());
  }
  // During a live build, `file` is the build's write handle and filePath its cache path
  // (both set by runBuildSetup). The header was never patched, so remove the file.
  file.close();
  Storage.remove(filePath.c_str());
  if (buildState_->cssParser) {
    buildState_->cssParser->clear();
  }
  buildState_.reset();
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (currentPage < 0 || currentPage >= static_cast<int>(lut.size())) {
    LOG_ERR("SCT", "loadPageFromSectionFile: page %d out of LUT range (%u entries)", currentPage,
            static_cast<uint32_t>(lut.size()));
    return nullptr;
  }

  if (!file) {
    // Safety fallback: file was closed unexpectedly; reopen
    LOG_ERR("SCT", "loadPageFromSectionFile: file not open, reopening");
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      return nullptr;
    }
  }

  if (!file.seek(lut[currentPage])) {
    LOG_ERR("SCT", "loadPageFromSectionFile: seek to page %d offset %u failed", currentPage, lut[currentPage]);
    return nullptr;
  }
  return Page::deserialize(file);
  // File is intentionally NOT closed; stays open for the next page load
}

uint16_t Section::activeBuildPageCount() const {
  if (!buildState_) return 0;
  return pageCount;  // pageCount is incremented by onPageComplete() as each page is written
}

uint16_t Section::estimatedTotalPages() const {
  // No build live -> the on-disk count is exact. While building, project from how much of the
  // XHTML has been consumed (activeBuildPercent), but never below what's already laid out. At
  // 100% the stream is exhausted and pageCount is final. Too early (no pages / 0%) -> fall back
  // to the watermark. Adapted from crosspoint-reader PR #2452 by GitHub user itsthisjustin
  // ("Lazy incremental EPUB indexing").
  if (!buildState_) return pageCount;
  const int pct = activeBuildPercent();
  if (pct <= 0 || pct >= 100 || pageCount == 0) return pageCount;
  const uint32_t projected = static_cast<uint32_t>(pageCount) * 100u / static_cast<uint32_t>(pct);
  return projected > pageCount ? static_cast<uint16_t>(projected) : pageCount;
}

bool Section::activeBuildCssDegraded() const {
  // Read the live CSS resolver's running stats: lowHeapSkips is incremented the moment the
  // resolver drops a disk lookup under heap pressure (see CssParser), so it flags a degrading
  // build mid-parse — before runBuildParse latches cssLowHeapDegraded_ at the parse end.
  return buildState_ && buildState_->cssParser && buildState_->cssParser->getResolveStats().lowHeapSkips > 0;
}

std::unique_ptr<Page> Section::loadPageFromActiveBuild(const uint16_t pageIndex) {
  if (!buildState_ || pageIndex >= pageCount) {
    LOG_ERR("SCT", "loadPageFromActiveBuild: page %u out of range (built=%u)", pageIndex, pageCount);
    return nullptr;
  }
  const uint32_t offset = buildState_->lut[pageIndex];
  if (offset == 0 || offset == UINT32_MAX) {
    LOG_ERR("SCT", "loadPageFromActiveBuild: bad LUT entry %u for page %u", offset, pageIndex);
    return nullptr;
  }
  // The build writes pages to `file` without syncing per page, so its most recently written
  // sector — and the directory-entry size — may not be on the card yet. A separate read handle
  // only sees committed data, so flush the writer first; otherwise the read could seek past a
  // stale EOF or deserialize a half-written sector.
  if (file) file.flush();  // SdFat flush() == sync(): commits the cached sector + dir entry
  FsFile readHandle;
  if (!Storage.openFileForRead("SCT", filePath, readHandle)) {
    LOG_ERR("SCT", "loadPageFromActiveBuild: cannot open %s for reading", filePath.c_str());
    return nullptr;
  }
  if (!readHandle.seek(offset)) {
    LOG_ERR("SCT", "loadPageFromActiveBuild: seek to %u failed", offset);
    return nullptr;
  }
  auto page = Page::deserialize(readHandle);
  readHandle.close();
  return page;
}

void Section::warmAllImageCaches(const int xOffset, const int yOffset, const bool forceLoad,
                                 const bool monochromeOutput) {
  if (pageCount == 0) return;
  const int savedPage = currentPage;
  int warmed = 0;
  for (int p = 0; p < static_cast<int>(pageCount); ++p) {
    currentPage = p;
    auto page = loadPageFromSectionFile();
    if (!page || !page->hasImages()) continue;
    page->warmImageCaches(renderer, xOffset, yOffset, forceLoad, monochromeOutput);
    ++warmed;
    // Each image decode can take hundreds of ms; reset the WDT between pages
    // to avoid an interrupt watchdog timeout on image-heavy chapters.
    esp_task_wdt_reset();
  }
  currentPage = savedPage;
  if (warmed > 0) {
    LOG_DBG("SCT", "warmAllImageCaches: warmed %d page(s) with images", warmed);
  }
}

// Resolve TOC anchor-to-page mappings from the parser's in-memory anchor vector.
// Called after createSectionFile when anchors are already in memory.
// See buildTocBoundariesFromFile for the on-disk variant; the two are kept separate
// because the anchor resolution has fundamentally different iteration patterns
// (scan in-memory vector vs. stream from file with early exit).
void Section::buildTocBoundaries(const std::vector<std::pair<std::string, uint16_t>>& anchors) {
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex < 0) return;

  // Count TOC entries for this spine and how many have anchors to resolve
  const int tocCount = epub->getTocItemsCount();
  uint16_t totalEntries = 0;
  uint16_t unresolvedCount = 0;
  for (int i = startTocIndex; i < tocCount; i++) {
    const auto entry = epub->getTocItem(i);
    if (entry.spineIndex != spineIndex) break;
    totalEntries++;
    if (!entry.anchor.empty()) unresolvedCount++;
  }

  // If no TOC entries have anchors, all chapters start at page 0 and
  // getTocIndexForPage falls back to epub->getTocIndexForSpineIndex,
  // so there's nothing to resolve and no value in storing boundaries.
  if (totalEntries == 0 || unresolvedCount == 0) return;

  tocBoundaries.reserve(totalEntries);
  for (int i = startTocIndex; i < startTocIndex + totalEntries; i++) {
    const auto entry = epub->getTocItem(i);
    uint16_t page = 0;
    if (!entry.anchor.empty()) {
      for (const auto& [key, val] : anchors) {
        if (key == entry.anchor) {
          page = val;
          break;
        }
      }
    }
    tocBoundaries.push_back({i, page});
  }

  // Defensive sort in case TOC entries are out of document order in a malformed epub
  std::sort(tocBoundaries.begin(), tocBoundaries.end(),
            [](const TocBoundary& a, const TocBoundary& b) { return a.startPage < b.startPage; });
}

// Resolve TOC anchor-to-page mappings by scanning the section cache's on-disk anchor data.
// Called from loadSectionFile when anchors are not in memory. Caches the small set of
// TOC anchor strings first (since getTocItem does file I/O to BookMetadataCache), then
// streams through on-disk anchors matching only those, stopping as soon as all are found.
// See buildTocBoundaries for the in-memory variant.
void Section::buildTocBoundariesFromFile(FsFile& f) {
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex < 0) return;

  // Count TOC entries for this spine, then reserve and populate
  const int tocCount = epub->getTocItemsCount();
  uint16_t totalEntries = 0;
  uint16_t unresolvedCount = 0;
  for (int i = startTocIndex; i < tocCount; i++) {
    const auto entry = epub->getTocItem(i);
    if (entry.spineIndex != spineIndex) break;
    totalEntries++;
    if (!entry.anchor.empty()) unresolvedCount++;
  }

  // If no TOC entries have anchors, all chapters start at page 0 and
  // getTocIndexForPage falls back to epub->getTocIndexForSpineIndex,
  // so there's nothing to resolve and no value in storing boundaries.
  if (totalEntries == 0 || unresolvedCount == 0) return;

  // Cache TOC anchor strings before scanning disk, since getTocItem() does file I/O
  struct TocAnchorEntry {
    int tocIndex;
    std::string anchor;
  };
  std::vector<TocAnchorEntry> tocAnchorsToResolve;
  tocAnchorsToResolve.reserve(unresolvedCount);
  tocBoundaries.reserve(totalEntries);
  for (int i = startTocIndex; i < startTocIndex + totalEntries; i++) {
    const auto entry = epub->getTocItem(i);
    tocBoundaries.push_back({i, 0});
    if (!entry.anchor.empty()) {
      tocAnchorsToResolve.push_back({i, std::move(entry.anchor)});
    }
  }

  // Single pass through on-disk anchors, matching against cached TOC anchors.
  // Stop early once all TOC anchors are resolved.
  f.seek(header::kAnchorMap);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);

  if (anchorMapOffset != 0) {
    f.seek(anchorMapOffset);
    uint16_t count;
    serialization::readPod(f, count);
    std::string key;
    for (uint16_t i = 0; i < count && unresolvedCount > 0; i++) {
      uint16_t page;
      serialization::readString(f, key);
      serialization::readPod(f, page);
      for (auto& tocAnchor : tocAnchorsToResolve) {
        if (!tocAnchor.anchor.empty() && key == tocAnchor.anchor) {
          tocBoundaries[tocAnchor.tocIndex - startTocIndex].startPage = page;
          tocAnchor.anchor.clear();  // mark resolved
          unresolvedCount--;
          break;
        }
      }
    }
  }

  // Defensive sort in case TOC entries are out of document order in a malformed epub
  std::sort(tocBoundaries.begin(), tocBoundaries.end(),
            [](const TocBoundary& a, const TocBoundary& b) { return a.startPage < b.startPage; });
}

void Section::buildPageBreakLabelsFromFile(FsFile& f) {
  pageBreakLabels.clear();
  f.seek(header::kPageBreakMap);
  uint32_t pageBreakMapOffset;
  serialization::readPod(f, pageBreakMapOffset);
  if (pageBreakMapOffset == 0 || pageBreakMapOffset >= f.size()) {
    return;
  }

  f.seek(pageBreakMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    uint16_t page;
    std::string label;
    serialization::readPod(f, page);
    serialization::readString(f, label);
    pageBreakLabels.emplace_back(page, std::move(label));
  }
}

int Section::getTocIndexForPage(const int page) const {
  if (tocBoundaries.empty()) {
    return epub->getTocIndexForSpineIndex(spineIndex);
  }

  // Find the first boundary AFTER page, then step back one
  auto it = std::upper_bound(tocBoundaries.begin(), tocBoundaries.end(), static_cast<uint16_t>(page),
                             [](uint16_t page, const TocBoundary& boundary) { return page < boundary.startPage; });
  if (it == tocBoundaries.begin()) {
    return tocBoundaries[0].tocIndex;
  }
  return std::prev(it)->tocIndex;
}

std::optional<int> Section::getPageForTocIndex(const int tocIndex) const {
  for (const auto& boundary : tocBoundaries) {
    if (boundary.tocIndex == tocIndex) {
      return boundary.startPage;
    }
  }
  return std::nullopt;
}

std::optional<Section::TocPageRange> Section::getPageRangeForTocIndex(const int tocIndex) const {
  for (size_t i = 0; i < tocBoundaries.size(); i++) {
    if (tocBoundaries[i].tocIndex == tocIndex) {
      const int startPage = tocBoundaries[i].startPage;
      const int endPage = (i + 1 < tocBoundaries.size()) ? static_cast<int>(tocBoundaries[i + 1].startPage) : pageCount;
      return TocPageRange{startPage, endPage};
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(header::kAnchorMap);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      f.close();
      return page;
    }
  }

  f.close();
  return std::nullopt;
}

std::optional<std::string> Section::getPrintedPageLabelFromCache(const std::string& sectionsDir, int spineIndex,
                                                                 uint16_t page) {
  // Find any cache variant for spineIndex. Filename format: "<spineIndex>_<hash>.bin".
  // We pick the first match — all variants for the same spine share the same printed-page
  // anchors (those are content-derived, not render-parameter-derived).
  char prefix[16];
  snprintf(prefix, sizeof(prefix), "%d_", spineIndex);
  const auto files = Storage.listFiles(sectionsDir.c_str(), 50);
  std::string match;
  for (const auto& f : files) {
    if (f.startsWith(prefix) && f.endsWith(".bin")) {
      match = f.c_str();
      break;
    }
  }
  if (match.empty()) {
    return std::nullopt;
  }

  FsFile file;
  if (!Storage.openFileForRead("SCT", sectionsDir + "/" + match, file)) {
    return std::nullopt;
  }

  // Header version guard — refuse to read a cache written by a different layout.
  uint8_t version = 0;
  file.seek(header::kVersion);
  serialization::readPod(file, version);
  if (version != SECTION_FILE_VERSION) {
    file.close();
    return std::nullopt;
  }

  file.seek(header::kPageBreakMap);
  uint32_t pageBreakMapOffset = 0;
  serialization::readPod(file, pageBreakMapOffset);
  if (pageBreakMapOffset == 0 || pageBreakMapOffset >= file.size()) {
    file.close();
    return std::nullopt;
  }

  file.seek(pageBreakMapOffset);
  uint16_t count = 0;
  serialization::readPod(file, count);
  std::vector<std::string> labelsOnPage;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t entryPage = 0;
    std::string label;
    serialization::readPod(file, entryPage);
    serialization::readString(file, label);
    if (entryPage == page) {
      labelsOnPage.push_back(std::move(label));
    } else if (entryPage > page) {
      break;
    }
  }
  file.close();

  if (labelsOnPage.empty()) {
    return std::nullopt;
  }
  if (labelsOnPage.size() == 1 || labelsOnPage.front() == labelsOnPage.back()) {
    return std::string("(") + labelsOnPage.front() + ")";
  }
  return std::string("(") + labelsOnPage.front() + "/" + labelsOnPage.back() + ")";
}

std::optional<std::string> Section::getNearestPrintedPageLabelAtOrBefore(uint16_t page) const {
  // pageBreakLabels is built in document order (i.e. ascending pageIndex), so the last
  // entry whose page is <= `page` is the "you're currently reading at or after this
  // printed page" hint. Returns the raw label (no parens, no slash-collapsing).
  std::optional<std::string> best;
  for (const auto& [labelPage, label] : pageBreakLabels) {
    if (labelPage > page) break;
    best = label;
  }
  return best;
}

std::optional<std::string> Section::getPrintedPageLabelForPage(uint16_t page) const {
  // Collect every printed-page label whose anchor lands on this exact rendered page.
  // Multiple labels can co-occur when a short device page contains more than one EPUB
  // pagebreak marker (e.g. printed pages 7 and 8 both starting within the same device page).
  // pageBreakLabels is recorded in document order, so we can short-circuit once we pass `page`.
  std::vector<std::string> labels;
  for (const auto& [labelPage, label] : pageBreakLabels) {
    if (labelPage == page) {
      labels.push_back(label);
    } else if (labelPage > page) {
      break;
    }
  }

  if (labels.empty()) {
    return std::nullopt;
  }
  if (labels.size() == 1 || labels.front() == labels.back()) {
    return std::string("(") + labels.front() + ")";
  }
  return std::string("(") + labels.front() + "/" + labels.back() + ")";
}

bool Section::readParagraphLutHeader(FsFile& outFile, uint16_t& outCount, uint32_t& outLutStart) const {
  if (!Storage.openFileForRead("SCT", filePath, outFile)) {
    return false;
  }

  const uint32_t fileSize = outFile.size();

  outFile.seek(header::kParagraphLut);
  uint32_t paragraphLutOffset;
  serialization::readPod(outFile, paragraphLutOffset);
  if (fileSize < sizeof(uint16_t) || paragraphLutOffset == 0 || paragraphLutOffset > fileSize - sizeof(uint16_t)) {
    outFile.close();
    return false;
  }

  outFile.seek(paragraphLutOffset);
  serialization::readPod(outFile, outCount);
  if (outCount == 0) {
    outFile.close();
    return false;
  }

  const uint64_t remainingBytes = static_cast<uint64_t>(fileSize) - paragraphLutOffset;
  const uint64_t requiredBytes = sizeof(uint16_t) + static_cast<uint64_t>(outCount) * PARAGRAPH_LUT_ENTRY_SIZE;
  if (remainingBytes < requiredBytes) {
    outFile.close();
    return false;
  }

  outLutStart = paragraphLutOffset + sizeof(uint16_t);

  return true;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  const uint32_t fileSize = f.size();

  // Each LUT entry stores the paragraph index at page-break time — i.e. the last
  // <p> whose start tag had been seen while page i was being laid out. Paragraph
  // P therefore first appears on the smallest i where storedPIdx[i] >= P.
  for (uint16_t i = 0; i < count; i++) {
    const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, i) + sizeof(uint32_t);
    const uint64_t requiredOffset = static_cast<uint64_t>(entryOffset) + sizeof(uint16_t);
    if (requiredOffset > fileSize) {
      f.close();
      return std::nullopt;
    }
    f.seek(entryOffset);
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      f.close();
      return i;
    }
  }

  f.close();
  return static_cast<uint16_t>(count - 1);
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  if (page >= count) {
    f.close();
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, page) + sizeof(uint32_t);
  const uint64_t requiredOffset = static_cast<uint64_t>(entryOffset) + sizeof(uint16_t);
  if (requiredOffset > fileSize) {
    f.close();
    return std::nullopt;
  }

  // Seek directly to the paragraphIndex field of the requested entry (skip xhtmlByteOffset)
  f.seek(entryOffset);
  uint16_t pIdx;
  serialization::readPod(f, pIdx);

  f.close();
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  if (liIndex == 0) {
    return std::nullopt;
  }

  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  const uint32_t fileSize = f.size();

  // Mirror getPageForParagraphIndex: each entry stores the running li count at page-break
  // time, so the target li first appears on the smallest i where storedLiIdx[i] >= liIndex.
  // The listItemIndex field follows xhtmlByteOffset + paragraphIndex within each entry.
  for (uint16_t i = 0; i < count; i++) {
    const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, i) + sizeof(uint32_t) + sizeof(uint16_t);
    const uint64_t requiredOffset = static_cast<uint64_t>(entryOffset) + sizeof(uint16_t);
    if (requiredOffset > fileSize) {
      f.close();
      return std::nullopt;
    }
    f.seek(entryOffset);
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      f.close();
      return i;
    }
  }

  f.close();
  return static_cast<uint16_t>(count - 1);
}

std::optional<uint32_t> Section::getXhtmlByteOffsetForPage(const uint16_t page) const {
  FsFile f;
  uint16_t count = 0;
  uint32_t lutStart = 0;
  if (!readParagraphLutHeader(f, count, lutStart)) {
    return std::nullopt;
  }
  if (page >= count) {
    f.close();
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  const uint32_t entryOffset = paragraphLutEntryOffset(lutStart, page);
  const uint64_t requiredOffset = static_cast<uint64_t>(entryOffset) + sizeof(uint32_t);
  if (requiredOffset > fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(entryOffset);
  uint32_t byteOffset;
  serialization::readPod(f, byteOffset);

  f.close();
  // A zero offset means the entry was recorded post-parse (last page), so it's unusable as a hint.
  return byteOffset > 0 ? std::optional<uint32_t>{byteOffset} : std::nullopt;
}
