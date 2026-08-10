#pragma once

#include <HalStorage.h>

#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CssStyle.h"

class BuildArena;  // lib/Memory — optional backing store for the disk index (Phase 2)

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  struct ResolveStats {
    uint32_t resolveCalls = 0;
    uint32_t lowHeapSkips = 0;
    uint32_t lowHeapRescuedHits = 0;
    uint32_t lowHeapDiskBypasses = 0;
    uint32_t mapHits = 0;
    uint32_t hotHits = 0;
    uint32_t diskHits = 0;
    uint32_t misses = 0;
    uint32_t negativeHits = 0;
  };

  // True when the whole ruleset (index + style pool) is resident in arena memory, so every
  // lookupRule() is served without touching SD. Callers use this to tell a REAL low-heap
  // degradation from a harmless one: under heap pressure resolveStyle() still counts a
  // lowHeapSkip and passes allowDiskLookup=false, but the resident branch in lookupRule()
  // returns before that flag is ever read — the styles come back complete either way.
  // Treating those skips as degradation discards correct work (measured on X3: a 14-page
  // background build thrown away, forcing the released-path rebuild that then fails its
  // framebuffer realloc).
  bool isArenaResident() const { return arenaResident_ != nullptr; }

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  // v8: v7 builds wrote saveToCache() payloads without the cssFloat byte the reader
  //     expects — bump invalidates those malformed caches.
  // v9: header gains a 4-byte indexOffset field; a pre-sorted {hash,offset} array is
  //     appended after the rule payloads so ensureCacheIndexLoaded() can bulk-read the
  //     entire index in one seek instead of scanning every selector record individually.
  // v10: indexOffset field removed; the sorted index is placed immediately after the
  //      11-byte header (before rule payloads), so ensureCacheIndexLoaded() reads header
  //      + index sequentially from file position 0 — no seek over the 37KB rule block.
  // v11: style payload gains a trailing smallCaps byte (font-variant: small-caps).
  // v12: ID selectors (#id, tag#id) are now stored and resolved.
  // v13: style payload gains fontSizeMultiplier (float + defined byte). Before this,
  //      font-size from stylesheets was parsed but silently DROPPED on device because
  //      every rule round-trips through this cache (only inline style="" survived).
  // v14: style payload gains a block-flags byte persisting listStyleNone and
  //      pageBreakBefore/After — the same dropped-through-the-cache defect as v13, so
  //      stylesheet-driven list markers and page breaks now actually reach the parser.
  // v15: rules with no renderer-supported declarations are omitted from the cache.
  static constexpr uint8_t CSS_CACHE_VERSION = 15;

  // Retained RAM per rule in disk-backed lookup mode (the sorted SelectorEntry index).
  // Heap gates (Section::heapAllowsEmbeddedStyle) size their contiguous-block floor
  // from this; a static_assert in CssParser.cpp pins it to sizeof(SelectorEntry).
  static constexpr size_t CSS_INDEX_BYTES_PER_RULE = 8;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found)
   */
  bool loadFromStream(FsFile& source);

  /**
   * Look up the style for an HTML element, considering tag name, class, and id attributes.
   * Applies CSS cascade: element < class < element.class < #id < tag#id
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @param idAttr The id attribute value (empty string if absent)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(const std::string& tagName, const std::string& classAttr,
                                      const std::string& idAttr = {}) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(const std::string& styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const;

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const;

  /**
   * Clear all loaded rules
   */
  void clear();

  /**
   * Clear per-section caches (hot/negative LRU) between section builds.
   * Retains the disk index in RAM if it fits within 10 KB, avoiding a cold SD
   * re-read (~240 ms) on the next section. Evicts if larger to protect heap.
   *
   * evictEverything=true is the defragmentation variant: it also drops the
   * retained disk index (lazily re-read from SD on the next resolve) and swaps
   * the unordered containers down so their bucket arrays are returned to the
   * heap — plain clear() keeps buckets allocated, and those small blocks can
   * pin the released-framebuffer hole a secondary-buffer realloc needs back.
   */
  void clearCaches(bool evictEverything = false);

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache() const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache();

  // Low-memory CSS compilation pipeline:
  // - beginCacheCompile(): starts streaming compile mode
  // - appendCompiledFromStream(): parses one stylesheet stream into compile staging
  // - endCacheCompile(): finalizes cache file from staged records
  bool beginCacheCompile();
  bool appendCompiledFromStream(FsFile& source);
  bool endCacheCompile();
  // Abort a streaming compile (e.g. a stylesheet was skipped for low heap): discards the staged
  // temp file and persists NOTHING, so hasCache() stays false and the next open re-parses.
  // Persisting a partial compile is poison: an empty/incomplete index loads as VALID on every
  // later open, permanently stripping the book's styles until the cache is manually cleared.
  void abortCacheCompile();

  // CSS lookup telemetry helpers for tuning memory/caching behavior on-device.
  void resetResolveStats() const;
  [[nodiscard]] ResolveStats getResolveStats() const;
  void logResolveStats(const char* context) const;

  // Resident-arena memory breakdown (valid after a RESIDENT load; zero otherwise). Lets host
  // benchmarks and device logs quantify the CSS footprint and the dedup/compression win.
  struct ResidentFootprint {
    uint16_t ruleCount = 0;       // selectors in the index
    uint16_t distinctStyles = 0;  // pool entries after dedup
    uint32_t indexBytes = 0;      // sorted {hash, styleIdx} index
    uint32_t poolBytes = 0;       // distinct-style pool (compressed once compression lands)
    uint32_t totalBytes() const { return indexBytes + poolBytes; }
  };
  [[nodiscard]] ResidentFootprint getResidentFootprint() const;

  // Phase-2 arena mode (docs/compiled-book-pipeline-plan.md). A section built with the
  // secondary framebuffer BORROWED (not freed) runs with ~52 KB less general heap than a
  // released build, so the resolver would otherwise self-degrade below MIN_FREE_HEAP_FOR_CSS
  // and its result gets discarded. These two knobs relocate the resolver's footprint into
  // the borrowed block instead:
  //   - setIndexArena(): the sorted disk index (SelectorEntry[]) is bump-allocated from the
  //     given arena rather than the heap vector. Pass nullptr to return to the heap vector.
  //     The arena owns the memory (reset per build); the parser only holds a view and drops
  //     it on clear()/clearCaches(), reloading lazily from disk on the next resolve.
  //   - setLeanResolve(true): skip the hot-rule LRU entirely (every lookup is index
  //     binary-search + one disk read) and use the lower LEAN_MIN_FREE_HEAP_FOR_CSS floor.
  //     With the index off-heap and no hot cache growth, the resolver's heap footprint is a
  //     few small strings, so the lower floor stays clear of the fault zone.
  // Both are reset by clear() so the shared per-epub instance never carries them into a
  // later heap-backed build or leaves a dangling arena view.
  void setIndexArena(BuildArena* arena) { indexArena_ = arena; }
  void setLeanResolve(bool enable) { leanResolve_ = enable; }

 private:
  // Storage: maps normalized selector -> style properties
  std::unordered_map<std::string, CssStyle> rulesBySelector_;

  std::string cachePath;

  // Disk-backed CSS dictionary index: FNV-1a hash of selector -> byte offset in cache file.
  // Flat sorted array — 8 bytes per entry vs ~60 bytes for unordered_map node.
  // At 1500 rules: ~12 KB instead of ~96 KB. Binary search on hot path.
  // The hash is only a candidate filter: lookups re-read the length-prefixed selector
  // at `offset` and compare it to the query, so a 32-bit collision costs one extra
  // disk read instead of resolving the wrong style.
  struct SelectorEntry {
    uint32_t hash;    // FNV-1a 32-bit of the normalized selector string
    uint32_t offset;  // byte offset of the rule record (selector + payload) in the cache file
  };
  static_assert(sizeof(SelectorEntry) == CSS_INDEX_BYTES_PER_RULE,
                "SelectorEntry size changed — update CSS_INDEX_BYTES_PER_RULE so heap gates stay calibrated");
  mutable bool cacheIndexLoaded_ = false;
  mutable size_t cachedRuleCount_ = 0;
  mutable std::vector<SelectorEntry> cacheRuleOffsets_;  // heap backing (indexArena_ == nullptr)
  mutable uint32_t totalSelectorCandidates_ = 0;
  mutable uint32_t unsupportedSelectorSkips_ = 0;

  // Phase-2 arena mode (see setIndexArena/setLeanResolve). When indexArena_ is set,
  // ensureCacheIndexLoaded() puts the ruleset in arena memory (not the heap vector) using
  // one of two layouts, decided by what fits — mirroring FreeInkBook's resident ruleset
  // (libs/book/FreeInkBook/src/css/Css.cpp) as far as witchhunt's ~108 B CssStyle allows:
  //   - RESIDENT (preferred): a sorted {hash, styleOff} index at arenaResident_ plus a pool of
  //     DISTINCT styles at arenaStylePool_, each SPARSE-COMPRESSED (only the fields the CSS
  //     'defined' mask flags — a real rule sets 1-4 of 24 properties, vs ~108 B for the full
  //     struct). Identical styles are deduplicated (Calibre emits hundreds of differently-named
  //     classes with byte-identical declarations). Compression + dedup together let a large or
  //     repetitive stylesheet that a flat {hash, CssStyle} array couldn't hold resolve in RAM
  //     with zero disk reads; styles decompress on lookup (build-time, ~microseconds).
  //   - INDEX-only (fallback when even the compressed pool won't fit): the sorted 8 B/rule
  //     SelectorEntry index at arenaIndex_; payloads are read from disk on demand.
  // All are non-owning views into arena memory (the arena resets per build); the parser
  // drops them on clear()/clearCaches() and reloads on the next resolve.
  struct ResidentEntry {
    uint32_t hash;      // FNV-1a of the normalized selector (same as SelectorEntry::hash)
    uint32_t styleOff;  // byte offset of this rule's compressed style within arenaStylePool_
  };  // {u32,u32} = 8 B with no padding (vs a padded {u32,u16})
  BuildArena* indexArena_ = nullptr;
  mutable ResidentEntry* arenaResident_ = nullptr;  // sorted by hash, cachedRuleCount_ entries
  mutable uint8_t* arenaStylePool_ = nullptr;       // distinct styles, sparse-compressed, length-prefixed
  mutable uint16_t arenaStyleCount_ = 0;            // distinct style records in the pool
  mutable uint32_t arenaPoolBytes_ = 0;             // bytes used by the compressed pool
  mutable SelectorEntry* arenaIndex_ = nullptr;
  bool leanResolve_ = false;

  // Stream the whole ruleset into the arena as a sorted {hash, styleIdx} index plus a pool of
  // distinct CssStyles (RESIDENT mode; identical styles deduplicated). Returns false if the
  // arena can't hold it (caller then tries INDEX-only).
  bool loadArenaResident(FsFile& file, uint16_t ruleCount, uint32_t totalCandidates, uint32_t unsupportedSkips) const;
  // Forget the current ruleset view (heap vector or either arena layout) so
  // ensureCacheIndexLoaded() reloads it.
  void dropIndex() const;

  static uint32_t selectorHash(std::string_view s);

  // Bounded hot cache of most recently used rules.
  mutable std::list<std::string> hotRuleLru_;
  mutable std::unordered_map<std::string, std::pair<CssStyle, std::list<std::string>::iterator>> hotRuleCache_;
  mutable std::unordered_set<std::string> negativeRuleCache_;
  mutable ResolveStats resolveStats_;

  bool compileModeActive_ = false;
  bool compileModeFailed_ = false;
  std::string compileTempPath_;
  FsFile compileTempFile_;
  std::unordered_map<std::string, uint32_t> compileSelectorOffsets_;

  // Reused scratch for the selector currently being processed (processRuleBlockWithStyle).
  // A member rather than a local so its buffer survives across selectors AND across rule
  // blocks: the parse then costs one growth for the whole stylesheet instead of a fresh heap
  // string per selector. Only valid inside processRuleBlockWithStyle.
  std::string selectorKeyBuf_;

  // Internal parsing helpers
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style, std::string& propNameBuf,
                                        std::string& propValueBuf);

  // Individual property value parsers
  // Take string_view and do NOT normalize — every caller already passes normalized text, and
  // normalized() is idempotent. See the note above the definitions in CssParser.cpp.
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);

  // String utilities
  static std::string normalized(const std::string& s);
  static void normalizedInto(std::string_view s, std::string& out);
  static std::vector<std::string> splitWhitespace(const std::string& s);

  // On-demand rule loading helpers
  bool ensureCacheIndexLoaded() const;
  bool lookupRule(const std::string& selector, CssStyle& outStyle, bool allowDiskLookup = true) const;
  bool readRuleFromDiskAtOffset(uint32_t ruleOffset, const std::string& selector, CssStyle& outStyle) const;
  static bool readCssStylePayload(FsFile& file, CssStyle& style);
  static void writeCssStylePayload(FsFile& file, const CssStyle& style);
  void touchHotRule(const std::string& selector) const;
  void cacheHotRule(const std::string& selector, const CssStyle& style) const;
};
