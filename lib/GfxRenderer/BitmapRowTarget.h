#pragma once

#include <cstdint>

#include "ImageScale.h"

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

// Row `bmpY`, counted in file order, of a `height`-row bitmap stored `topDown` (or bottom-up), of which
// the `shownRows` rows from image row `cropTop` down are drawn, scaled by `scale`, their top at screen
// row `y` on a `screenHeight`-row screen.
inline RowTarget rowTarget(const int bmpY, const int height, const bool topDown, const int cropTop, const int shownRows,
                           const ImageScale scale, const int y, const int screenHeight) {
  const int fromTop = (topDown ? bmpY : height - 1 - bmpY) - cropTop;  // the row within the shown part
  const int screenY = scale.apply(fromTop) + y;                        // the offset is not scaled
  // Top-down rows walk down the image and the screen, bottom-up rows walk up them. Only a row past the
  // edge they walk towards ends the read: a bottom-up image taller than the screen starts below it, and
  // stopping there drew nothing at all (crosspoint-reader PR #4472, Justin Mitchell).
  if (fromTop >= shownRows || screenY >= screenHeight) return {topDown ? RowFate::Stop : RowFate::Skip, screenY};
  if (fromTop < 0 || screenY < 0) return {topDown ? RowFate::Skip : RowFate::Stop, screenY};
  return {RowFate::Draw, screenY};
}

}  // namespace bitmap_rows
