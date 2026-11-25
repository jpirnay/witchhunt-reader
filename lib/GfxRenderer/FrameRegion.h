#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

// The bytes of the frame buffer a logical rectangle covers, their copy out and back, a logical row read back, and a
// digest of a whole frame.
// GfxRenderer's region helpers (getRegionByteSize, copyRegionToBuffer, copyBufferToRegion) are thin wrappers over this
// header, kept free of Arduino so the host can test it (test/frame_region).
namespace frameregion {

// Mirrors GfxRenderer::Orientation's declared order, as touchtransform does; GfxRenderer.cpp static_asserts it.
enum Orientation : int {
  Portrait = 0,
  LandscapeClockwise = 1,
  PortraitInverted = 2,
  LandscapeCounterClockwise = 3,
};

// Logical (x, y) -> panel (x, y): the mapping of GfxRenderer.cpp's rotateCoordinates(), which the per-pixel draws keep
// inline there; the inverse of touchtransform::tapToLogical().
inline void toPanel(const int orientation, const int x, const int y, const int panelWidth, const int panelHeight,
                    int* panelX, int* panelY) {
  switch (orientation) {
    case Portrait:
      *panelX = y;
      *panelY = panelHeight - 1 - x;
      break;
    case LandscapeClockwise:
      *panelX = panelWidth - 1 - x;
      *panelY = panelHeight - 1 - y;
      break;
    case PortraitInverted:
      *panelX = panelWidth - 1 - y;
      *panelY = x;
      break;
    case LandscapeCounterClockwise:
    default:
      *panelX = x;
      *panelY = y;
      break;
  }
}

// Logical row y of the frame (rows of `stride` bytes) in `orientation`, pixels [0, width), packed one bit a pixel, MSB
// first, as the frame keeps them (a set bit is white): one row of a picture of the screen as it is held, which is what
// a screenshot writes (ScreenshotUtil, crosspoint-reader PR #4457 by Uri Tauber). `out` takes (width + 7) / 8 bytes;
// bits past `width` in the last one stay clear.
inline void copyLogicalRow(const uint8_t* frame, const int stride, const int orientation, const int panelWidth,
                           const int panelHeight, const int y, const int width, uint8_t* out) {
  memset(out, 0, static_cast<size_t>(width + 7) / 8);
  for (int x = 0; x < width; x++) {
    int panelX = 0;
    int panelY = 0;
    toPanel(orientation, x, y, panelWidth, panelHeight, &panelX, &panelY);
    const uint8_t pixel = (frame[static_cast<size_t>(panelY) * stride + panelX / 8] >> (7 - panelX % 8)) & 0x01;
    out[x / 8] |= static_cast<uint8_t>(pixel << (7 - x % 8));
  }
}

// Panel rows [row0, row0 + rows), and in each the bytes [byte0, byte0 + rowBytes).
struct Span {
  int row0 = 0;
  int rows = 0;
  int byte0 = 0;
  int rowBytes = 0;
  size_t size() const { return static_cast<size_t>(rows) * static_cast<size_t>(rowBytes); }
};

// The span of the logical rectangle [x, x + w) x [y, y + h), clipped to the panel. False when that leaves nothing.
inline bool spanOf(const int orientation, const int x, const int y, const int w, const int h, const int panelWidth,
                   const int panelHeight, Span* out) {
  if (w <= 0 || h <= 0) return false;
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  // Two opposite corners: every orientation is a quarter turn or a flip, so they bound the rectangle's image.
  toPanel(orientation, x, y, panelWidth, panelHeight, &x0, &y0);
  toPanel(orientation, x + w - 1, y + h - 1, panelWidth, panelHeight, &x1, &y1);
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= panelWidth) x1 = panelWidth - 1;
  if (y1 >= panelHeight) y1 = panelHeight - 1;
  if (x0 > x1 || y0 > y1) return false;
  out->row0 = y0;
  out->rows = y1 - y0 + 1;
  out->byte0 = x0 / 8;
  out->rowBytes = x1 / 8 - out->byte0 + 1;
  return true;
}

// The span's bytes of `frame` (rows of `stride` bytes) into `out`, row after row.
inline void copyOut(const uint8_t* frame, const int stride, const Span& span, uint8_t* out) {
  for (int row = 0; row < span.rows; row++) {
    memcpy(out + static_cast<size_t>(row) * span.rowBytes,
           frame + static_cast<size_t>(span.row0 + row) * stride + span.byte0, span.rowBytes);
  }
}

// copyOut()'s bytes back into `frame`.
inline void copyIn(uint8_t* frame, const int stride, const Span& span, const uint8_t* in) {
  for (int row = 0; row < span.rows; row++) {
    memcpy(frame + static_cast<size_t>(span.row0 + row) * stride + span.byte0,
           in + static_cast<size_t>(row) * span.rowBytes, span.rowBytes);
  }
}

// FNV-1a over the frame's bytes, a word at a time: what tells a frame drawn again from the one it is to equal. Not a
// hash for anything adversarial; 13,068 words on the X3's panel.
inline uint32_t digest(const uint8_t* frame, const size_t size) {
  uint32_t hash = 2166136261u;
  size_t i = 0;
  for (; i + 4 <= size; i += 4) {
    uint32_t word;
    memcpy(&word, frame + i, sizeof(word));
    hash = (hash ^ word) * 16777619u;
  }
  for (; i < size; i++) hash = (hash ^ frame[i]) * 16777619u;
  return hash;
}

}  // namespace frameregion
