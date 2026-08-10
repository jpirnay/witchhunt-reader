#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  // Slots are keyed by EpdFontData POINTER, and every SIZE is a distinct EpdFontData -- so these
  // are shared across style x size, not "one per style" as long assumed. A page using R/B/I at
  // two sizes wants six. Exhaustion evicts the least-recently-used slot rather than refusing the
  // prewarm outright; a refused font used to send every one of its glyphs down the per-glyph
  // fallback for the whole page.
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first and otherwise transiently
  // allocates/decompresses the glyph's group into a temporary buffer and
  // compacts the requested glyph. The returned pointer is valid only until the
  // next getBitmap call or cache eviction; callers must copy bitmap data if a
  // longer lifetime is required.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffers).
  void clearCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm or getBitmap miss
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls

    // LRU specific stats
    uint32_t fallbackCacheHits = 0;
    uint32_t fallbackCacheMisses = 0;

    // Fallback-path OOM latch. Smallest group size (bytes) whose transient allocation
    // failed during this pass, or 0 if none has. Lives in Stats so it is cleared by
    // resetStats() — i.e. it lasts exactly one render phase, and the next phase retries
    // with whatever heap is available by then. See getBitmap().
    uint32_t fallbackOomBytes = 0;
    uint16_t fallbackOomGlyphs = 0;  // glyphs that got no bitmap because of it
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
    uint16_t groupIndex;     // cached to avoid re-calling getGroupIndex in prewarm Step 4
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
    uint32_t bufferBytes = 0;   // size of `buffer`, so eviction can unwind the stats it added
    uint32_t lastUsedTick = 0;  // bumped on prewarm and on every getBitmap hit; drives eviction
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;
  uint32_t pageSlotTick_ = 0;

  // Fixed scratch for group decompression, shared by prewarmCache() and the getBitmap()
  // fallback. At most ONE group is live at a time in either path (each decompresses, extracts,
  // then releases before touching the next), and the two never overlap -- prewarm runs before
  // the render pass that calls getBitmap -- so a single buffer serves both.
  //
  // This replaces a malloc/free per group per prewarm call per page. Measured X3: one page ran
  // 3 prewarm calls over 5 groups with a peak group of 10555 B, so ~5 transient allocations of
  // wildly varying size per page, each landing in the largest free block and splitting it at a
  // different offset. That is what ratcheted contig down 42996 -> 38900 -> 34804 without ever
  // recovering, while allocated/free block counts merely oscillated (i.e. fragmentation, not a
  // leak). A fixed size is the point: the block is carved once, at a stable address.
  //
  // NOT sized to the format cap. GROUP_MAX_UNCOMPRESSED_BYTES is 65536 in fontconvert.py, whose
  // comment calls 64 KB "a comfortable transient malloc on the ESP32-C3" -- untrue once contig
  // sits near 34 KB. Groups above this bound still take the malloc path.
  static constexpr uint32_t GROUP_SCRATCH_BYTES = 16 * 1024;
  uint8_t* groupScratch_ = nullptr;
  // Borrow the shared scratch when the group fits, else malloc. *outOwned tells the caller
  // whether releaseGroupBuffer() must free it.
  uint8_t* acquireGroupBuffer(uint32_t bytes, bool* outOwned);
  static void releaseGroupBuffer(uint8_t* buf, bool owned);

  static constexpr uint16_t HOT_GLYPH_BUF_SIZE = 512;  // largest packed single glyph
  // A 1-slot LRU is pathological: consecutive distinct glyphs evict each other every time, so
  // the fallback degenerates to a full group inflate per glyph. Measured X3 on a page whose
  // prewarm did not cover its heading size: 16 getBitmap calls, 121007 us total (7562 us/call),
  // fallback hits=1 misses=12. These slots are fixed-size and live inside the object, so more
  // of them cost static bytes and cannot fragment the heap.
  static constexpr uint8_t FALLBACK_CACHE_SLOTS = 8;

  struct FallbackSlot {
    const EpdFontData* fontData;
    uint32_t glyphIndex;
    uint32_t lastUsedTick;
    uint8_t buffer[HOT_GLYPH_BUF_SIZE];
  };

  // LRU cache for fallback glyph decompresion
  FallbackSlot _fallbackCache[FALLBACK_CACHE_SLOTS] = {};
  uint32_t _fallbackTick = 0;

  void freePageBuffer();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
