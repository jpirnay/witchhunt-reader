#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

class FontCacheManager;
class SdCardFont;

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Bitmap.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory
  static constexpr unsigned int REFRESH_OVERRIDE_NONE = 0;

  HalDisplay& display;
  std::atomic<int> renderMode;
  std::atomic<int> orientation;
  std::atomic<bool> fadingFix;
  // Text darkness for 2-bit grayscale glyph rendering.
  std::atomic<uint8_t> textDarkness;
  //   0 = Normal     — true 4-level AA (raw=1 → light gray, raw=2 → dark gray)
  //   1 = Dark       — historical default; raw=2 collapses to black
  //   2 = Extra Dark — both AA shades go black in the grayscale plane
  //   3 = Maximum    — grayscale pass skipped entirely; only the hard BW
  //                    pass remains, so AA pixels render as solid black
  //                    with the FAST waveform (no gray-LUT softening)
  // Only affects AA pixels in GRAYSCALE_MSB / GRAYSCALE_LSB rendering of 2-bit fonts.
  // 1-bit fonts and the BW pass are unchanged. Default is 1 to preserve historical
  // rendering. See drawMaskFor2BitMode() in GfxRenderer.cpp for the per-level
  // pixel breakdown and a worked example glyph.
  mutable uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = 0;       // set in begin()
  uint16_t panelHeight = 0;      // set in begin()
  uint16_t panelWidthBytes = 0;  // set in begin()
  uint32_t frameBufferSize = 0;  // set in begin()
  uint16_t bwSnapshotRowStart = 0;
  uint16_t bwSnapshotRowEnd = 0;
  size_t bwSnapshotSizeBytes = 0;
  size_t bwBufferChunkSize = BW_BUFFER_CHUNK_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureFontReady() is const (called from layout code that
  // holds a const GfxRenderer&) but triggers SD card reads and heap allocation
  // inside the SdCardFont objects. Same pragmatic compromise as fontCacheManager_.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // --- Scaled-glyph mask cache ----------------------------------------------
  // Text drawn at a scale != 1.0 — i.e. every word of any book whose stylesheet
  // puts a font-size on body text, which is most of them — is resampled per
  // destination pixel by renderCharAtScale(). Uncached, that resample runs once
  // per glyph *occurrence*: ~500 times a page against ~40 distinct glyphs.
  // Measured 2026-08-02 (X4, "A Scandalous Deception", body text at 0.92em):
  // 303 ms for the page's BW pass with the scaling, 39 ms with it off.
  //
  // Caching the resampled 1-bit coverage mask makes the resample once per
  // distinct (glyph, scale, selector) and lets renderGlyphFastBW() — the same
  // 8-pixel-chunk blitter unscaled text uses — draw every occurrence.
  //
  // Budget: 80 x 16B table + 3.5 KB arena ≈ 4.75 KB, two allocations made lazily
  // on the first scaled glyph and released by releaseScaledGlyphCache(). It is
  // a bump allocator that resets wholesale when full: no per-glyph allocation,
  // nothing to fragment, and a page whose working set overflows the arena
  // degrades toward the uncached path instead of growing.
  //
  // Not internally synchronised: it is written only from glyph rasterisation,
  // which every caller (render task, loop-task pre-render, deferred AA) already
  // serialises behind RenderLock — the same lock that protects the framebuffer
  // these masks are blitted into.
  //
  // Keyed by (font, codepoint, scale, sel), NOT by EpdGlyph address: SD-card
  // fonts rebuild their glyph array every page and serve overflow codepoints
  // from an 8-entry ring, so the same address means different glyphs over time.
  // The resampled pixels are a pure function of this key, which makes it both
  // stable and exact. Style is deliberately absent: EpdFontFamily::getData() and
  // getGlyph() both resolve through getFont(style), so the EpdFontData pointer
  // already identifies the variant.
  struct ScaledGlyphEntry {
    const void* fontData;  // EpdFontData for the style (stable per font object)
    uint32_t scaleBits;    // exact float bits of the scale factor (no quantisation)
    uint16_t cp;           // codepoint; above the BMP renders uncached (see alloc)
    uint16_t offset;       // byte offset into scaledGlyphArena_
    uint8_t sel;           // minRaw2Bit (scale < 1) or drawMask (scale >= 1)
    uint8_t w;
    uint8_t h;
  };
  // Keeps the documented budget honest if a field is ever added.
  static_assert(sizeof(ScaledGlyphEntry) <= 16, "scaled-glyph entry grew; update the cache budget comment");
  // Sized from measurement, not guesswork: a page of this corpus needs 45-58
  // distinct renditions (three font groups x ~40 glyphs, per the prewarm logs)
  // averaging ~40 bytes of mask each. 48 entries reset on every pass — the table,
  // not the arena, was the binding constraint.
  static constexpr uint8_t SCALED_GLYPH_MAX_ENTRIES = 80;
  static constexpr uint16_t SCALED_GLYPH_ARENA_BYTES = 3584;
  // One outsized glyph (a large heading) must not evict a page's whole body-text
  // working set, so anything above this renders uncached.
  static constexpr uint16_t SCALED_GLYPH_MAX_MASK_BYTES = 512;
  mutable std::unique_ptr<ScaledGlyphEntry[]> scaledGlyphEntries_;
  mutable std::unique_ptr<uint8_t[]> scaledGlyphArena_;
  mutable uint8_t scaledGlyphCount_ = 0;
  mutable uint16_t scaledGlyphUsed_ = 0;
  mutable bool scaledGlyphOom_ = false;  // arena allocation failed; do not retry every glyph

  mutable std::atomic<unsigned int> refreshOverride = REFRESH_OVERRIDE_NONE;
  // Atomically consume a pending setNextDisplayRefreshMode() override: if one is set, clear it
  // and return its mode; otherwise return `requested`. Shared by displayBuffer() and
  // triggerDisplay() so the override is honored on BOTH the blocking and non-blocking display
  // paths (otherwise a HALF set before a triggerDisplay() render would persist and leak onto a
  // later displayBuffer() turn).
  HalDisplay::RefreshMode consumeRefreshOverride(HalDisplay::RefreshMode requested) const;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Write a patterned horizontal span directly to the physical framebuffer using byte-level operations.
  // phyY: physical row; phyX_start/phyX_end: inclusive physical column range.
  // patternByte is repeated across the span; partial edge bytes are blended with existing content.
  // Bit layout: MSB-first (bit 7 = phyX=0); 0 bits = dark pixel, 1 bits = white pixel.
  void fillPhysicalHSpanByte(int phyY, int phyX_start, int phyX_end, uint8_t patternByte) const;
  // Write a solid horizontal span directly to the physical framebuffer using byte-level operations.
  // Thin wrapper around fillPhysicalHSpanByte: state=true → 0x00 (dark), false → 0xFF (white).
  void fillPhysicalHSpan(int phyY, int phyX_start, int phyX_end, bool state) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay),
        renderMode(static_cast<int>(BW)),
        orientation(static_cast<int>(Portrait)),
        fadingFix(false),
        textDarkness(1) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    invalidateScaledGlyphCache();
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  // Each of these can retire an EpdFontData a cached scaled mask is keyed on, so
  // they all drop the cache (see ScaledGlyphEntry).
  void registerSdCardFont(int fontId, SdCardFont* font) {
    sdCardFonts_[fontId] = font;
    invalidateScaledGlyphCache();
  }
  void unregisterSdCardFont(int fontId) {
    sdCardFonts_.erase(fontId);
    invalidateScaledGlyphCache();
  }
  void clearSdCardFonts() {
    sdCardFonts_.clear();
    invalidateScaledGlyphCache();
  }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }

  // Ensure glyph metrics are loaded for the given text before layout measurement.
  // No-op for built-in fonts (map lookup finds nothing and returns immediately).
  // For SD/flash fonts: reads glyph metrics (no bitmaps) for all codepoints in text.
  void ensureFontReady(int fontId, const char* utf8Text) const;

  // Clear the cumulative font metadata cache built up across paragraphs.
  // No-op when no SD font is active.
  void clearFontAccumulation() const;

  // Phase lifecycle: drop layout-phase metadata to free heap before createSectionFile().
  // No-op when no SD font is active or font is mmap'd (metadata is always accessible).
  void dropFontMetadata() const;

  // Restore layout-phase metadata after createSectionFile().
  // Returns true if all fonts reloaded successfully (always true for mmap fonts).
  bool restoreFontMetadata() const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation.store(static_cast<int>(o), std::memory_order_relaxed); }
  Orientation getOrientation() const { return static_cast<Orientation>(orientation.load(std::memory_order_relaxed)); }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix.store(enabled, std::memory_order_relaxed); }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void setNextDisplayRefreshMode(HalDisplay::RefreshMode refreshMode) const;
  // True if a setNextDisplayRefreshMode() override is armed but not yet consumed. Peek only —
  // does NOT consume it (unlike consumeRefreshOverride). Lets a caller that is about to issue an
  // intermediate refresh (which would consume the override) detect that a deliberate-transition
  // override is pending and react accordingly.
  bool hasRefreshOverridePending() const {
    return refreshOverride.load(std::memory_order_acquire) != REFRESH_OVERRIDE_NONE;
  }
  // Discard a pending setNextDisplayRefreshMode() override without applying it, so the next display
  // uses its own requested mode. Use when the armed override should move to a later refresh — e.g.
  // the reader keeps the indexing popup FAST but forces the following content page to HALF itself.
  void clearRefreshOverride() const { consumeRefreshOverride(HalDisplay::FAST_REFRESH); }
  // Make the write framebuffer match the currently displayed frame. Call before
  // a partial repaint that patches a few regions and re-displays without
  // re-rendering the full frame: displayBuffer() ends with swapBuffers(), so
  // the write buffer otherwise holds the frame from two refreshes ago. No-op in
  // single-buffer mode, where the write buffer is already the displayed frame.
  void syncWriteBufferFromDisplayed() const { display.syncWriteBufferFromActive(); }

  // Temporarily free the secondary (previous-frame) buffer (~52 KB) during
  // operations that don't need it (e.g. chapter compilation). BW rendering
  // continues normally. Grayscale AA and (on X4) fast differential are
  // unavailable until reallocSecondaryBuffer() is called.
  bool releaseSecondaryBuffer() const { return display.releaseSecondaryBuffer(); }
  bool reallocSecondaryBuffer() const { return display.reallocSecondaryBuffer(); }
  bool hasSecondaryBuffer() const { return display.hasSecondaryBuffer(); }
  // Borrow/return variant of release/realloc: the block never enters the heap,
  // so nothing can allocate inside it and the return cannot fail. See
  // FreeInkDisplay::borrowSecondaryBuffer.
  uint8_t* borrowSecondaryBuffer(size_t* size) const { return display.borrowSecondaryBuffer(size); }
  bool returnSecondaryBuffer() const { return display.returnSecondaryBuffer(); }
  // Keep fast differential alive (X4) after releaseSecondaryBuffer() by diffing
  // against the controller's retained baseline. See HalDisplay::setSingleBufferFastDiff.
  void setSingleBufferFastDiff(bool enabled) const { display.setSingleBufferFastDiff(enabled); }
  bool isX3() const { return display.deviceIsX3(); }

  // Non-blocking display split.
  // triggerDisplay() sends pixels, issues the refresh command and returns
  // immediately — the waveform runs in hardware. frameBuffer is safe to
  // overwrite after this returns. completeDisplay() genuinely sleeps (via
  // FreeRTOS semaphore) until BUSY deasserts, then does post-waveform work.
  // Both must be called from the render task; no other task may call SPI
  // display methods between triggerDisplay() and completeDisplay().
  // Honors a pending setNextDisplayRefreshMode() override (see consumeRefreshOverride);
  // defined out-of-line in the .cpp so it can share that logic with displayBuffer().
  void triggerDisplay(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH, bool turnOffScreen = false) const;
  // X4 async refresh split: triggerDisplayAsync() returns while the waveform
  // runs; finishDisplayAsync() sleeps until it completes. CPU/RAM-only work is
  // allowed between the two calls (no display/SPI), same task as the trigger.
  // Honors the same refresh override + fading-fix policy as triggerDisplay().
  // On X3 the trigger falls back to the synchronous path and finish is a no-op.
  void triggerDisplayAsync(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH, bool turnOffScreen = false) const;
  void finishDisplayAsync() const { display.finishDisplayAsync(); }
  void completeDisplay() const {
    display.completeDisplay();
    // No per-page RED reseed here — same rationale as displayBuffer(): the
    // display driver keeps the RED (previous-frame) plane current on every
    // refresh (writes RED from prev on dual-buffer fast, resyncs it after
    // single-buffer refreshes), so a syncRedRamFromFrameBuffer() here is a
    // redundant ~48 KB SPI write per page turn. The explicit seed lives at the
    // dual->single transition (release sites) only.
  }
  bool isRefreshPending() const { return display.isRefreshPending(); }
  bool isRedRamSynced() const { return display.isRedRamSynced(); }
  // Diagnostics: effective refresh mode of the last refresh (after any downgrade).
  HalDisplay::RefreshMode getLastRefreshMode() const { return display.getLastRefreshMode(); }
  // Diagnostics: last X4 displayMode byte (0x0C fast / 0x1C OTP-flash / 0xD4 half / 0x34 full).
  uint8_t getLastDisplayModeByte() const { return display.getLastDisplayModeByte(); }
  void displayWindow(int x, int y, int width, int height, bool turnOffScreen = false) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  // Copy one packed 1bpp row in the device's physical portrait coordinate
  // space into the controller framebuffer. Source and framebuffer are MSB-first
  // with 0 = black, 1 = white; set invertBits when the source row uses 1 = ink.
  void writePhysicalPortraitPackedRow(int physicalY, const uint8_t* packedRow, int pixelWidth,
                                      bool invertBits = false) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIconInverted(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextWidthScaled(int fontId, const char* text, EpdFontFamily::Style style, float scale) const;
  // Ink extents of `text` relative to its baseline, from glyph bitmap metrics:
  // aboveBaseline = tallest glyph ink top, belowBaseline = deepest ink below the
  // baseline (0 for caps/digits). Excludes the font's internal leading, unlike
  // getFontAscenderSize. Returns false when no glyph is found. SD fonts need
  // ensureFontReady(fontId, text) first so glyph metrics are loaded.
  bool getTextInkMetrics(int fontId, const char* text, EpdFontFamily::Style style, int* aboveBaseline,
                         int* belowBaseline) const;
  int getLineHeightScaled(int fontId, float scale) const;
  int getFontAscenderSizeScaled(int fontId, float scale) const;
  void drawTextScaled(int fontId, int x, int y, const char* text, bool black, EpdFontFamily::Style style,
                      float scale) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) {
    this->renderMode.store(static_cast<int>(mode), std::memory_order_relaxed);
  }
  RenderMode getRenderMode() const { return static_cast<RenderMode>(renderMode.load(std::memory_order_relaxed)); }

  // Text darkness control:
  //   0 = Normal, 1 = Dark, 2 = Extra Dark, 3 = Maximum.
  // Only affects anti-aliased pixels in 2-bit (grayscale) glyph rendering;
  // 1-bit fonts and the BW pass are unchanged. See drawMaskFor2BitMode() in
  // GfxRenderer.cpp for the per-level pixel breakdown and a worked example.
  void setTextDarkness(const uint8_t d) { textDarkness.store(d, std::memory_order_relaxed); }
  uint8_t getTextDarkness() const { return static_cast<uint8_t>(textDarkness.load(std::memory_order_relaxed)); }
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;

  // Timing breakdown returned by renderGrayscalePlanesSequential().
  struct GrayscaleTimings {
    unsigned long planesMs = 0;   // LSB render+copy + MSB render+copy
    unsigned long displayMs = 0;  // displayGrayBuffer() waveform
    unsigned long restoreMs = 0;  // cleanupGrayscaleWithPreviousBuffer() SPI write
    bool aborted = false;         // dropped at the mid-pass abort point; no gray flush happened
  };

  // Both plane passes below take a shouldAbort predicate, evaluated once at the last point
  // where the pass can still be dropped without the user seeing anything: after the LSB plane
  // has been rendered into the framebuffer but before the first grayscale byte reaches the
  // controller. Aborting there costs only the framebuffer reseed that the normal tail performs
  // anyway, and gives back the MSB render, both plane writes and the gray flush — which is the
  // whole net cost of anti-aliasing. Pass a predicate that answers "is this page already on its
  // way out"; return false to always complete.

  // Abandon a grayscale pass without flushing it, and report it as aborted. Reseeds the
  // framebuffer and controller baseline from the previous-frame slot, exactly as a completed
  // pass's tail does. Valid at either gate: plane data may already sit in controller RAM, but
  // only displayGrayBuffer() runs the gray waveform, so nothing has reached the panel and the
  // BW page stays displayed.
  GrayscaleTimings abandonGrayscalePass(const unsigned long planesMs) {
    GrayscaleTimings t;
    t.planesMs = planesMs;
    const unsigned long tAbort = millis();
    setRenderMode(BW);
    cleanupGrayscaleWithPreviousBuffer();
    t.restoreMs = millis() - tAbort;
    t.aborted = true;
    return t;
  }

  // Render both grayscale planes sequentially into the BW framebuffer, streaming
  // each plane to the controller immediately after rendering it. No extra allocation
  // needed — the BW framebuffer is the scratch pad for both passes.
  //
  // After displayGrayBuffer(), cleanupGrayscaleWithPreviousBuffer() reseeds the
  // controller's RED RAM and the in-RAM active buffer from frameBufferActive —
  // which holds the exact full BW page (including images) that displayBuffer()
  // left there before the grayscale pass began. This is the correct differential
  // baseline for the next fast refresh.
  //
  // renderFn is called twice (LSB, MSB). The RenderMode argument tells it which
  // pass is running. The caller sets setFastGrayscaleLut() before calling.
  //
  // Returns wall-clock timings for each of the three phases.
  //
  // Signature: void renderFn(RenderMode mode)
  template <typename RenderFn, typename AbortFn>
  GrayscaleTimings renderGrayscalePlanesSequential(RenderFn renderFn, AbortFn shouldAbort) {
    GrayscaleTimings t;
    const unsigned long t0 = millis();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_LSB);
    renderFn(GRAYSCALE_LSB);
    if (shouldAbort()) {
      return abandonGrayscalePass(millis() - t0);
    }
    copyGrayscaleLsbBuffers();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_MSB);
    renderFn(GRAYSCALE_MSB);
    // The MSB render is as long as the LSB one and can bail just as late, so it needs its own
    // gate: without it a plane abandoned part-way was still copied and flushed, putting a
    // half-drawn overlay on screen (seen on X3 as a COMPLETED pass with planes=545ms against a
    // normal ~890ms). Checked before the copy so no partial plane even reaches the controller.
    if (shouldAbort()) {
      return abandonGrayscalePass(millis() - t0);
    }
    copyGrayscaleMsbBuffers();

    const unsigned long t1 = millis();
    t.planesMs = t1 - t0;

    setRenderMode(BW);
    display.displayGrayBuffer(/*turnOffScreen=*/false);

    const unsigned long t2 = millis();
    t.displayMs = t2 - t1;

    // Reseed RED RAM and frameBufferActive from the previous-frame slot, which
    // holds the full BW page exactly as displayBuffer() left it. Using this
    // instead of re-rendering gives the correct baseline (images + text) and
    // costs only one SPI write.
    cleanupGrayscaleWithPreviousBuffer();

    t.restoreMs = millis() - t2;
    return t;
  }

  // Same plane dance as renderGrayscalePlanesSequential(), but entered while an
  // async BW refresh is still in flight (triggerDisplayAsync()): the LSB plane
  // renders into the write framebuffer DURING the waveform — CPU/RAM work only,
  // the controller scans its own RAM — then finishDisplayAsync() consumes the
  // remaining wait before the first SPI plane write. This lands the gray flush
  // ~one plane-render earlier than deferring the whole pass to after the
  // waveform, which is what makes the AA touch-up read as part of the page
  // refresh instead of a separate later update.
  //
  // Caller contract: an async refresh MUST be in flight, and the glyphs the
  // renderFn draws must already be prewarmed (a cache miss would issue SD reads
  // — allowed — but a display/SPI call in renderFn is not).
  template <typename RenderFn, typename AbortFn>
  GrayscaleTimings renderGrayscalePlanesInterleaved(RenderFn renderFn, AbortFn shouldAbort) {
    GrayscaleTimings t;
    const unsigned long t0 = millis();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_LSB);
    renderFn(GRAYSCALE_LSB);
    const unsigned long tLsbDone = millis();

    // Waveform still running: sleep out the remainder (power hooks active).
    display.finishDisplayAsync();
    const unsigned long tWaveDone = millis();

    // Abort point. This is the ideal place for it on the interleaved path: the BW page is now
    // fully on screen, and the LSB render just spent was paid for out of the waveform wait
    // rather than out of the page-turn budget — so dropping the pass here reclaims the entire
    // net cost of the AA and forfeits nothing that was actually charged to the user.
    if (shouldAbort()) {
      return abandonGrayscalePass(tLsbDone - t0);
    }

    copyGrayscaleLsbBuffers();
    clearScreen(0x00);
    setRenderMode(GRAYSCALE_MSB);
    renderFn(GRAYSCALE_MSB);
    // Second gate, same reason as in the sequential pass: an MSB render that bailed part-way
    // must never be copied or flushed. Checked before the copy.
    if (shouldAbort()) {
      return abandonGrayscalePass((tLsbDone - t0) + (millis() - tWaveDone));
    }
    copyGrayscaleMsbBuffers();

    const unsigned long t1 = millis();
    // Report only actual plane work; the residual waveform sleep is not ours.
    t.planesMs = (tLsbDone - t0) + (t1 - tWaveDone);

    setRenderMode(BW);
    display.displayGrayBuffer(/*turnOffScreen=*/false);

    const unsigned long t2 = millis();
    t.displayMs = t2 - t1;

    cleanupGrayscaleWithPreviousBuffer();

    t.restoreMs = millis() - t2;
    return t;
  }

  // X3-only: trade AA visual fidelity for ~2.2 s faster page-flip wall clock.
  // No effect on X4 (its single grayscale LUT already runs at ~500 ms).
  void setFastGrayscaleLut(bool fast) const { display.setFastGrayscaleLut(fast); }
  bool getFastGrayscaleLut() const { return display.getFastGrayscaleLut(); }

  // Active pixel-write target for raw writers that bypass drawPixel for speed.
  // Returns the full framebuffer and its extent ([0, panelHeight)).
  uint8_t* getWriteTarget() const { return frameBuffer; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return static_cast<int>(panelHeight); }
  bool isStripActive() const { return false; }
  bool glyphIntersectsStrip(int, int, int, int) const { return true; }

  bool storeBwBuffer();                                         // Returns true if buffer was stored successfully
  bool storeBwBufferRect(int x, int y, int width, int height);  // Store only rows intersecting logical rect
  void restoreBwBuffer();                                       // Restore and free the stored buffer
  // Re-syncs the controller's RED RAM from the current BW framebuffer so the
  // next differential page turn has a clean baseline. Called after the tiled
  // grayscale path, which leaves the panel's gray planes loaded but the BW
  // framebuffer untouched.
  //
  // const-correctness caveat: on X3 the underlying display call (see
  // EInkDisplay::cleanupGrayscaleBuffers) performs an in-place Y-flip of the
  // framebuffer bytes, sends them, and flips back. The framebuffer's logical
  // contents are identical before and after, but during the call the bytes
  // are transiently reordered. The method stays `const` because the renderer's
  // observable state doesn't change; callers must not race a framebuffer
  // reader against this call.
  void syncRedRamFromFrameBuffer() const;
  void cleanupGrayscaleWithFrameBuffer() const;
  // Reseed controller RED RAM and frameBufferActive from the display's internal
  // previous-frame buffer (frameBufferActive in EInkDisplay). This holds the
  // exact full BW page that displayBuffer() committed before the grayscale pass
  // — including images — giving a correct differential baseline for the next
  // fast refresh without any re-render.
  void cleanupGrayscaleWithPreviousBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Scaled-glyph mask cache (see ScaledGlyphEntry). Public only because the glyph
  // pipeline lives in free functions in GfxRenderer.cpp; treat as internal.
  // find returns the cached mask for an exact key match, alloc reserves a zeroed
  // mask of w*h bits for the caller to fill, or nullptr when the glyph is too
  // large to cache or the arena could not be allocated.
  const uint8_t* findScaledGlyphMask(const void* fontData, uint32_t cp, float scale, uint8_t sel, int w, int h) const;
  uint8_t* allocScaledGlyphMask(const void* fontData, uint32_t cp, float scale, uint8_t sel, int w, int h) const;

  // Drop every cached mask, keeping the arena. MUST run whenever a font is added
  // or removed: entries are keyed by EpdFontData address, which a later font
  // object could reuse.
  void invalidateScaledGlyphCache() const {
    scaledGlyphCount_ = 0;
    scaledGlyphUsed_ = 0;
  }
  // Give the ~4 KB arena back. The cache re-allocates lazily if rendering resumes.
  void releaseScaledGlyphCache() const {
    scaledGlyphEntries_.reset();
    scaledGlyphArena_.reset();
    scaledGlyphOom_ = false;
    invalidateScaledGlyphCache();
  }

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;

  // Release both display frame buffers back to the heap (~96-104KB total).
  // Nulls the local frameBuffer pointer too; displayBuffer() rejects flushes
  // (LOG_ERR + no-op) while it is null instead of streaming freed memory.
  // Only valid after the final displayBuffer(); the device must reboot before
  // any display operation is attempted again.
  void releaseFrameBuffers() {
    display.releaseBuffers();
    frameBuffer = nullptr;
    releaseScaledGlyphCache();
  }

  // Release both display buffers and install a caller-owned scratch buffer as
  // the active framebuffer. Pixel writes during the warm pass land in scratch
  // (discarded on reboot) while the decoder can use the freed ~96 KB for its
  // own allocation. scratchSize must be >= panelWidthBytes * panelHeight.
  // The device must reboot before any display operation is attempted again.
  bool releaseFrameBuffersWithScratch(uint8_t* scratch, size_t scratchSize) {
    if (!scratch || scratchSize < static_cast<size_t>(panelWidthBytes) * panelHeight) return false;
    display.releaseBuffers();
    releaseScaledGlyphCache();
    memset(scratch, 0, scratchSize);
    frameBuffer = scratch;
    return true;
  }
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache helpers: operate on a logical (orientation-aware) rect and
  // copy only the framebuffer bytes it touches. Used by HomeActivity to snapshot
  // the cover tile (~16 KB) instead of the full 48 KB framebuffer.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
