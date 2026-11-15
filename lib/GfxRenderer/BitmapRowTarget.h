#pragma once

#include <cmath>
#include <cstdint>

// Where GfxRenderer::drawBitmap() puts one row of a BMP, free of Arduino so the host can test it
// (test/bitmap_rows). The rows come off the card in file order, and an ordinary BMP stores them
// bottom-up: the first row read is the image's last.
namespace bitmap_rows {

enum class RowFate : uint8_t {
  Draw,  // on the screen, at screenY
  Skip,  // off the screen or cropped away, but a later row may land on it: read it and go on
  Stop,  // off the screen, and every later row is too: nothing more to read
};

struct RowTarget {
  RowFate fate;
  int screenY;
};

// Row `bmpY`, counted in file order, of a `height`-row bitmap stored `topDown` (or bottom-up), with
// `cropPixY` rows cut from each end, scaled by `scale` when `isScaled`, its top at screen row `y`
// on a `screenHeight`-row screen.
inline RowTarget rowTarget(const int bmpY, const int height, const bool topDown, const int cropPixY,
                           const bool isScaled, const float scale, const int y, const int screenHeight) {
  int screenY = -cropPixY + (topDown ? bmpY : height - 1 - bmpY);
  if (isScaled) {
    screenY = static_cast<int>(std::floor(static_cast<float>(screenY) * scale));
  }
  screenY += y;  // the offset should not be scaled
  // Top-down rows walk down the screen, bottom-up rows walk up it. Only a row past the edge they walk
  // towards ends the read: a bottom-up image taller than the screen starts below it, and stopping
  // there drew nothing at all (crosspoint-reader PR #4472, Justin Mitchell).
  if (screenY >= screenHeight) return {topDown ? RowFate::Stop : RowFate::Skip, screenY};
  if (screenY < 0) return {topDown ? RowFate::Skip : RowFate::Stop, screenY};
  if (bmpY < cropPixY) return {RowFate::Skip, screenY};
  return {RowFate::Draw, screenY};
}

}  // namespace bitmap_rows
