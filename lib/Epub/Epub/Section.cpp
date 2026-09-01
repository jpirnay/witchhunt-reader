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
#include <BuildArena.h>
#include <InflateReader.h>

#include <algorithm>
#include <cstring>

#include "Epub/css/CssParser.h"
#include "FootnotePreviews.h"
#include "Page.h"
#include "blocks/ImageBlock.h"  // image_scratch: pass-wide decode arena
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION = 71;  // bumped: the packed word style byte now carries a
                                              // per-word "continues the previous word" bit, so the
                                              // dictionary overlay can select a bionic-split or
                                              // hyphenated word as one word. A v70 cache reads the
                                              // bit as clear everywhere, i.e. keeps the old split
                                              // selection, so it has to be rebuilt
                                              // v70: a wrapper's horizontal inset reaches every
                                              // child block, and insets/hanging indents are clamped to
                                              // the panel. Layout bakes insets into word xpos (see v61),
                                              // so a v69 cache keeps the off-panel lines this fixes
                                              // v69: table cells carry a colSpan
                                              // v68: near-body font-size snapping now also covers block-level
                                              // CSS (p.body { font-size: 1.1em }), not just inline wrappers
                                              // v67: img boxes honour `auto` and keep the source aspect ratio
                                              // v64: main-text font-size normalization
                                              // v63: drop-cap float zones + ink-metric cap placement
                                              // (62 was consumed by an earlier iteration of this feature)
                                              // v61: TextBlock no longer serializes the block-spacing
                                              // fields (margins/padding/indent + their defined flags);
                                              // layout bakes them into word xpos/line y and nothing read
                                              // them back — 19 bytes/line saved
                                              // v60: TextBlock word data serialized as one flat arena
                                              // (offset table + NUL-terminated text blob) instead of
                                              // length-prefixed strings and per-field arrays
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
//
// Applies to HEAP-BACKED builds only. An arena-backed build (borrowed framebuffer) takes the
// ruleset from the arena and is exempt — see heapAllowsEmbeddedStyle(..., arenaBacked).
// 2026-08-11: 56 KB -> 44 KB, because the component it was covering no longer exists.
//
// The 56 KB was index + hot/negative caches + margin, sized against the resolver's 40 KB
// self-protection floor. Lean resolve is now unconditional (see runBuildSetup), so the hot LRU
// never allocates — that is the "typically ~10 KB" term above — and the resolver's floor is
// LEAN_MIN_FREE_HEAP_FOR_CSS (24 KB), not 40 KB. Both halves of the arithmetic moved down.
//
// It also had to move for the gate to mean anything. On X3 free heap peaks at ~72 KB at reader
// entry and sits at 50-60 KB while reading, so a 56 KB floor rejected essentially every
// heap-backed CSS build — and Background-B, whose fallback path this gates, simply stopped
// pre-building on CSS books. A floor that no reachable heap state satisfies is not a safety
// margin, it is a disabled feature.
//
// This is DERIVED, not measured: 56 minus the ~10 KB hot cache, rounded down. It is a
// pre-filter, not a guarantee, and it is not the safety mechanism — that is the contig check
// below (a failed std::vector reserve aborts under -fno-exceptions, so contig must be real)
// plus the resolver's own 24 KB floor and isCssLowHeapDegraded(), which lets a caller discard a
// degraded result rather than ship it. Watch lowHeapSkips: if heap-backed builds start
// degrading, this is the number that moved.
#ifndef SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES
#define SCT_EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES (44 * 1024)
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

// --- Heap-analysis instrumentation (temporary; heap-analysis branch) ------------------------
// The fragmented-heap restart fires because a 52 KB framebuffer realloc cannot find one
// contiguous block, and the build is what leaves it that way — measured end-of-build state was
// "50380 free, 11764 max alloc". What is NOT known is WHERE inside the build the largest block
// collapses, which decides what to fix. Existing phase logs print free heap only; this adds the
// contiguous number at the same points plus a per-page trace.
//
// SCT_HEAP_TRACE=0 compiles all of it out. Remove this block once the question is answered.
// Default OFF (2026-08-11). This trace answered what it was added for: section 8.4 of
// docs/memory-allocation-strategy.md asked whether small-object churn or retention collapses the
// contiguous block during a parse, and the per-page block counts showed BOTH — freeBlk 16 -> 35
// from churn, plus a one-page cliff from the mid-build render's font page slots (fixed in
// 040b2c1b). Kept rather than deleted because the cliff is only partly closed and the next
// attempt needs exactly these numbers again: build with -DSCT_HEAP_TRACE=1.
//
// Run 2026-08-18 (X4, 241 KB Cyrillic chapter, 262 pages, cold build): the parse ends ~9 KB and
// ~65 blocks above where it started while contig steps down in exact 1024-byte units — doubling
// reallocs of containers held for the whole parse, not a single cliff (the low-water probe found
// no transient below 27 KB across the entire feed). The `retained:` line added below names only
// ~2.9 KB of it (anchors 784 B, paraLut 2096 B, pageBreakLabels 0); the other ~6 KB is still
// unattributed, with the two CssStyle unordered_map caches the prime suspect.
//
// Left unfixed on purpose: the dominant term was never the parse. It was the heap the build
// STARTED from — the reading-stats store cost ~15 KB and dropped largest8 65524 -> 26612 before
// the reader opened anything. With that made lazy the same chapter builds from contig 49140,
// finishes all 262 pages in 15.9 s, and logs ZERO "degraded mode" lines where it previously
// logged hundreds and then aborted. Revisit only if a chapter starts failing from a healthy heap.
#ifndef SCT_HEAP_TRACE
#define SCT_HEAP_TRACE 0
#endif

#if SCT_HEAP_TRACE
#define SCT_TRACE_HEAP(spine, label)                                      \
  LOG_INF("HEAP", "spine=%d %-18s free=%lu contig=%lu", (spine), (label), \
          static_cast<unsigned long>(esp_get_free_heap_size()),           \
          static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)))

