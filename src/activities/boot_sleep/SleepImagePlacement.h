#pragma once

#include <ImageScale.h>

#include <cstdint>

// Where SleepActivity puts a custom sleep BMP and how much of it is cut off, free of Arduino so the
// host can test it (test/sleep_image_placement). GfxRenderer::drawBitmap() draws the result: it scales
// an image down to fit the bounds, never up.
namespace SleepImagePlacement {

struct Placement {
  int x = 0;
  int y = 0;
  BoxCut draw;  // drawBitmap's bounds and visible part
};

// Fit (`crop` false): the whole image, scaled down when it is larger than the screen, centred.
// Crop: the screen filled where the image is large enough to fill it.
inline Placement place(const int imageWidth, const int imageHeight, const int pageWidth, const int pageHeight,
                       const bool crop) {
  Placement p;
  p.draw = BoxCut{pageWidth, pageHeight, 0, 0};
  if (imageWidth <= pageWidth && imageHeight <= pageHeight) {
    p.x = (pageWidth - imageWidth) / 2;
    p.y = (pageHeight - imageHeight) / 2;
    return p;
  }

  if (crop && (imageWidth < pageWidth || imageHeight < pageHeight)) {
    // Larger than the screen one way and smaller the other. Cut to the screen's shape, as below, it
    // would need scaling up to fill the screen, which drawBitmap does not do: it came out unscaled in
    // the top-left corner, short on both sides (reported against CrossInk as #925, an X4-sized
    // 480x800 image on the X3's 528x792). Only the overflow is cut, and the short side centred.
    if (imageWidth > pageWidth) {
      p.draw.visibleWidth = pageWidth;
      p.y = (pageHeight - imageHeight) / 2;
    } else {
      p.draw.visibleHeight = pageHeight;
      p.x = (pageWidth - imageWidth) / 2;
    }
    return p;
  }

  // Larger than the screen: crop cuts it to the screen's shape and covers the screen exactly.
  if (crop) {
    p.draw = cutToBox(imageWidth, imageHeight, pageWidth, pageHeight);
    return p;
  }

  // Fit: scaled down, and centred on the side the scale leaves short. Cross-multiplied, the shapes
  // compare exactly.
  const int64_t imageW = imageWidth;
  const int64_t imageH = imageHeight;
  if (imageW * pageHeight > pageWidth * imageH) {
    p.y = roundedQuotient(pageHeight * imageW - pageWidth * imageH, 2 * imageW);
  } else {
    p.x = roundedQuotient(pageWidth * imageH - pageHeight * imageW, 2 * imageH);
  }
  return p;
}

}  // namespace SleepImagePlacement
