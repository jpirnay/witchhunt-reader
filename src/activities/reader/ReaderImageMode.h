#pragma once

#include <cstdint>

#include "CrossPointSettings.h"

// The reader's Images setting (CrossPointSettings::IMAGE_RENDERING) as the reader's layout, gray-pass and refresh
// decisions read it. Apart from the reader so the host can test it.
//
// Black & White lays images out exactly as Display does and differs only in how they reach the panel. The B/W pass
// draws every image from its 1-bit pixel cache, an Atkinson dither of the decoded grays made at the size the image is
// drawn (ImageBlock::render, monochromeOutput). Under Display the gray pass then replays the image's 4-level cache over
// that; under Black & White it does not, so the image stays as the B/W pass drew it and its page is a text page to
// every gray-pass and refresh decision. Text keeps its anti-aliasing either way.
namespace ReaderImageMode {

// The value a chapter is laid out with and its section cache keyed by. Black & White is laid out as Display, so
// switching between the two keeps the cache, and the parser, which knows only display, placeholder and suppress, draws
// the book's images, CSS background pictures included.
constexpr uint8_t forLayout(const uint8_t mode) {
  return mode == CrossPointSettings::IMAGES_DISPLAY_BW ? static_cast<uint8_t>(CrossPointSettings::IMAGES_DISPLAY)
                                                       : mode;
}

// Whether the gray pass replays the images' 4-level cache, and so whether that cache is worth decoding.
constexpr bool imagesInGray(const uint8_t mode) { return mode != CrossPointSettings::IMAGES_DISPLAY_BW; }

// Whether a page's images take part in its gray pass. A page with none is a text page to the gray pass, the one-pass
// AA paths and the pre-render.
constexpr bool imagesTakeGrayPass(const bool pageHasImages, const uint8_t mode) {
  return pageHasImages && imagesInGray(mode);
}

// Whether a page whose images were drawn owes the page after it the half refresh of "Refresh after image pages", which
// clears the gray an image leaves behind. Black & White images leave none.
constexpr bool imagePageOwesRefresh(const uint8_t mode) {
  return mode != CrossPointSettings::IMAGES_SUPPRESS && imagesInGray(mode);
}

// What "Refresh after image pages" still owes once a page is drawn. `owedBefore` is what was pending as the draw
// began: a new page has paid it already, so it arrives set only on a redraw of the page on screen (a clock tick, a menu
// closing), which keeps it for the turn away from that page. `drewImages`: this draw put an image, not a placeholder,
// on the panel. A draw in a mode that leaves no gray owes nothing, so a Black & White redraw also drops the debt a
// Display draw of the same page left.
constexpr bool refreshOwedAfterDraw(const bool owedBefore, const bool drewImages, const uint8_t mode) {
  return imagePageOwesRefresh(mode) && (owedBefore || drewImages);
}

// The page summary's images= field: none on the page, drawn with their gray pass, or in black and white.
constexpr const char* logLabel(const bool pageHasImages, const uint8_t mode) {
  return !pageHasImages ? "none" : imagesInGray(mode) ? "gray" : "bw";
}

}  // namespace ReaderImageMode
