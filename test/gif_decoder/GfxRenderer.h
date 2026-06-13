#pragma once
// Minimal host-test stub — only the members used by DirectPixelWriter.h and
// GifToFramebufferConverter.cpp need to be declared.  decodeToFramebuffer() is
// never called from host tests; this stub just needs to compile.
#include <cstdint>

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  uint8_t* getWriteTarget() { return nullptr; }
  int getWriteOriginY() { return 0; }
  int getWriteRows() { return 0; }
  RenderMode getRenderMode() { return BW; }
  uint16_t getDisplayWidthBytes() { return 60; }
  int getDisplayWidth() { return 480; }
  int getDisplayHeight() { return 800; }
  int getScreenWidth() { return 480; }
  int getScreenHeight() { return 800; }
  Orientation getOrientation() { return Portrait; }
};
