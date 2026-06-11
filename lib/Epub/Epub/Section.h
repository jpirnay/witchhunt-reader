#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;
  std::vector<uint32_t> lut;  // Cached page byte-offsets; loaded once, avoids per-page LUT seek
  bool truncatedCache = false;
  bool embeddedStyleFallback = false;
  // Set by the last build when CssParser hit its own low-heap mode mid-parse
  // (lowHeapSkips > 0): some elements were cached without their styles. The cache is
  // usable but visually degraded; background callers discard it so the foreground
  // blocking path (more headroom) rebuilds it clean.
  bool cssLowHeapDegraded_ = false;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, bool bionicReadingEnabled, uint8_t imageRendering);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

  struct TocBoundary {
    int tocIndex = 0;
    uint16_t startPage = 0;
  };
  std::vector<TocBoundary> tocBoundaries;
  std::vector<std::pair<uint16_t, std::string>> pageBreakLabels;

  void buildTocBoundaries(const std::vector<std::pair<std::string, uint16_t>>& anchors);
  void buildTocBoundariesFromFile(FsFile& f);
  void buildPageBreakLabelsFromFile(FsFile& f);

  // Live state of an in-progress section build, shared by the blocking path and the
  // sliceable stepSectionBuild() path. Holds exactly the locals that must survive across
  // build phases (and, for the sliced path, across loop ticks). Declared here but defined
  // in Section.cpp so the heavy parser type stays out of this header.
  struct BuildState;
  // In-flight incremental build, owned across stepSectionBuild() calls. Null when no build
  // is live. Heap-owned so the visitor's &lut capture stays stable across ticks.
  std::unique_ptr<BuildState> buildState_;
  // Outcome of one phase method. Mostly maps to BuildStep: More means the phase yielded
  // mid-way after spending its time budget; RetryNoCss asks the entry function to tear the
  // state down and restart from setup with embeddedStyle=false.
  enum class BuildPhaseResult : uint8_t { Ok, More, Done, Failed, RetryNoCss };
  // Phase methods. Each operates on the shared BuildState and is purely linear (the
  // CSS/heap fallback recursion lives in the entry function, not here), which is what lets
  // them be called either back-to-back (blocking) or with Parse re-entered across ticks.
  BuildPhaseResult runBuildSetup(BuildState& st);
  // Drives the ZIP entry reader → parser feed loop. When budgetMs is 0 the whole stream is
  // consumed in one call (blocking path); otherwise returns More once the budget is spent,
  // with the live reader+visitor retained in BuildState for the next call.
  BuildPhaseResult runBuildParse(BuildState& st, uint32_t budgetMs);
  BuildPhaseResult runBuildFinalize(BuildState& st);

  // Open the section file and seek to the first paragraph LUT entry, validating the header
  // and LUT bounds against fileSize. On success, returns true with `outLutStart` set to the
  // byte offset of the first entry (just past the count) and `outCount` to the entry count.
  // Caller is responsible for closing `outFile`. Returns false on any I/O or validation error.
  bool readParagraphLutHeader(FsFile& outFile, uint16_t& outCount, uint32_t& outLutStart) const;

  // Calculates a stable hash for a given set of rendering properties.
  // Used to suffix cache files so multiple variants can coexist safely without constant recompilation.
  static uint32_t calculatePropertyHash(int fontId, float lineCompression, bool extraParagraphSpacing,
                                        uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                                        bool hyphenationEnabled, bool embeddedStyle, bool bionicReadingEnabled,
                                        uint8_t imageRendering);

  // Computes the active file path for this section based on rendering properties
  std::string getSectionFilePath(uint32_t propertyHash) const;
  // Computes the image base path for extract images related to this specific section variant
  std::string getImageBasePath(uint32_t propertyHash) const;
  // Garbage collection: Keep only the most recent N variants per chapter
  void evictOldVariants() const;

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Resolved heading fonts for h1/h2/h3, computed by the app layer (which knows the body
  // font family/size) and passed down as pure data so the Epub lib stays settings-agnostic.
  // For each level: fontId != 0 means "render this heading with that taller built-in font"
  // and `residual` is a small extra scale (usually 1.0) applied on top. fontId == 0 means
  // "scale the body font by `residual`" (the nearest-neighbor fallback for SD fonts / caps).
  // Index 0 = h1, 1 = h2, 2 = h3. Default-constructed = all-fallback (current behavior).
  struct HeadingFonts {
    // 32-bit font-id hashes (see fontIds.h) — must NOT be truncated to a byte, or the
    // heading font lookup fails and headings render nothing. 0 = scale-fallback for that level.
    int32_t fontId[3] = {0, 0, 0};
    float residual[3] = {1.6f, 1.4f, 1.2f};
  };

  // Render parameters that determine a section-cache variant. Bundles the long argument
  // list shared by loadSectionFile / createSectionFile / stepSectionBuild so a resumable
  // build can carry them across slices without re-passing ten arguments each call.
  struct BuildParams {
    int fontId = 0;
    float lineCompression = 1.0f;
    bool extraParagraphSpacing = false;
    uint8_t paragraphAlignment = 0;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    bool hyphenationEnabled = false;
    bool embeddedStyle = false;
    bool bionicReadingEnabled = false;
    uint8_t imageRendering = 0;
    // Heading fonts derived from the body font (see HeadingFonts). Not part of the property
    // hash: they are a deterministic function of fontId, which is already hashed.
    HeadingFonts headingFonts;
  };

  // Progress of an incremental (sliceable) section build. Setup/Parse/Finalize are the
  // three temporal phases of createSectionFile; More means the current phase yielded
  // mid-way after spending its time budget and should be resumed on the next call.
  // Done/Failed are terminal. See docs/epubreader-control-flow-refactor.md §2.6–2.7.
  enum class BuildStep : uint8_t { Setup, Parse, Finalize, More, Done, Failed };

  // Ctor/dtor defined in Section.cpp: both need the complete BuildState type (the
  // unique_ptr member's deleter is instantiated in each), and the dtor must abort a still
  // in-flight incremental build so its partially written cache file doesn't survive.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       bool bionicReadingEnabled, uint8_t imageRendering);
  bool clearCache();
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         bool bionicReadingEnabled, uint8_t imageRendering, const std::function<void(int)>& progressFn,
                         bool skipEviction, const HeadingFonts& headingFonts);

  // Incremental section-cache build. Advances the build by at most ~budgetMs of work
  // (budgetMs == 0 means no budget: run to a terminal state in one call) and returns More
  // when work remains. The caller invokes it repeatedly (typically from idle time) until
  // Done or Failed; the build state is owned by this Section across calls. Yield
  // granularity is one ~1 KB inflate-feed chunk, so a slice never interrupts a page's SD
  // write and overshoots the budget by at most one chunk's layout work.
  //
  // Re-entry contract: a later call whose params hash differs from the original request
  // discards the partial build (the partial cache is for the wrong variant) and starts
  // fresh. progressFn/skipEviction only apply when a fresh build is started.
  BuildStep stepSectionBuild(const BuildParams& params, uint32_t budgetMs,
                             const std::function<void(int)>& progressFn = {}, bool skipEviction = false);
  // True while an incremental build is in flight (stepSectionBuild returned More).
  bool hasActiveBuild() const { return static_cast<bool>(buildState_); }
  // Percent of the spine XHTML consumed by the in-flight build (0–100; 100 once the
  // stream is exhausted and only Finalize remains). 0 when no build is live. Feeds the
  // DEBUG_BACKGROUND_WORK overlay.
  int activeBuildPercent() const;
  // Discard an in-flight incremental build: deletes the partially written cache file
  // (its header was never patched, so it must not be left for loadSectionFile to find)
  // and drops the live parser/inflate state. No-op when no build is live. Call on
  // non-consecutive navigation or when render params change.
  void abortSectionBuild();
  // True when current heap headroom permits an embedded-CSS build (the gate startBuild
  // applies before silently downgrading to no-CSS). Exposed so a background caller can
  // refuse to start instead — a downgraded background build would produce the fallback
  // variant and the foreground would still rebuild with CSS, wasting the work.
  static bool heapAllowsEmbeddedStyle();
  std::unique_ptr<Page> loadPageFromSectionFile();
  // Pre-decode every image in the section into its .pxc cache while heap is
  // maximally contiguous (secondary display buffer still released). Skips images
  // that are already cached or would show as a placeholder. The decode writes
  // pixels into the framebuffer as a side effect; call renderer.clearScreen()
  // afterward. forceLoad mirrors the effectiveForceLoad rule used at render time.
  void warmAllImageCaches(int xOffset, int yOffset, bool forceLoad, bool monochromeOutput = true);
  bool isTruncatedCache() const { return truncatedCache; }
  bool isEmbeddedStyleFallback() const { return embeddedStyleFallback; }
  // True when the last build's CSS resolution hit low-heap skips (styles silently
  // missing from the cached pages). Only meaningful right after a build.
  bool isCssLowHeapDegraded() const { return cssLowHeapDegraded_; }

  // Given a page in this section, return the TOC index for that page.
  int getTocIndexForPage(int page) const;
  // Given a TOC index, return the start page in this section.
  // Returns nullopt if the TOC index doesn't map to a boundary in this spine (e.g. belongs to a different spine).
  std::optional<int> getPageForTocIndex(int tocIndex) const;

  struct TocPageRange {
    int startPage;  // inclusive
    int endPage;    // exclusive
  };
  // Returns the page range [start, end) within this spine that belongs to the given TOC index.
  std::optional<TocPageRange> getPageRangeForTocIndex(int tocIndex) const;

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  // Returns the printed-page label for a rendered page, wrapped in parens (e.g. "(42)"), if
  // one or more EPUB pagebreak markers / NCX <pageList> / page-map entries land on it.
  // Returns nullopt when no printed-page anchor falls on this exact page.
  std::optional<std::string> getPrintedPageLabelForPage(uint16_t page) const;
  // Like getPrintedPageLabelForPage but returns the most recent printed-page label at or
  // before `page` (raw label, no parens). Useful for pre-filling jump-to-page dialogs when
  // the current rendered page doesn't itself carry an anchor. Returns nullopt when no
  // printed-page anchor exists on this or any earlier page in the section.
  std::optional<std::string> getNearestPrintedPageLabelAtOrBefore(uint16_t page) const;

  // Standalone lookup that doesn't require a loaded Section. Walks the book's sections cache
  // directory, finds any cache variant for `spineIndex`, reads its printed-page label map,
  // and returns the parenthesised label for `page` if one is recorded. Returns nullopt when
  // no cache exists or the page carries no printed-page anchor. Used by SleepActivity to
  // augment the overlay without instantiating a full Section + render parameters.
  static std::optional<std::string> getPrintedPageLabelFromCache(const std::string& sectionsDir, int spineIndex,
                                                                 uint16_t page);

  // Look up the page number for a paragraph index (1-based, from XPath p[N]).
  // Uses the per-page paragraph LUT stored in the section cache.
  // Returns nullopt if the paragraph LUT is not available (old cache format).
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running <li> index (1-based, the Nth <li> at any depth
  // in the chapter). Used to snap KOReader-supplied list-item XPaths to a precise page
  // the same way getPageForParagraphIndex handles <p>-anchored XPaths.
  // Returns nullopt if the LUT is not available or the index is out of range.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the paragraph index for a given page number.
  // Returns the 1-based paragraph index of the last <p> element on or before the page.
  // Returns nullopt if the paragraph LUT is not available (old cache format).
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Look up the XHTML byte offset recorded at the page break that started the given page.
  // This is the Expat byte position within the decompressed spine XHTML file — useful as a
  // seek hint for findXPathForParagraph to avoid scanning from byte 0 on large chapters.
  // Returns nullopt if the paragraph LUT is unavailable (old cache format) or offset is 0
  // (last page, recorded after parse completion).
  std::optional<uint32_t> getXhtmlByteOffsetForPage(uint16_t page) const;

 private:
  // Allocates buildState_ and runs Setup. Applies the low-heap embedded-CSS downgrade.
  // requestedHash is the property hash of the params as requested by the caller (before
  // any CSS downgrade) — used to detect stale partial builds on later calls.
  // Returns false (with buildState_ cleared) on failure.
  bool startBuild(const BuildParams& params, const std::function<void(int)>& progressFn, uint32_t requestedHash);
};
