#pragma once
// Minimal host-test stub for HalDisplay.h. JpegToBmpConverter.cpp includes it but
// only the no-size jpegFileToBmpStream() entry point reads display dimensions; the
// WithSize variants exercised by these tests pass explicit targets.
#include <cstdint>

class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 480;
  static constexpr uint16_t DISPLAY_HEIGHT = 800;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = 60;

  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
};

// Singleton used as `display.getDisplay...()` in the converter.
inline HalDisplay display;

// The converter gates decoding on ESP.getFreeHeap(); on device this global comes
// from Arduino. Provide a generous host stub so the gate always passes.
struct EspClass {
  uint32_t getFreeHeap() const { return 256u * 1024u; }
};
inline EspClass ESP;