// Per-chunk low-water probe for the parse feed loop.
//
// The per-page trace showed something the page granularity cannot explain: on X4 spine 1,
// free went 51088 -> 6996 between page 13 and page 14 (contig 42996 -> 4084) and recovered to
// 43948 by page 18, with an 86 ms gap where the normal page interval is ~30 ms. That is a large
// transient block held across several pages, not gradual churn — and nothing on the parse path
// is supposed to take ~44 KB.
//
// The parser is fed in PARSE_CHUNK_BYTES (1 KB) slices, so sampling around each write() brackets
// the drop to a single chunk instead of a whole page. Only the WORST dip is kept and it is
// reported once per page, so this costs one log line per page rather than one per KB.
struct ParseHeapLowWater {
  uint32_t minFree = UINT32_MAX;
  uint32_t minContig = UINT32_MAX;
  uint32_t atByteOffset = 0;  // bytes fed into the parser when the low point was seen

  void sample(const size_t bytesFedSoFar) {
    const uint32_t f = static_cast<uint32_t>(esp_get_free_heap_size());
    if (f < minFree) {
      minFree = f;
      minContig = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
      atByteOffset = static_cast<uint32_t>(bytesFedSoFar);
    }
  }
  void reset() { *this = ParseHeapLowWater{}; }
  bool seen() const { return minFree != UINT32_MAX; }
};

// One instance: only one section build parses at a time (Background-B and C are mutually
// exclusive over the arena), so a file-scope probe cannot interleave.
ParseHeapLowWater g_parseLowWater;
#endif
#if !SCT_HEAP_TRACE
#define SCT_TRACE_HEAP(spine, label) ((void)0)
#endif

// Visitor-feed chunk for phase (b): the granularity at which runBuildParse checks its time budget
// between visitor writes. Kept small so a build slice yields promptly and input stays responsive.
constexpr size_t PARSE_CHUNK_BYTES = 1024;

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

uint32_t Section::calculatePropertyHash(const BuildParams& p) {
  uint8_t buffer[64];
  size_t offset = 0;

  auto append = [&](const void* ptr, size_t size) {
    memcpy(buffer + offset, ptr, size);
    offset += size;
  };

  append(&p.fontId, sizeof(p.fontId));
  append(&p.lineCompression, sizeof(p.lineCompression));
  append(&p.extraParagraphSpacing, sizeof(p.extraParagraphSpacing));
  append(&p.paragraphAlignment, sizeof(p.paragraphAlignment));
  append(&p.viewportWidth, sizeof(p.viewportWidth));
  append(&p.viewportHeight, sizeof(p.viewportHeight));
  append(&p.hyphenationEnabled, sizeof(p.hyphenationEnabled));
  append(&p.fontSizeNormalization, sizeof(p.fontSizeNormalization));
  append(&p.embeddedStyle, sizeof(p.embeddedStyle));
  append(&p.bionicReadingEnabled, sizeof(p.bionicReadingEnabled));
  append(&p.inlineFootnotePreviews, sizeof(p.inlineFootnotePreviews));
  if (p.inlineFootnotePreviews) {
    append(&INLINE_FOOTNOTE_PREVIEW_LAYOUT_VERSION, sizeof(INLINE_FOOTNOTE_PREVIEW_LAYOUT_VERSION));
  }
  append(&p.imageRendering, sizeof(p.imageRendering));

  return fnv1a(buffer, offset);
}

std::string Section::getSectionFilePath(uint32_t propertyHash) const {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d_%08x", spineIndex, propertyHash);
  return epub->getCachePath() + "/sections/" + buf + ".bin";
}

std::string Section::sectionHtmlCachePath(const std::string& bookCachePath, const int spineIndex) {
  char buf[32];
  snprintf(buf, sizeof(buf), "html_%d", spineIndex);
  return bookCachePath + "/sections/" + buf + ".bin";
}

std::string Section::getSectionHtmlCachePath() const { return sectionHtmlCachePath(epub->getCachePath(), spineIndex); }

// Deliberately carries NEITHER the spine index nor the layout property hash. What gets written
// here is the archive entry's own bytes, which do not depend on either -- only the .pxc pixel
// caches do, because those are dithered at display dimensions.
//
// It used to be keyed by both, so changing font size, margins or orientation re-extracted every
// image from scratch: 3.3 s and 857 KB of SD writes for one cover, per layout, with up to five
// byte-identical copies alive at once (evictOldVariants keeps 5 section variants). Now one
// extraction serves every variant, and two spines referencing the same image share it too.
std::string Section::getImageBasePath() const { return epub->getCachePath() + "/img_"; }

struct SectionVariant {
  std::string filename;
  uint16_t date;
  uint16_t time;
};

