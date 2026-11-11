#pragma once

#include <cstdint>

// Composing a page onto a borrowed 8-bit canvas (GfxRenderer::borrowGray8Canvas), from the buffers the 1-bit page
// render leaves: the framebuffer and the two anti-aliasing planes its capture fills (GfxRenderer::beginGrayCapture).
// All three are PHYSICAL panel layout, `widthBytes` bytes a row and the most significant bit the leftmost pixel, as is
// the canvas (`stride` bytes a row, 0x00 black .. 0xFF white), so no orientation applies. Apart from GfxRenderer so the
// host can test it.
namespace gray8compose {

// The framebuffer's BLACK pixels (bit clear) as 0x00, every other canvas byte left as it is.
inline void stampBlack(uint8_t* canvas, const uint16_t stride, const uint8_t* frameBuffer, const int widthBytes,
                       const int width, const int height) {
  for (int y = 0; y < height; y++) {
    const uint8_t* src = frameBuffer + static_cast<uint32_t>(y) * widthBytes;
    uint8_t* dst = canvas + static_cast<uint32_t>(y) * stride;
    for (int bx = 0; bx < widthBytes; bx++) {
      const uint8_t bits = src[bx];
      // The overwhelmingly common case on a text page: a fully white byte has nothing to stamp, so skip the eight-bit
      // unpack entirely.
      if (bits == 0xFF) continue;
      const int x0 = bx * 8;
      for (int bit = 0; bit < 8; bit++) {
        const int x = x0 + bit;
        if (x >= width) break;
        if (!(bits & (0x80 >> bit))) dst[x] = 0x00;
      }
    }
  }
}

// The anti-aliasing greys the capture recorded, as the panel's own single push reads its planes: LSB set is the dark
// grey, MSB alone the light one. Such a pixel is black in the framebuffer (the B/W pass draws every non-white glyph
// pixel), so this goes after stampBlack() and over it. Every other canvas byte is left as it is.
inline void overlayAntiAliasing(uint8_t* canvas, const uint16_t stride, const uint8_t* lsbPlane,
                                const uint8_t* msbPlane, const int widthBytes, const int width, const int height,
                                const uint8_t darkByte, const uint8_t lightByte) {
  for (int y = 0; y < height; y++) {
    const uint8_t* lsb = lsbPlane + static_cast<uint32_t>(y) * widthBytes;
    const uint8_t* msb = msbPlane + static_cast<uint32_t>(y) * widthBytes;
    uint8_t* dst = canvas + static_cast<uint32_t>(y) * stride;
    for (int bx = 0; bx < widthBytes; bx++) {
      const uint8_t l = lsb[bx];
      const uint8_t m = msb[bx];
      if ((l | m) == 0) continue;  // no glyph edge in these eight pixels
      const int x0 = bx * 8;
      for (int bit = 0; bit < 8; bit++) {
        const int x = x0 + bit;
        if (x >= width) break;
        const uint8_t mask = 0x80 >> bit;
        if (l & mask) {
          dst[x] = darkByte;
        } else if (m & mask) {
          dst[x] = lightByte;
        }
      }
    }
  }
}

}  // namespace gray8compose
