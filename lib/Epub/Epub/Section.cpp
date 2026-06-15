#include "Section.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint8_t SECTION_FILE_VERSION =
    51;  // bumped: long (>96-word) paragraphs beside a tall float now wrap in the mid-block
         // split path too; discards v50 caches where such paragraphs overflowed the image

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

// Inflate-feed chunk for the sliced parse: same size readFileToStream used, and the
// granularity at which runBuildParse checks its time budget between visitor writes.
constexpr size_t PARSE_CHUNK_BYTES = 1024;

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
                                        uint8_t imageRendering) {
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
  append(&imageRendering, sizeof(imageRendering));

  return fnv1a(buffer, offset);
}

std::string Section::getSectionFilePath(uint32_t propertyHash) const {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d_%08x", spineIndex, propertyHash);
  return epub->getCachePath() + "/sections/" + buf + ".bin";
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
  LOG_DBG("SCT", "Page %d processed", pageCount);

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
                              const bool bionicReadingEnabled, const uint8_t imageRendering) {
  truncatedCache = false;
  embeddedStyleFallback = false;
  uint32_t propertyHash =
      calculatePropertyHash(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                            viewportHeight, hyphenationEnabled, embeddedStyle, bionicReadingEnabled, imageRendering);
  filePath = getSectionFilePath(propertyHash);

  bool usingEmbeddedStyleFallback = false;
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    // Fallback: allow loading a no-CSS cache variant when embedded CSS is enabled.
    if (embeddedStyle) {
      const uint32_t fallbackHash =
          calculatePropertyHash(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                                viewportHeight, hyphenationEnabled, false, bionicReadingEnabled, imageRendering);
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
  CssParser* cssParser = nullptr;
  std::vector<uint32_t> lut;
  std::unique_ptr<ChapterHtmlSlimParser> visitor;
  // Live ZIP-side state of the parse, created on the first runBuildParse call and
  // released the moment the compressed stream is exhausted. zip must outlive reader
  // (reader holds a reference to it). chunkBuf (PARSE_CHUNK_BYTES) is the feed buffer —
  // heap because it must survive across slices. The inflate ring inside the reader is
  // sized to the entry (≤32 KB).
  std::unique_ptr<ZipFile> zip;
  std::unique_ptr<ZipFile::EntryReader> reader;
  std::unique_ptr<uint8_t[]> chunkBuf;
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
  FsFile tempFile;
  std::string tempPath;
  size_t tempBytesFed = 0;
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

// Out-of-line (see header): both need the complete BuildState type, and the dtor must
// not leave a partially written cache file behind when a Section dies with a build in
// flight (book close, activity exit) — its header was never patched with offsets.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub), spineIndex(spineIndex), renderer(renderer) {}
Section::~Section() { abortSectionBuild(); }

Section::BuildPhaseResult Section::runBuildSetup(BuildState& st) {
  const BuildParams& p = st.params;
  st.propertyHash = calculatePropertyHash(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment,
                                          p.viewportWidth, p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle,
                                          p.bionicReadingEnabled, p.imageRendering);
  filePath = getSectionFilePath(st.propertyHash);

  st.localPath = epub->getSpineItem(spineIndex).href;
  LOG_INF("SCT", "createSectionFile spine=%d start: %s (free=%lu)", spineIndex, st.localPath.c_str(),
          esp_get_free_heap_size());

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Get inflated size up-front so the parser can choose progress granularity.
  const uint32_t phaseSetupStart = millis();
  st.inflatedSize = 0;
  if (!epub->getSpineItemInflatedSize(spineIndex, &st.inflatedSize)) {
    LOG_ERR("SCT", "Failed to get inflated size for %s", st.localPath.c_str());
    return BuildPhaseResult::Failed;
  }

  // Reset build state — createSectionFile may be called on a Section that previously
  // loaded a cache (e.g. fallback no-CSS file). pageCount must start at 0 so that
  // onPageComplete() numbering and paragraphLutPerPage stay in lockstep.
  file.close();
  pageCount = 0;
  this->lut.clear();
  cssLowHeapDegraded_ = false;

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return BuildPhaseResult::Failed;
  }
  writeSectionFileHeader(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
                         p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle, p.bionicReadingEnabled,
                         p.imageRendering);
  st.lut.clear();

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = st.localPath.find_last_of('/');
  st.contentBase = (lastSlash != std::string::npos) ? st.localPath.substr(0, lastSlash + 1) : "";
  st.imageBasePath = getImageBasePath(st.propertyHash);

  st.cssParser = nullptr;
  if (p.embeddedStyle) {
    st.cssParser = epub->getCssParser();
    if (st.cssParser) {
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
  st.visitor->setHeadingFonts(p.headingFonts.fontId, p.headingFonts.residual);
  Hyphenator::setPreferredLanguage(epub->getLanguage());

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
    st.useTempExtract = budgetMs != 0;
    // Heap, not stack: these must survive across slices. Ring (≤32 KB, sized to the
    // entry) + 2× PARSE_CHUNK_BYTES scratch — the same buffers readFileToStream held
    // for the whole stream, so net-neutral vs the old one-shot path.
    st.zip.reset(new (std::nothrow) ZipFile(epub->getPath()));
    st.chunkBuf.reset(new (std::nothrow) uint8_t[PARSE_CHUNK_BYTES]);
    if (st.zip && st.chunkBuf) {
      st.reader.reset(new (std::nothrow) ZipFile::EntryReader(*st.zip, PARSE_CHUNK_BYTES));
    }
    if (!st.reader) {
      LOG_ERR("SCT", "Failed to allocate entry reader (%u bytes scratch, free=%lu)",
              static_cast<uint32_t>(PARSE_CHUNK_BYTES), esp_get_free_heap_size());
      streamFailed = true;
    } else {
      const std::string entryPath = FsHelpers::normalisePath(st.localPath);
      if (!st.reader->open(entryPath.c_str())) {
        streamFailed = true;  // EntryReader::open already logged the cause
      }
    }
    if (!streamFailed && st.useTempExtract) {
      st.tempPath = filePath + ".xtmp";
      if (!Storage.openFileForWrite("SCT", st.tempPath, st.tempFile)) {
        streamFailed = true;
      }
    }
  }

  // Phase (a), sliced path only: inflate the entry to a temp SD file, then release all
  // ZIP state. Keeps the ring and the parser's layout working set temporally disjoint.
  if (!streamFailed && st.useTempExtract && !st.extractDone) {
    bool done = false;
    while (!done) {
      size_t produced = 0;
      if (!st.reader->step(st.chunkBuf.get(), PARSE_CHUNK_BYTES, &produced, &done)) {
        streamFailed = true;
        break;
      }
      if (produced > 0 && st.tempFile.write(st.chunkBuf.get(), produced) != produced) {
        LOG_ERR("SCT", "Failed to write extracted XHTML to %s", st.tempPath.c_str());
        streamFailed = true;
        break;
      }
      if (!done && overBudget()) {
        return yieldSlice();
      }
    }
    if (!streamFailed && st.reader->bytesProduced() != st.inflatedSize) {
      LOG_ERR("SCT", "Decompressed size mismatch (expected %u, got %u)", static_cast<uint32_t>(st.inflatedSize),
              static_cast<uint32_t>(st.reader->bytesProduced()));
      streamFailed = true;
    }
    st.reader.reset();
    st.zip.reset();
    st.tempFile.flush();
    st.tempFile.close();
    st.extractDone = true;
    if (!streamFailed) {
      if (!Storage.openFileForRead("SCT", st.tempPath, st.tempFile)) {
        streamFailed = true;
      } else {
        LOG_INF("SCT", "createSectionFile spine=%d extracted %u bytes to temp (free=%lu)", spineIndex,
                static_cast<uint32_t>(st.inflatedSize), esp_get_free_heap_size());
        if (overBudget()) {
          return yieldSlice();
        }
      }
    }
  }

  // Phase (b): feed the visitor — from the temp file (sliced path, no ZIP state live)
  // or straight from the inflate stream (blocking path).
  if (!streamFailed) {
    if (st.useTempExtract) {
      while (true) {
        const int n = st.tempFile.read(st.chunkBuf.get(), PARSE_CHUNK_BYTES);
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
        if (st.visitor->write(st.chunkBuf.get(), static_cast<size_t>(n)) != static_cast<size_t>(n)) {
          streamFailed = true;
          break;
        }
        if (overBudget()) {
          return yieldSlice();
        }
      }
      if (!streamFailed && st.tempBytesFed != st.inflatedSize) {
        LOG_ERR("SCT", "Extracted size mismatch (expected %u, fed %u)", static_cast<uint32_t>(st.inflatedSize),
                static_cast<uint32_t>(st.tempBytesFed));
        streamFailed = true;
      }
    } else {
      bool done = false;
      while (!done) {
        size_t produced = 0;
        if (!st.reader->step(st.chunkBuf.get(), PARSE_CHUNK_BYTES, &produced, &done)) {
          streamFailed = true;
          break;
        }
        if (produced > 0 && st.visitor->write(st.chunkBuf.get(), produced) != produced) {
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
  st.reader.reset();
  st.zip.reset();
  st.chunkBuf.reset();
  if (st.tempFile) {
    st.tempFile.close();
  }
  if (!st.tempPath.empty()) {
    Storage.remove(st.tempPath.c_str());
    st.tempPath.clear();
  }
  st.streamOk = !streamFailed;
  st.finalizeOk = st.visitor->finalize();
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
  return BuildPhaseResult::Done;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const bool bionicReadingEnabled, const uint8_t imageRendering,
                                const std::function<void(int)>& progressFn, const bool skipEviction,
                                const HeadingFonts& headingFonts) {
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
  params.imageRendering = imageRendering;
  params.headingFonts = headingFonts;

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
  const uint32_t requestedHash = calculatePropertyHash(
      params.fontId, params.lineCompression, params.extraParagraphSpacing, params.paragraphAlignment,
      params.viewportWidth, params.viewportHeight, params.hyphenationEnabled, params.embeddedStyle,
      params.bionicReadingEnabled, params.imageRendering);
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
  // Drop the extraction temp file alongside the partial cache file.
  if (buildState_->tempFile) {
    buildState_->tempFile.close();
  }
  if (!buildState_->tempPath.empty()) {
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
