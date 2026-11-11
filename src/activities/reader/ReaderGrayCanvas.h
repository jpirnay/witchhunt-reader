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

// The tone every picture sample takes on its way to the canvas: v' = 255 * (v / 255)^0.28, rounded. The driver aims
// its levels at equal steps of drive, not of reflectance, and a picture comes out far too dark without this; the
// exponent was chosen by eye on the T5S3 over 0.40 and 0.55 on real photos. Pictures only: text, rules and the sleep
// screen take none.
inline constexpr uint8_t kPictureTone[256] = {
    0,   54,  66,  74,  80,  85,  89,  93,  97,  100, 103, 106, 108, 111, 113, 115, 117, 119, 121, 123, 125, 127,
    128, 130, 132, 133, 135, 136, 137, 139, 140, 141, 143, 144, 145, 146, 147, 149, 150, 151, 152, 153, 154, 155,
    156, 157, 158, 159, 160, 161, 162, 162, 163, 164, 165, 166, 167, 168, 168, 169, 170, 171, 172, 172, 173, 174,
    175, 175, 176, 177, 178, 178, 179, 180, 180, 181, 182, 182, 183, 184, 184, 185, 186, 186, 187, 187, 188, 189,
    189, 190, 191, 191, 192, 192, 193, 193, 194, 195, 195, 196, 196, 197, 197, 198, 198, 199, 199, 200, 200, 201,
    202, 202, 203, 203, 204, 204, 205, 205, 206, 206, 206, 207, 207, 208, 208, 209, 209, 210, 210, 211, 211, 212,
    212, 213, 213, 213, 214, 214, 215, 215, 216, 216, 216, 217, 217, 218, 218, 219, 219, 219, 220, 220, 221, 221,
    221, 222, 222, 223, 223, 223, 224, 224, 225, 225, 225, 226, 226, 227, 227, 227, 228, 228, 228, 229, 229, 229,
    230, 230, 231, 231, 231, 232, 232, 232, 233, 233, 233, 234, 234, 234, 235, 235, 236, 236, 236, 237, 237, 237,
    238, 238, 238, 239, 239, 239, 240, 240, 240, 241, 241, 241, 242, 242, 242, 242, 243, 243, 243, 244, 244, 244,
    245, 245, 245, 246, 246, 246, 247, 247, 247, 247, 248, 248, 248, 249, 249, 249, 250, 250, 250, 250, 251, 251,
    251, 252, 252, 252, 252, 253, 253, 253, 254, 254, 254, 254, 255, 255,
};

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