void Section::evictOldVariants() const {
  // We keep up to 5 most recently accessed/modified variants to prevent SD card bloat
  constexpr size_t MAX_VARIANTS = 5;

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
        // Legacy layout-keyed image caches (img_<spine>_<hash>_<n>) only. Current names are
        // content-keyed and shared across variants, so they must NOT die with a section variant;
        // they are reclaimed by Epub::clearCache like any other parsing artifact. This branch
        // stays to sweep up what older firmware left behind.
        char legacyPrefix[32];
        snprintf(legacyPrefix, sizeof(legacyPrefix), "img_%d_%08x_", spineIndex, parsedHash);
        std::string imgBasePath = epub->getCachePath() + "/" + legacyPrefix;
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

#if SCT_HEAP_TRACE
  // Per-page contiguous trace: the page is serialized and destroyed here, so this samples the
  // heap at the one point in the parse where a page's worth of transient objects has just been
  // released. A monotonic decline across pages means the parse is the fragmenter; a flat line
  // with a single step means something else is, and the step says where to look.
  // Page-boundary state, plus the WORST heap state seen while this page was being parsed. The
  // boundary numbers alone hid a ~44 KB transient (X4 spine 1, pages 13->14): whatever takes it
  // is allocated and released between two page emissions, so only a low-water mark inside the
  // feed loop can see it. lowAt is the parser byte offset where the dip occurred — feed that
  // offset back into the chapter XHTML to identify the construct responsible.
  //
  // Block counts are what separate the two remaining explanations, and free/contig alone cannot:
  //   allocBlk rising, allocBytes rising -> the parse RETAINS something per page
  //   allocBlk flat, freeBlk rising      -> pure fragmentation; the bytes come back split
  // Measured X3 2026-08-11: contig fell 40948 -> 15348 across one 17-page parse while free
  // oscillated 20-40 KB, with the arena covering the ring, the chunk feed AND the CSS ruleset
  // (failedAlloc=0). So whatever does this is on the heap and is NOT arena-eligible — but no
  // measurement yet names it. That is the open question in docs/memory-allocation-strategy.md
  // section 8.4, unchanged since it was written; these three counters are what answer it.
  multi_heap_info_t pageHeapInfo{};
  heap_caps_get_info(&pageHeapInfo, MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  if (g_parseLowWater.seen()) {
    LOG_INF("HEAP",
            "spine_page=%d free=%lu contig=%lu allocBlk=%lu freeBlk=%lu allocBytes=%lu | page-low free=%lu "
            "contig=%lu lowAt=%lu",
            pageCount, static_cast<unsigned long>(esp_get_free_heap_size()),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)),
            static_cast<unsigned long>(pageHeapInfo.allocated_blocks),
            static_cast<unsigned long>(pageHeapInfo.free_blocks),
            static_cast<unsigned long>(pageHeapInfo.total_allocated_bytes),
            static_cast<unsigned long>(g_parseLowWater.minFree), static_cast<unsigned long>(g_parseLowWater.minContig),
            static_cast<unsigned long>(g_parseLowWater.atByteOffset));
    g_parseLowWater.reset();  // per-page window
  } else {
    LOG_INF("HEAP", "spine_page=%d free=%lu contig=%lu allocBlk=%lu freeBlk=%lu allocBytes=%lu", pageCount,
            static_cast<unsigned long>(esp_get_free_heap_size()),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)),
            static_cast<unsigned long>(pageHeapInfo.allocated_blocks),
            static_cast<unsigned long>(pageHeapInfo.free_blocks),
            static_cast<unsigned long>(pageHeapInfo.total_allocated_bytes));
  }
#endif

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

