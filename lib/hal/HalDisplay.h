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
  bool isRefreshPending() const;
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

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void syncRedRamFromFrameBuffer();
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
  void cleanupGrayscaleWithPreviousBuffer();

  void displayGrayBuffer(bool turnOffScreen = false);

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
