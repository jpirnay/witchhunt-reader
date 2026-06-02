#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

class FontCacheManager;
class SdCardFont;

#include <atomic>
#include <cstring>
#include <map>
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
  mutable std::atomic<unsigned int> refreshOverride = REFRESH_OVERRIDE_NONE;

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
  void removeFont(int fontId) { fontMap.erase(fontId); }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { sdCardFonts_.erase(fontId); }
  void clearSdCardFonts() { sdCardFonts_.clear(); }
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

  // Temporarily free the secondary (previous-frame) buffer (~52 KB) during
  // operations that don't need it (e.g. chapter compilation). BW rendering
  // continues normally. Grayscale AA and (on X4) fast differential are
  // unavailable until reallocSecondaryBuffer() is called.
  bool releaseSecondaryBuffer() const { return display.releaseSecondaryBuffer(); }
  bool reallocSecondaryBuffer() const { return display.reallocSecondaryBuffer(); }

  // Non-blocking display split.
  // triggerDisplay() sends pixels, issues the refresh command and returns
  // immediately — the waveform runs in hardware. frameBuffer is safe to
  // overwrite after this returns. completeDisplay() genuinely sleeps (via
  // FreeRTOS semaphore) until BUSY deasserts, then does post-waveform work.
  // Both must be called from the render task; no other task may call SPI
  // display methods between triggerDisplay() and completeDisplay().
  void triggerDisplay(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH, bool turnOffScreen = false) const {
    display.triggerDisplay(mode, turnOffScreen);
  }
  void completeDisplay() const { display.completeDisplay(); }
  bool isRefreshPending() const { return display.isRefreshPending(); }
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
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
  };

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
  template <typename RenderFn>
  GrayscaleTimings renderGrayscalePlanesSequential(RenderFn renderFn) {
    GrayscaleTimings t;
    const unsigned long t0 = millis();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_LSB);
    renderFn(GRAYSCALE_LSB);
    copyGrayscaleLsbBuffers();

    clearScreen(0x00);
    setRenderMode(GRAYSCALE_MSB);
    renderFn(GRAYSCALE_MSB);
    copyGrayscaleMsbBuffers();

    const unsigned long t1 = millis();
    t.planesMs = t1 - t0;

    setRenderMode(BW);
    displayGrayBuffer();

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

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;

  // Release both display frame buffers back to the heap (~96-104KB total).
  // Nulls the local frameBuffer pointer too so any accidental render attempt
  // fails visibly rather than corrupting freed memory.
  // Only valid after the final displayBuffer(); the device must reboot before
  // any display operation is attempted again.
  void releaseFrameBuffers() {
    display.releaseBuffers();
    frameBuffer = nullptr;
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
