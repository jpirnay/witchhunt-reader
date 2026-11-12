#pragma once

// Dark mode keeps pictures as they are: the panel shows the frame inverted (HalDisplay::setInverted),
// so a picture's box is inverted once more in the frame buffer and the two cancel. The geometry of
// that, free of Arduino so the host can test it (test/image_polarity). GfxRenderer::
// preserveImagePolarity() is the caller.
//
// Ported from crosspoint-reader PR #2705 (Justin Mitchell, GfxRenderer::preserveImagePolarity), which
// does the same XOR inside the renderer; split out here for the tests.

#include <cstdint>

#include "TouchTransform.h"  // touchtransform::Orientation mirrors GfxRenderer::Orientation (asserted there)

namespace image_polarity {

// A box on the panel, inclusive on both ends, in physical pixels (x along a frame-buffer row).
struct PanelRect {
  int x0;
  int y0;
  int x1;
  int y1;
};

// The panel pixels a logical box covers in `orientation` (a GfxRenderer::Orientation, which
// touchtransform::Orientation mirrors), clipped to the panel. False when nothing of it is on the
// panel. The mapping is GfxRenderer's rotateCoordinates() applied to the box's corners.
inline bool panelRectOf(const int orientation, const int panelWidth, const int panelHeight, const int x, const int y,
                        const int width, const int height, PanelRect& out) {
  if (width <= 0 || height <= 0) return false;
  const int lx1 = x + width - 1;
  const int ly1 = y + height - 1;
  int px0;
  int py0;
  int px1;
  int py1;
  switch (orientation) {
    case touchtransform::Portrait:  // phyX = y, phyY = H-1-x
      px0 = y;
      px1 = ly1;
      py0 = panelHeight - 1 - lx1;
      py1 = panelHeight - 1 - x;
      break;
    case touchtransform::LandscapeClockwise:  // phyX = W-1-x, phyY = H-1-y
      px0 = panelWidth - 1 - lx1;
      px1 = panelWidth - 1 - x;
      py0 = panelHeight - 1 - ly1;
      py1 = panelHeight - 1 - y;
      break;
    case touchtransform::PortraitInverted:  // phyX = W-1-y, phyY = x
      px0 = panelWidth - 1 - ly1;
      px1 = panelWidth - 1 - y;
      py0 = x;
      py1 = lx1;
      break;
    default:  // touchtransform::LandscapeCounterClockwise: the panel's own frame
      px0 = x;
      px1 = lx1;
      py0 = y;
      py1 = ly1;
      break;
  }
  if (px0 < 0) px0 = 0;
  if (py0 < 0) py0 = 0;
  if (px1 > panelWidth - 1) px1 = panelWidth - 1;
  if (py1 > panelHeight - 1) py1 = panelHeight - 1;
  if (px0 > px1 || py0 > py1) return false;
  out = PanelRect{px0, py0, px1, py1};
  return true;
}

// Flip every pixel of `r` in a 1-bpp, MSB-first frame buffer `widthBytes` wide: whole bytes in the
// middle of a row, a mask for a byte the box only partly covers at either end.
inline void invertPanelRect(uint8_t* frame, const int widthBytes, const PanelRect& r) {
  const int firstByte = r.x0 >> 3;
  const int lastByte = r.x1 >> 3;
  const auto headMask = static_cast<uint8_t>(0xFFu >> (r.x0 & 7));
  const auto tailMask = static_cast<uint8_t>(0xFFu << (7 - (r.x1 & 7)));
  for (int row = r.y0; row <= r.y1; ++row) {
    uint8_t* line = frame + static_cast<uint32_t>(row) * widthBytes;
    if (firstByte == lastByte) {
      line[firstByte] ^= static_cast<uint8_t>(headMask & tailMask);
      continue;
    }
    line[firstByte] ^= headMask;
    for (int b = firstByte + 1; b < lastByte; ++b) line[b] = static_cast<uint8_t>(~line[b]);
    line[lastByte] ^= tailMask;
  }
}

}  // namespace image_polarity
