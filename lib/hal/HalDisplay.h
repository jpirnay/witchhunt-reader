#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  // Initialize the display hardware and driver.
  // When seamless=true, skip the on-wake resync so existing panel content is preserved
  // (used by Quick Resume to bring back the last reader page without a boot screen).
  void begin(bool seamless = false);

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen = false);

  // Non-blocking display split — see EInkDisplay.h for full contract.
  // triggerDisplay() sends pixels + triggers waveform, swaps buffers, returns.
  // completeDisplay() sleeps (via FreeRTOS semaphore) until BUSY deasserts,
  // then does post-waveform SPI work. Both must be called from the render task.
  void triggerDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void completeDisplay();
  // X4 async refresh split: triggerDisplayAsync() returns while the waveform
  // runs (BUSY ISR armed); finishDisplayAsync() sleeps until it completes and
  // clears the refresh bookkeeping. Between the calls the caller may do
  // CPU/RAM-only work — no display or SPI-bus access, same task as trigger.
  // On X3 the trigger falls back to triggerDisplay() and finish is a no-op.
  void triggerDisplayAsync(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Whether triggerDisplayAsync() actually returns while the waveform runs on
  // this panel, rather than falling back to a blocking refresh. False on drivers
  // that cannot overlap (PanelDriver's default, e.g. LgfxEpd and Paper Mono) and
  // while an inversion is pending. Callers that spend the gap on real work --
  // the reader's inline AA -- must ask this rather than infer it from the board,
  // or they pay the plane/gray/restore cost with no overlap to hide it in.
  bool supportsAsyncRefresh() const;
  void finishDisplayAsync();
  bool isRefreshPending() const;
  bool isRedRamSynced() const;
  // Diagnostics: effective refresh mode of the last refresh (after any downgrade).
  RefreshMode getLastRefreshMode() const;
  // Diagnostics: last X4 displayMode byte (0x0C fast / 0x1C OTP-flash / 0xD4 half / 0x34 full).
  uint8_t getLastDisplayModeByte() const;

  // Request extra X3 ghost-clearing on the next display refresh.
  // No-op on non-X3 panels. Consumed by the next displayBuffer/refreshDisplay call.
  void requestResync(uint8_t settlePasses = 0);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Copy the just-displayed frame back into the write framebuffer. The display
  // path ends with a buffer swap, so the write buffer otherwise holds the frame
  // from two refreshes ago — partial repaints that patch a few regions and
  // re-display must call this first. No-op in single-buffer mode.
  void syncWriteBufferFromActive() const;

  // Release both frame buffers back to the heap (~52KB on X3, ~48KB on X4 each,
  // so ~104KB / ~96KB total). Call only after the final displayBuffer(); the
  // e-ink controller retains the image in its own RAM. No display operations
  // may be performed after this. The device must reboot before display resumes.
  void releaseBuffers();

  // Release only the secondary (previous-frame) buffer to free ~52 KB of heap
  // temporarily — e.g. during chapter compilation. BW rendering continues;
  // on X4 fast differential degrades to half/full until restored; on X3
  // fast differential is unaffected. Grayscale AA is unavailable until restored.
  // Returns true if the buffer was freed (false if already released).
  bool releaseSecondaryBuffer();

  // Restore the secondary buffer freed by releaseSecondaryBuffer().
  // Must be called before any grayscale AA pass or (on X4) fast differential.
  // Returns true on success, false on OOM.
  bool reallocSecondaryBuffer();

  // Returns true when the secondary (previous-frame) buffer is allocated.
  bool hasSecondaryBuffer() const;

  // Borrow/return variant of release/realloc: the display drops to the same
  // single-buffer mode, but the block stays owned by the display and is lent to
  // the caller as scratch — it never enters the heap, so nothing can allocate
  // inside it and returnSecondaryBuffer() cannot fail. See FreeInkDisplay.
  uint8_t* borrowSecondaryBuffer(size_t* size);
  bool returnSecondaryBuffer();

  // Allow fast differential refresh to continue (X4) after the secondary buffer
  // is released, diffing against the controller's retained RED-RAM baseline
  // instead of downgrading to half/full. See EInkDisplay::setSingleBufferFastDiff
  // for the contract the caller must uphold.
  void setSingleBufferFastDiff(bool enabled);

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void syncRedRamFromFrameBuffer();
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
  void cleanupGrayscaleWithPreviousBuffer();

  void displayGrayBuffer(bool turnOffScreen = false);
  // True when the panel can show a B/W base and its grayscale planes as ONE
  // waveform. Where it can, the two-push flow (base, then a grey overlay) is
  // not merely slower but wrong: a self-normalizing grey column expects the
  // pixel it drives not to have been driven already.
  bool supportsGrayFrame() const;
  // Compose the intact B/W framebuffer with the LSB/MSB planes staged by
  // copyGrayscale*Buffers() and display the result in one refresh. Falls back to
  // a plain displayBuffer() on a panel that cannot, so callers need no branch of
  // their own beyond deciding whether to stage planes at all.
  void displayGrayscaleFrame(RefreshMode refreshMode, bool turnOffScreen = false);

  // Grey levels this panel resolves in one refresh. 4 on every dual-plane
  // controller (X3, X4, X4 Pro, M5 Paper Mono) — two selector bits per pixel is
  // the whole state space, so it is a hard ceiling, not a setting — and 16 on a
  // panel whose driver keeps a multi-bit buffer of its own (T5S3). Callers that
  // can render either way branch on this; everyone else keeps the plane path.
  uint8_t getGrayLevels() const;
  // Borrow the driver's composition buffer and paint 8-bit grey straight into
  // it: one byte per pixel (0x00 black, 0xFF white), `stride` bytes per row,
  // getDisplayHeight() rows, PHYSICAL panel orientation. Costs no heap — the
  // buffer is the driver's own and already allocated — and returns nullptr
  // wherever that is not on offer, which is every 4-level panel.
  //
  // The loan ends at displayGray8Canvas() or at the next ordinary refresh; a
  // caller that borrows and then abandons the frame corrupts nothing, because
  // every normal push rebuilds the buffer from the 1-bpp framebuffer.
  uint8_t* borrowGray8Canvas(uint16_t* stride);
  // Quantise the borrowed canvas to the panel's native depth and show it as one
  // waveform. The driver picks a bank that can land every level, so this may
  // refresh more thoroughly (and more slowly) than `refreshMode` asked for.
  void displayGray8Canvas(RefreshMode refreshMode, bool turnOffScreen = false);

  // Returns true when the device is an X3 (X4 returns false).
  bool deviceIsX3() const;

  // X3-only knob: pick between the OEM 53-frame grayscale LUT (default, slow
  // and accurate) and the 7-frame community LUT (fast, slightly darker
  // mid-tones). No effect on X4. See EInkDisplay::setFastGrayscaleLut.
  void setFastGrayscaleLut(bool fast);
  bool getFastGrayscaleLut() const;

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  EInkDisplay einkDisplay;
  uint8_t pendingX3SettlePasses = 0;
  RefreshMode lastRefreshMode = RefreshMode::FAST_REFRESH;
  uint8_t lastDisplayModeByte = 0x0C;  // default to fast refresh mode byte
};

extern HalDisplay display;
