#pragma once
// Minimal host-test stub for HalDisplay.h (included by DirectPixelWriter.h).
#include <cstdint>

class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 480;
  static constexpr uint16_t DISPLAY_HEIGHT = 800;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = 60;
};