bool Section::loadSectionFile(const BuildParams& p) {
  truncatedCache = false;
  embeddedStyleFallback = false;
  uint32_t propertyHash = calculatePropertyHash(p);
  filePath = getSectionFilePath(propertyHash);

  bool usingEmbeddedStyleFallback = false;
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    // Fallback: allow loading a no-CSS cache variant when embedded CSS is enabled.
    if (p.embeddedStyle) {
      BuildParams noCss = p;
      noCss.embeddedStyle = false;
      const uint32_t fallbackHash = calculatePropertyHash(noCss);
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
        (p.embeddedStyle == fileEmbeddedStyle) || (usingEmbeddedStyleFallback && !fileEmbeddedStyle);
    if (p.fontId != fileFontId || p.lineCompression != fileLineCompression ||
        p.extraParagraphSpacing != fileExtraParagraphSpacing || p.paragraphAlignment != fileParagraphAlignment ||
        p.viewportWidth != fileViewportWidth || p.viewportHeight != fileViewportHeight ||
        p.hyphenationEnabled != fileHyphenationEnabled || !embeddedStyleMatches ||
        p.bionicReadingEnabled != fileBionicReadingEnabled || p.imageRendering != fileImageRendering) {
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
  // Only close a handle we actually opened: a Section whose cache is cleared before it ever
  // loaded or built (settings change on a freshly constructed Section) still holds the default
  // handle, and HalFile::close() asserts on those. Same reason as the guards in runBuildSetup
  // and abortBuild.
  if (file) file.close();  // Must be closed before removal on FAT32
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
  // Disk-backed book-level footnote preview lookup (footnotes.bin), opened at setup when
  // the build wants inline previews. Owned here so the visitor's non-owning pointer stays
  // valid across build slices.
  std::unique_ptr<FootnotePreviews::Lookup> footnotePreviewLookup;
  // Property hash of the params as requested by the caller, before any low-heap CSS
  // downgrade. Lets stepSectionBuild detect a stale partial build (variant changed)
  // without a heap-forced no-CSS build reading as a mismatch against its own request.
  uint32_t requestedHash = 0;
  // True once the between-phases step has run: note previews resolved and the layout parser
  // initialised. runBuildParse is re-entered on every slice, so without this the resolve would
  // re-scan the whole spine each time phase (b) yielded.
  bool visitorReady = false;
  // The note-preview resolve, which is itself sliced: it survives across yields until it reports
  // Done or Failed, and its destructor rolls back a half-written store if the build is abandoned
  // first. `previewResolveDone` covers the case where it finished but phase (b) has not started.
  std::unique_ptr<FootnotePreviews::Resolver> previewResolver;
  bool previewResolveDone = false;
  uint32_t previewResolveStartMs = 0;
  uint32_t previewResolveMs = 0;          // CPU actually spent resolving, summed across slices
  uint32_t previewResolveMaxSliceMs = 0;  // worst single slice — the number that says it slices
  // Parse-result flags, set by runBuildParse and consumed by runBuildFinalize.
  bool streamOk = false;
  bool finalizeOk = false;
  bool parserStreamOk = false;
  // Coarse timing for the summary log; per-phase wall clock (parseMs accumulates across slices).
  uint32_t totalStartMs = 0;
  uint32_t setupMs = 0;
  uint32_t parseMs = 0;
};

// Out-of-line (see header): both need the complete BuildState type, and the dtor must
// not leave a partially written cache file behind when a Section dies with a build in
// flight (book close, activity exit) — its header was never patched with offsets.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub), spineIndex(spineIndex), renderer(renderer) {}
Section::~Section() { abortSectionBuild(); }

// Starts this spine's note-preview resolve. False means it could not even begin (OOM) — the
// caller treats that like a failed pass: the build goes on and the markers stay plain.
//
// The pass is SLICED from here on. It used to run to completion in this one call, which made its
// cost a property of the book rather than of the slice budget: a big chapter's link scan and a
// fat rearnotes document are both unbounded work on the loop task. Background-B ran it that way
// and could stall input for as long as the document took. FootnotePreviews::Resolver does the
// same work a chunk at a time so runBuildParse can spend its budget on it and yield, exactly as
// it does on the layout parse.
bool Section::beginInlineFootnotePreviewResolve(BuildState& st) {
  // Cheap when there is nothing to do: a spine whose notes were resolved in an earlier session
  // answers from its resolved bit and finishes on the first step, reading nothing.
  //
  // Pass A reads the XHTML this build just extracted, so it costs an SD read and a SAX walk with
  // no inflate. Only a note document that has never been streamed needs the archive, and that
  // one gets the build's arena when there is room — a sliced resolve would otherwise hold its
  // inflate ring on the reading heap across every page render in between.
  st.previewResolver = makeUniqueNoThrow<FootnotePreviews::Resolver>();
  if (!st.previewResolver) {
    return false;
  }
  const std::string banked = st.useTempExtract ? st.tempPath : std::string();
  BuildArena* const external = (st.arena && st.arena != st.ownedArena.get()) ? st.arena : nullptr;
  if (!st.previewResolver->begin(*epub, spineIndex, banked, external)) {
    st.previewResolver.reset();
    return false;
  }
  st.previewResolveStartMs = millis();
  return true;
}

// Wires the visitor to the store once the resolve has finished (however it finished): even a
// failed pass leaves earlier chapters' notes in place, and those still expand.
void Section::finishInlineFootnotePreviewResolve(BuildState& st) {
  if (!st.params.inlineFootnotePreviews) {
    return;
  }
  st.footnotePreviewLookup = makeUniqueNoThrow<FootnotePreviews::Lookup>();
  if (st.footnotePreviewLookup && st.footnotePreviewLookup->open(epub->getCachePath(), epub.get(), spineIndex)) {
    st.visitor->setInlineFootnotePreviews(st.footnotePreviewLookup.get());
  } else {
    st.footnotePreviewLookup.reset();  // no notes anywhere in this book yet, or store unreadable
  }
  if (st.previewResolveMs > 0) {
    // Both numbers, because they answer different questions: the total is what the resolve costs
    // the build, the worst slice is whether it is actually slicing. The second is the one that
    // regresses silently — a document that never yields shows up here and nowhere else.
    LOG_INF("SCT", "createSectionFile spine=%d footnote previews resolved in %ums (worst slice %ums)", spineIndex,
            st.previewResolveMs, st.previewResolveMaxSliceMs);
  }
}

Section::BuildPhaseResult Section::runBuildSetup(BuildState& st) {
  const BuildParams& p = st.params;
  st.propertyHash = calculatePropertyHash(p);
  filePath = getSectionFilePath(st.propertyHash);

  st.localPath = epub->getSpineItem(spineIndex).href;
  SCT_TRACE_HEAP(spineIndex, "build_start");
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
  // "Previously loaded" is the exception, not the rule: on a first build this handle has never
  // been opened, and closing one of those asserts inside the HAL.
  if (file) file.close();
  pageCount = 0;
  this->lut.clear();
  cssLowHeapDegraded_ = false;
  footnotePreviewsUnresolved_ = false;
  imageHeaderDegraded_ = false;

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return BuildPhaseResult::Failed;
  }
  writeSectionFileHeader(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
                         p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle, p.bionicReadingEnabled,
                         p.imageRendering);
  st.lut.clear();
  // One u32 per page, appended by the completePageFn below across the whole parse. Pre-sized for
  // the same reason as the parser's paragraph LUT — see estimatePagesForSpine and
  // docs/memory-allocation-strategy.md §9.6.
  st.lut.reserve(estimatePagesForSpine(st.inflatedSize));

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = st.localPath.find_last_of('/');
  st.contentBase = (lastSlash != std::string::npos) ? st.localPath.substr(0, lastSlash + 1) : "";
  st.imageBasePath = getImageBasePath();

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
      // Lean resolve unconditionally, not just for arena builds. It does exactly two things:
      // skip the hot-rule LRU, and drop the resolver's self-protection floor from
      // MIN_FREE_HEAP_FOR_CSS (40 KB) to LEAN_MIN_FREE_HEAP_FOR_CSS (24 KB).
      //
      // The hot LRU cannot hit during a build, structurally: ChapterHtmlSlimParser memoises
      // resolved styles on `tag|class|id` in cssStyleCache_ BEFORE calling resolveStyle
      // (ChapterHtmlSlimParser.cpp), so the resolver only ever sees the FIRST occurrence of each
      // distinct key. A second-occurrence cache downstream of a first-occurrence filter has
      // nothing to catch. Measured on three books with the LRU enabled (heap-backed host runs):
      // hotHits=0 on every spine — alice 341 calls, kings-avatar 24, small-gods 17.
      //
      // So the LRU was pure cost: ~100 B/entry × 128 entries of heap growth, and 16 KB of extra
      // resolver floor to accommodate it. Dropping it makes the disk-backed path — sparse index
      // (8 B/rule) + on-demand seek + negative cache — the single CSS strategy on every build
      // path, instead of only the borrowed one.
      st.cssParser->setLeanResolve(true);
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
      p.viewportHeight, p.hyphenationEnabled, p.fontSizeNormalization, p.bionicReadingEnabled,
      [this, &st](std::unique_ptr<Page> page) { st.lut.emplace_back(this->onPageComplete(std::move(page))); },
      p.embeddedStyle, st.contentBase, st.imageBasePath, p.imageRendering, std::move(tocAnchors), st.progressFn,
      st.cssParser, epub->getImageManifest());
  st.visitor->setExternalPageBreakAnchors(std::move(externalPageBreakAnchors));
  st.visitor->setFontSizeLadder(p.fontSizeLadder);
  Hyphenator::setPreferredLanguage(epub->getLanguage());

  // Inline footnote previews are NOT wired up here: the note text this spine needs may not be
  // in the store yet, and resolving it needs the spine's inflated XHTML — which phase (a) is
  // about to produce. See resolveInlineFootnotePreviews(), called between the phases.

  // The layout parser is NOT initialised here. Its yxml state is ~10 KB, and the note-preview
  // resolve that runs between the phases needs a SAX parser of its own; initialising this one
  // first would put both on the heap at once for no reason. See runBuildParse.
  st.setupMs = millis() - phaseSetupStart;
  SCT_TRACE_HEAP(spineIndex, "after_setup");
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
        SCT_TRACE_HEAP(spineIndex, "after_extract");
        LOG_INF("SCT", "createSectionFile spine=%d extracted %u bytes to temp (free=%lu)", spineIndex,
                static_cast<uint32_t>(st.inflatedSize), esp_get_free_heap_size());
        if (overBudget()) {
          return yieldSlice();
        }
      }
    }
  }

  // The gap between the phases: the whole document is on SD, and ALL ZIP state was just
  // released, so this is both the only moment the note text can be resolved from a local file
  // and the widest the heap gets during a build. Preview text is a layout input — it changes
  // line breaking — so it has to be in place before the visitor sees the first byte, or this
  // spine would be cached under the previews-on hash while showing bare markers.
  //
  // The resolve's SAX parser is created and destroyed inside this block, and the layout parser
  // is only initialised after it: ~10 KB of yxml state each, and never both at once. Guarded to
  // run a single time — phase (b) yields per slice and re-enters from the top of this function.
  if (!streamFailed && !st.visitorReady) {
    // Resolve the note previews first, in slices. The resolver is created once and survives
    // across yields in BuildState; `previewResolveDone` is what makes this whole block one-shot
    // for the resolve while runBuildParse re-enters from the top on every slice.
    if (st.params.inlineFootnotePreviews && !st.previewResolveDone) {
      if (!st.previewResolver && !beginInlineFootnotePreviewResolve(st)) {
        LOG_ERR("SCT", "Could not start the footnote resolve for spine %d; markers stay plain", spineIndex);
        footnotePreviewsUnresolved_ = true;
        st.previewResolveDone = true;
      }
      const uint32_t resolveSliceStart = millis();
      while (st.previewResolver) {
        const FootnotePreviews::Resolver::Step rs = st.previewResolver->step();
        if (rs == FootnotePreviews::Resolver::Step::More) {
          if (!overBudget()) {
            continue;
          }
          // Out of budget mid-resolve: bank what this slice cost and hand the loop task back.
          // The resolver keeps its position, so the next slice picks up inside the same document.
          const uint32_t spent = millis() - resolveSliceStart;
          st.previewResolveMs += spent;
          st.previewResolveMaxSliceMs = std::max(st.previewResolveMaxSliceMs, spent);
          return yieldSlice();
        }
        if (rs == FootnotePreviews::Resolver::Step::Failed) {
          // The store is untouched — the resolver rolled its append back — so the only cost is
          // that some notes in THIS build stay plain markers until the spine is built again.
          // Deliberately not fatal: a chapter that renders with unexpanded markers is worth far
          // more to the reader than a chapter that fails. Latched so a background caller can
          // throw the result away instead, since nothing would ever rebuild it — the cache is
          // keyed "previews on" either way.
          footnotePreviewsUnresolved_ = true;
          LOG_ERR("SCT", "Footnote previews unresolved for spine %d; markers stay plain in this build", spineIndex);
        }
        st.previewResolver.reset();
        st.previewResolveDone = true;
      }
      const uint32_t spent = millis() - resolveSliceStart;
      st.previewResolveMs += spent;
      st.previewResolveMaxSliceMs = std::max(st.previewResolveMaxSliceMs, spent);
    }
    st.visitorReady = true;
    finishInlineFootnotePreviewResolve(st);
    if (!st.visitor->setup(st.inflatedSize)) {
      LOG_ERR("SCT", "Failed to set up chapter parser");
      file.close();
      Storage.remove(filePath.c_str());
      if (st.cssParser) {
        st.cssParser->clear();
      }
      return BuildPhaseResult::Failed;
    }
  }

  // Phase (b): feed the visitor — from the temp file (sliced path, no ZIP state live)
  // or straight from the inflate stream (blocking path).
  if (!streamFailed) {
    if (st.useTempExtract) {
#ifdef BENCH_EXTRACT_PROFILE
      uint32_t sdReadUs = 0, visitorUs = 0;
#endif
      while (true) {
#ifdef BENCH_EXTRACT_PROFILE
        const int64_t tr = esp_timer_get_time();
#endif
        const int n = st.tempFile.read(st.chunkBuf, PARSE_CHUNK_BYTES);
#ifdef BENCH_EXTRACT_PROFILE
        sdReadUs += static_cast<uint32_t>(esp_timer_get_time() - tr);
#endif
        if (n < 0) {
          LOG_ERR("SCT", "Failed to read extracted XHTML from %s", st.tempPath.c_str());
          streamFailed = true;
          break;
        }
        if (n == 0) {
          break;
        }
        st.tempBytesFed += static_cast<size_t>(n);
        // A short write means the parser failed mid-stream (it returns 0 after an
        // internal error) — same abort the one-shot readFileToStream path performed.
#ifdef BENCH_EXTRACT_PROFILE
        const int64_t tv = esp_timer_get_time();
#endif
        const size_t wrote = st.visitor->write(st.chunkBuf, static_cast<size_t>(n));
#if SCT_HEAP_TRACE
        // Sample AFTER the write: the allocation we are hunting happens inside the parser while
        // it consumes this chunk, and by here it is either still held or already released — a
        // dip visible here is a block that outlived the chunk that created it.
        g_parseLowWater.sample(st.tempBytesFed);
#endif
#ifdef BENCH_EXTRACT_PROFILE
        visitorUs += static_cast<uint32_t>(esp_timer_get_time() - tv);
#endif
        if (wrote != static_cast<size_t>(n)) {
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
  // Latch a heap-degraded image before the visitor is torn down. Same contract as the CSS and
  // footnote latches: the cache is written either way, but a background caller can throw it away
  // and leave the spine to a build with more headroom.
  if (st.visitor->imageHeaderDegraded()) {
    imageHeaderDegraded_ = true;
  }
  if (st.cssParser) {
    st.cssParser->logResolveStats(st.localPath.c_str());
    // Latch before Finalize clears the parser (which resets its stats): lowHeapSkips
    // means pages were cached with styles silently missing.
    //
    // ...but ONLY when the resolver could actually lose a rule. An arena-RESIDENT ruleset is
    // served entirely from arena memory: lookupRule() returns from arenaResident_ before it
    // ever reads allowDiskLookup, so a lowHeapSkip there costs nothing and the styles are
    // complete. Counting those as degradation threw away correct builds — measured on X3, a
    // 14-page background build discarded after a single 960-byte dip below the lean floor,
    // forcing the released-path foreground rebuild whose 52 KB framebuffer realloc is the
    // failure that ends the session.
    cssLowHeapDegraded_ = !st.cssParser->isArenaResident() && st.cssParser->getResolveStats().lowHeapSkips > 0;
  }
  st.parseMs += millis() - sliceStart;
  SCT_TRACE_HEAP(spineIndex, "after_parse");
  LOG_INF("SCT", "createSectionFile spine=%d parse done: %ums pages=%u (stream=%d finalize=%d parser=%d free=%lu)",
          spineIndex, st.parseMs, pageCount, st.streamOk ? 1 : 0, st.finalizeOk ? 1 : 0, st.parserStreamOk ? 1 : 0,
          esp_get_free_heap_size());
  return BuildPhaseResult::Ok;
}

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
      file.close();
      Storage.remove(filePath.c_str());
      if (st.cssParser) {
        st.cssParser->clear();
      }
      // Ask the entry function to restart the whole build with embeddedStyle disabled.
      return BuildPhaseResult::RetryNoCss;
    } else {
      LOG_ERR("SCT", "Failed to parse XML and build pages (stream=%d finalize=%d parser=%d)", st.streamOk ? 1 : 0,
              st.finalizeOk ? 1 : 0, st.parserStreamOk ? 1 : 0);
      file.close();
      Storage.remove(filePath.c_str());
      if (st.cssParser) {
        st.cssParser->clear();
      }
      return BuildPhaseResult::Failed;
    }
  }
  const uint32_t fileSize = static_cast<uint32_t>(st.inflatedSize);

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT. 0 marks a failed onPageComplete; 0xFFFFFFFF is what FsFile::position()
  // degenerates to on a broken handle — neither must ever reach the cache file, where
  // it would only surface later as a failed seek at page-load time.
  for (const uint32_t& pos : st.lut) {
    if (pos == 0 || pos == UINT32_MAX) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, pos);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    file.close();
    Storage.remove(filePath.c_str());
    return BuildPhaseResult::Failed;
  }

  // Write anchor-to-page map for fragment navigation (TOC + footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();

