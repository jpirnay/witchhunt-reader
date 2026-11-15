#pragma once

#include <cmath>

// Where SleepActivity puts a custom sleep BMP and how much of it is cut off, free of Arduino so the
// host can test it (test/sleep_image_placement). GfxRenderer::drawBitmap() draws the result with the
// screen as its bounds: it scales an image down to fit them, never up.
namespace SleepImagePlacement {

struct Placement {
  int x = 0;
  int y = 0;
  // drawBitmap's crop: the share of the width (height) cut off, half at each end.
  float cropX = 0.0f;
  float cropY = 0.0f;
};

// Fit (`crop` false): the whole image, scaled down when it is larger than the screen, centred.
// Crop: the screen filled where the image is large enough to fill it.
inline Placement place(const int imageWidth, const int imageHeight, const int pageWidth, const int pageHeight,
                       const bool crop) {
  Placement p;
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
    //
    // Half a pixel more than the overflow: drawBitmap cuts the crop in whole pixels, rounding down,
    // and a share that came out a hair short of the overflow would leave the rest a hair larger than
    // the screen, which it would then scale down.
    if (imageWidth > pageWidth) {
      p.cropX = (static_cast<float>(imageWidth - pageWidth) + 0.5f) / static_cast<float>(imageWidth);
      p.y = (pageHeight - imageHeight) / 2;
    } else {
      p.cropY = (static_cast<float>(imageHeight - pageHeight) + 0.5f) / static_cast<float>(imageHeight);
      p.x = (pageWidth - imageWidth) / 2;
    }
    return p;
  }

  // Larger than the screen: scaled down, and placed for it.
  float ratio = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
  const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
  if (ratio > screenRatio) {
    // Wider than the screen's shape: the scaled-down image is centred vertically.
    if (crop) {
      p.cropX = 1.0f - (screenRatio / ratio);
      ratio = (1.0f - p.cropX) * static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
    }
    p.x = 0;
    p.y = static_cast<int>(std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2));
  } else {
    // Taller than the screen's shape: the scaled-down image is centred horizontally.
    if (crop) {
      p.cropY = 1.0f - (ratio / screenRatio);
      ratio = static_cast<float>(imageWidth) / ((1.0f - p.cropY) * static_cast<float>(imageHeight));
    }
    p.x = static_cast<int>(std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2));
    p.y = 0;
  }
  return p;
}

}  // namespace SleepImagePlacement
