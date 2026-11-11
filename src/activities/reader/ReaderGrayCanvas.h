#pragma once

#include <Gray8Compose.h>

#include <cstdint>
#include <cstring>

#include "CrossPointSettings.h"

// The reader's pages with pictures on a panel that keeps a deeper gray buffer of its own
// (GfxRenderer::borrowGray8Canvas(), today the LilyGo T5S3). Such a page goes out on that buffer with the panel's full
// gray refresh, its pictures in their own grays, rather than as B/W plus the two anti-aliasing greys of the single
// push: those are cut for glyph edges and land a picture far too dark. Apart from the reader so the host can test it.
namespace ReaderGrayCanvas {

// Whether the page goes out on the canvas.
//   panelGrayLevels:  GfxRenderer::getGrayLevels(); 4 on every panel without such a buffer.
//   pageHasPictures:  the page has a picture the B/W pass draws as one, not as a "large image" box.
//   picturesAreBoxes: the page draws its pictures as boxes (the memory ladder gave up, or they wait for a build).
//   imageMode:        the Images setting in effect (CrossPointSettings::IMAGE_RENDERING); Display shows the gray.
constexpr bool pageGoesOutOnCanvas(const uint8_t panelGrayLevels, const bool pageHasPictures,
                                   const bool picturesAreBoxes, const uint8_t imageMode) {
  return panelGrayLevels > 4 && pageHasPictures && !picturesAreBoxes && imageMode == CrossPointSettings::IMAGES_DISPLAY;
}

// Text anti-aliasing on such a page, as canvas bytes, so its text looks like a text page's. The single push of a text
// page draws its two greys as nudges from black in the panel's fast bank; the canvas goes out through the clean bank
// and the driver's own linearisation, so the same greys are other bytes there. Measured on the T5S3 with a camera, on
// 160 px patches against black and white in the same frame: a text page's dark grey came out as black and its light
// one 0.02 above black (0 black, 1 white); on the canvas every byte up to 76 came out black and 84 at 0.02.
constexpr uint8_t kAntiAliasingDark = 0;
constexpr uint8_t kAntiAliasingLight = 84;

// What the 1-bit page render left, in panel layout (see gray8compose).
struct PageBuffers {
  const uint8_t* frameBuffer;
  const uint8_t* lsbPlane;  // the capture's planes; null when the page was drawn without anti-aliasing
  const uint8_t* msbPlane;
  int widthBytes;
  int width;
  int height;
};

// The page on the canvas, in this order: white; the page's black pixels, which are its text, its rules and every
// picture as the B/W pass dithered it; the anti-aliasing greys over the glyph edges they belong to; last each picture's
// own grays over its box (paintPictures(), whose result this returns). A picture paintPictures() leaves alone keeps
// its dither.
template <typename PaintPictures>
auto composePage(uint8_t* canvas, const uint16_t stride, const PageBuffers& page, PaintPictures&& paintPictures) {
  for (int y = 0; y < page.height; y++) memset(canvas + static_cast<uint32_t>(y) * stride, 0xFF, page.width);
  gray8compose::stampBlack(canvas, stride, page.frameBuffer, page.widthBytes, page.width, page.height);
  if (page.lsbPlane && page.msbPlane) {
    gray8compose::overlayAntiAliasing(canvas, stride, page.lsbPlane, page.msbPlane, page.widthBytes, page.width,
                                      page.height, kAntiAliasingDark, kAntiAliasingLight);
  }
  return paintPictures();
}

}  // namespace ReaderGrayCanvas