#if SCT_HEAP_TRACE
  // Which retained container accounts for the parse's growth? The per-page trace shows the parse
  // ending ~9 KB and ~65 blocks above where it started while contig falls ~11 KB, so something is
  // held for the whole parse and reallocated as it grows. These are the three that scale with the
  // chapter rather than with the page being laid out; sizes are logged once, at the only point
  // where all of them are still alive. Temporary, same lifetime as SCT_HEAP_TRACE.
  {
    const size_t anchorBytes = anchors.size() * sizeof(std::pair<std::string, uint16_t>);
    const size_t labelBytes = visitor.getPageBreakLabels().size() * sizeof(std::pair<uint16_t, std::string>);
    const size_t lutBytes = visitor.getParagraphLutPerPage().size() * 8;
    LOG_INF("HEAP", "spine=%d retained: anchors=%u (~%uB) pageBreakLabels=%u (~%uB) paraLut=%u (~%uB) lut=%u (~%uB)",
            spineIndex, static_cast<unsigned>(anchors.size()), static_cast<unsigned>(anchorBytes),
            static_cast<unsigned>(visitor.getPageBreakLabels().size()), static_cast<unsigned>(labelBytes),
            static_cast<unsigned>(visitor.getParagraphLutPerPage().size()), static_cast<unsigned>(lutBytes),
            static_cast<unsigned>(lut.size()), static_cast<unsigned>(lut.size() * sizeof(uint32_t)));
  }
#endif

  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  // Write printed page label map for EPUB pagebreak markers.
  const uint32_t pageBreakMapOffset = file.position();
  const auto& pageBreakLabelsLocal = visitor.getPageBreakLabels();
  serialization::writePod(file, static_cast<uint16_t>(pageBreakLabelsLocal.size()));
  for (const auto& [page, label] : pageBreakLabelsLocal) {
    serialization::writePod(file, page);
    serialization::writeString(file, label);
  }

  // Write per-page paragraph LUT: count + array of {xhtmlByteOffset(u32), paragraphIndex(u16)}.
  // The byte offset lets findXPathForParagraph seek near the target paragraph without scanning
  // from the beginning of the XHTML file, reducing SD reads on large chapters.
  const uint32_t paragraphLutOffset = file.position();
  const auto& paragraphLut = visitor.getParagraphLutPerPage();
  if (paragraphLut.size() != static_cast<size_t>(pageCount)) {
    LOG_ERR("SCT", "Paragraph LUT size mismatch: lut=%u pageCount=%u", static_cast<uint32_t>(paragraphLut.size()),
            static_cast<uint32_t>(pageCount));
    file.close();
    Storage.remove(filePath.c_str());
    return BuildPhaseResult::Failed;
  }
  serialization::writePod(file, static_cast<uint16_t>(paragraphLut.size()));
  for (const auto& entry : paragraphLut) {
    serialization::writePod(file, entry.xhtmlByteOffset);
    serialization::writePod(file, entry.paragraphIndex);
    serialization::writePod(file, entry.listItemIndex);
  }

  // Patch header with final parseComplete/pageCount and offsets.
  const size_t headerPatchStart = header::kParseComplete;
  if (!file.seek(headerPatchStart)) {
    LOG_ERR("SCT", "Failed to seek to section header patch offset %u", header::kParseComplete);
    file.close();
    Storage.remove(filePath.c_str());
    return BuildPhaseResult::Failed;
  }
  serialization::writePod(file, parseComplete);
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, pageBreakMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  file.flush();

  const size_t expectedHeaderPatchEnd = headerPatchStart + sizeof(parseComplete) + sizeof(pageCount) +
                                        sizeof(lutOffset) + sizeof(anchorMapOffset) + sizeof(pageBreakMapOffset) +
                                        sizeof(paragraphLutOffset);
  if (file.position() != expectedHeaderPatchEnd) {
    LOG_ERR("SCT", "Section header patch write failed: wrote %u bytes at offset %u",
            static_cast<unsigned>(file.position() - headerPatchStart), static_cast<unsigned>(headerPatchStart));
    file.close();
    Storage.remove(filePath.c_str());
    return BuildPhaseResult::Failed;
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

  file.close();

  // Cache the LUT in memory and open the file for reading so that
  // subsequent loadPageFromSectionFile() calls can seek directly without re-opening.
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    LOG_ERR("SCT", "Failed to open section file for reading after creation");
    return BuildPhaseResult::Failed;
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

bool Section::createSectionFile(const BuildParams& p, const std::function<void(int)>& progressFn,
                                const bool skipEviction) {
  if (!skipEviction) {
    evictOldVariants();
  }

  // Run-to-completion path: pump the incremental build with no time budget. budgetMs == 0
  // never yields mid-parse, so the only More this loop sees is the restart after a
  // RetryNoCss downgrade — foreground behaviour is unchanged. If a background build for
  // this exact variant is already in flight, the pump resumes it instead of starting over.
  while (true) {
    switch (stepSectionBuild(p, /*budgetMs=*/0, progressFn, skipEviction)) {
      case BuildStep::Done:
        return true;
      case BuildStep::Failed:
        return false;
      default:
        continue;
    }
  }
}

void Section::setExternalBuildScratch(BuildArena* scratch) {
  // A build binds its arena exactly once, in initArena() at startBuild(). So if a build is
  // already live on the small owned arena and the large external region (the borrowed
  // framebuffer) only becomes available now, the build cannot adopt it — it keeps the small one
  // and keeps failing to fit things the region would have held comfortably.
  //
  // Device-observed on Small Gods: spine 1 (one 583991-byte entry) started with no framebuffer
  // free, so it took the 10240-byte owned arena. Background-C then borrowed the 52272-byte
  // framebuffer, but the resumed build still had the 10240, so its 33824-byte inflate window was
  // evicted to the heap — where 30708 contiguous was not enough. Three attempts failed and the
  // borrow bought nothing.
  //
  // Restart instead. Cheap: abortSectionBuild() deliberately keeps the inflated-HTML cache, so
  // the restart skips re-inflation and only redoes setup.
  const bool upgradesArena = scratch != nullptr && scratch->valid() && buildState_ && buildState_->arena != nullptr &&
                             buildState_->arena == buildState_->ownedArena.get();
  externalScratch_ = scratch;
  if (upgradesArena) {
    LOG_INF("SCT", "spine=%d: build scratch arrived mid-build (%u bytes); restarting to use it", spineIndex,
            static_cast<uint32_t>(scratch->capacity()));
    abortSectionBuild();
  }
}

bool Section::heapAllowsEmbeddedStyle(const size_t cssRuleCount, const bool arenaBacked) {
  // An arena-backed build takes the ruleset from the BUILD ARENA, not the heap, so no heap
  // floor applies to it. Measured X3 (alice, 94 rules): the resident ruleset costs 752 index
  // + 1306 pool = ~2 KB out of a 52272 B arena whose whole-build high water was 28652 B.
  // The floors below were sized for the heap-vector layout and predate the arena; leaving
  // them in front of an arena build made the decision a coin flip on heap noise — the X3
  // trace has spine 2 passing and spine 3 refused at free=57300 vs a 57344 floor, 44 bytes
  // apart, in the same boot on the same book. That is not a memory decision, and it is
  // expensive: the two outcomes write different cache variants (`N_b93d54e8` vs
  // `N_886d5d5f`), so the reader misses the variant it wants and rebuilds the spine.
  // The arena path also has a real fallback — every failure in the resident loader does
  // `indexArena_->release(block); return false;` and drops back to the disk-backed index.
  if (arenaBacked) return true;

  // Heap-backed builds still need a gate: there the index is a std::vector reserve, and a
  // failed reserve under -fno-exceptions aborts rather than falling back.
  //
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
  // Same predicate runBuildSetup() uses to decide setIndexArena(): an external scratch region
  // (the borrowed framebuffer) means the ruleset lands in the arena, not the heap.
  const bool arenaBacked = externalScratch_ && externalScratch_->valid();
  if (p.embeddedStyle && !heapAllowsEmbeddedStyle(css ? css->ruleCount() : 0, arenaBacked)) {
    // Report the floors, not just the heap state: this gate silently downgrades a book to a
    // no-CSS section cache (different layout, different cache key), and the X3 trace showed it
    // firing on spine 2 where the arena still had ~18 KB spare. See
    // docs/memory-allocation-strategy.md — the floors predate the build arena.
    const uint32_t requiredContig = std::max<uint32_t>(
        EMBEDDED_STYLE_MIN_CONTIG_HEAP_BYTES,
        static_cast<uint32_t>((css ? css->ruleCount() : 0) * CssParser::CSS_INDEX_BYTES_PER_RULE) + 8 * 1024);
    LOG_INF("SCT",
            "Low heap for embedded CSS (free=%lu(floor=%lu) contig=%lu(floor=%lu) rules=%u); building no-CSS section "
            "cache",
            static_cast<unsigned long>(esp_get_free_heap_size()),
            static_cast<unsigned long>(EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)),
            static_cast<unsigned long>(requiredContig), static_cast<unsigned>(css ? css->ruleCount() : 0));
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

  if (runBuildSetup(*buildState_) != BuildPhaseResult::Ok) {
    buildState_.reset();
    return false;
  }
  return true;
}

Section::BuildStep Section::stepSectionBuild(const BuildParams& params, const uint32_t budgetMs,
                                             const std::function<void(int)>& progressFn, const bool skipEviction) {
  const uint32_t requestedHash = calculatePropertyHash(params);
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
    return fin == BuildPhaseResult::Done ? BuildStep::Done : BuildStep::Failed;
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
  // HTML cache, and the only version worth deleting is an INCOMPLETE one: a partial inflate is
  // garbage, a finished one is exactly what the next attempt wants.
  //
  // This used to test `!reusedHtml` — "delete whenever WE produced it" — which threw away every
  // complete extraction a preempted build had just paid for, because a build that produced the
  // file has reusedHtml == false whether or not its inflate finished. That silently broke the
  // retry design documented at BG_BUILD_MAX_PREEMPTIONS: attempt 1 is supposed to bank the
  // inflated XHTML so attempt 2 skips inflation and gets a real shot at the parse. It never did.
  // Attempt 2 re-inflated from scratch, lost the same race, and Background-B abandoned the spine
  // — which is why B stopped pre-building after its first section or two.
  //
  // Device-observed (X3, 2026-08-11): spine 2 extracted 14054 bytes at t=16924, was preempted at
  // t=17087 with phase (a) complete, and the cache was deleted anyway.
  //
  // extractDone covers both ways the file becomes complete: the reused-cache path sets it when it
  // skips phase (a), and the extract loop sets it when the inflate finishes. Erring towards
  // keeping is self-correcting — runBuildParse re-validates the cache against the entry's
  // inflated size and re-inflates on any mismatch.
  if (buildState_->tempFile) {
    buildState_->tempFile.close();
  }
  if (!buildState_->extractDone && !buildState_->tempPath.empty()) {
    Storage.remove(buildState_->tempPath.c_str());
  }
  // During a live build, `file` is the build's write handle and filePath its cache path
  // (both set by runBuildSetup). The header was never patched, so remove the file.
  // An abort before runBuildSetup got as far as opening it leaves the handle untouched, and
  // closing an unopened handle asserts.
  if (file) file.close();
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
  //
  // An arena-RESIDENT ruleset never loses a rule to heap pressure (see the note at the
  // cssLowHeapDegraded_ latch in runBuildParse), so its skips must not abort the build. This
  // is the mid-parse half of the same rule: Background-B polls this every slice and discards
  // the whole build the first time it returns true.
  if (!buildState_ || !buildState_->cssParser) return false;
  if (buildState_->cssParser->isArenaResident()) return false;
  return buildState_->cssParser->getResolveStats().lowHeapSkips > 0;
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

// Scratch budget for a whole warm pass: the largest per-decode working set on either path —
// PNG's ≤32 KB inflate ring plus two scanline buffers, or JPEG's 12 KB work pool — with slack
// for per-allocation alignment. Sized once for the pass rather than per image, because a bump
// arena reuses the same bytes for every decode; the peak is one decode's needs, not N.
//
// This is the fix for the churn described in docs/memory-allocation-strategy.md rule 4: without
// it, each of the N decodes in a pass takes and returns its own 12-32 KB block, breaking up the
// contiguous region the caller must later hand back to the framebuffer.
static constexpr size_t WARM_PASS_SCRATCH_BYTES = 32 * 1024 + 2 * 4096 + 256;

void Section::warmAllImageCaches(const int xOffset, const int yOffset, const bool forceLoad,
                                 const bool monochromeOutput, const bool alsoWarmGrayscale) {
  if (pageCount == 0) return;

  // Prefer the LENT framebuffer region (externalScratch_) over a fresh heap block. Asking the
  // heap for ~40 KB contiguous is the very thing this pass is trying not to do — on X3 the
  // largest free block after boot is 42996, so that allocation is both likely to fail and, when
  // it succeeds, likely to be the one that splits the region the framebuffer needs back. The
  // borrowed region is already ours, costs nothing to reuse, and cannot fail to be returned.
  //
  // Only usable when nothing else is building out of it: a live build owns the arena cursor,
  // and a decode bump-allocating underneath it would corrupt that scope.
  BuildArena* lent = (externalScratch_ && externalScratch_->valid() && !hasActiveBuild()) ? externalScratch_ : nullptr;

  // Fall back to a heap arena only when no region is lent. Best-effort either way: with no
  // arena at all the decoders use their own heap allocations and the pass behaves exactly as
  // before — no correctness change, just without the anti-fragmentation benefit.
  std::unique_ptr<BuildArena> owned;
  if (!lent) {
    owned = makeUniqueNoThrow<BuildArena>(WARM_PASS_SCRATCH_BYTES);
    if (!owned || !owned->valid()) {
      owned.reset();
      LOG_DBG("SCT", "warmAllImageCaches: no scratch arena (%u bytes); decoding from heap",
              static_cast<uint32_t>(WARM_PASS_SCRATCH_BYTES));
    }
  }
  BuildArena* scratchArena = lent ? lent : owned.get();
  if (scratchArena) scratchArena->reset();  // the pass owns the whole cursor for its duration
  image_scratch::ScopedArena scratchScope(scratchArena);

  const int savedPage = currentPage;
  int warmed = 0;
  for (int p = 0; p < static_cast<int>(pageCount); ++p) {
    currentPage = p;
    auto page = loadPageFromSectionFile();
    if (!page || !page->hasImages()) continue;
    page->warmImageCaches(renderer, xOffset, yOffset, forceLoad, monochromeOutput, alsoWarmGrayscale);
    ++warmed;
    // Each image decode can take hundreds of ms; reset the WDT between pages
    // to avoid an interrupt watchdog timeout on image-heavy chapters.
    esp_task_wdt_reset();
  }
  currentPage = savedPage;
  if (warmed > 0) {
    // highWater/failedAlloc tell you whether the scratch budget is right: a non-zero failedAlloc
    // means decodes fell back to the heap (i.e. the churn is still there). `src` says which
    // region served the pass — "lent" is the one that costs no contiguous heap.
    LOG_DBG("SCT", "warmAllImageCaches: warmed %d page(s) with images (scratch src=%s highWater=%u failedAlloc=%u)",
            warmed, lent ? "lent" : (owned ? "heap" : "none"),
            scratchArena ? static_cast<uint32_t>(scratchArena->highWater()) : 0u,
            scratchArena ? static_cast<uint32_t>(scratchArena->failedAllocSize()) : 0u);
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
