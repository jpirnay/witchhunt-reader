#pragma once
#include <AdaptiveTone.h>
#include <HalStorage.h>

#include <memory>
#include <string>

#include "ImageDimensionsGuard.h"

class GfxRenderer;
struct DirectGray8Writer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

enum class ImageDitherMode : uint8_t {
  Bayer = 0,
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  Atkinson = 1,
  DiffusedBayer = 2,
#endif
  COUNT,
};

inline ImageDitherMode imageDitherModeFromSetting(uint8_t value) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  switch (static_cast<ImageDitherMode>(value)) {
    case ImageDitherMode::Bayer:
    case ImageDitherMode::Atkinson:
    case ImageDitherMode::DiffusedBayer:
      return static_cast<ImageDitherMode>(value);
    case ImageDitherMode::COUNT:
    default:
      return ImageDitherMode::Bayer;
  }
#else
  (void)value;
  return ImageDitherMode::Bayer;
#endif
}

inline const char* getImageDitherCacheSuffix(ImageDitherMode mode) {
  switch (mode) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    case ImageDitherMode::Atkinson:
      return ".atkinson";
    case ImageDitherMode::DiffusedBayer:
      return ".diffused-bayer";
#endif
    case ImageDitherMode::Bayer:
    case ImageDitherMode::COUNT:
    default:
      return ".bayer";
  }
}

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  ImageDitherMode ditherMode = ImageDitherMode::Bayer;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  // If true, the decoder uses a 1-bit Atkinson dither and emits only the values 0/3 — suitable for
  // pure black-and-white display (no grayscale planes). The 4-level dither path produces values 1/2
  // which the BW DirectPixelWriter collapses to black, making mid-grays render very dark.
  bool monochromeOutput = false;
  // Adaptive tone mapping, applied to each sample before dithering. Inactive by
  // default, in which case the pixel pipeline is untouched. The caller derives these
  // points itself (see PngToFramebufferConverter::analyzeAdaptiveTone) because a PNG
  // cannot be rewound -- building the histogram costs a full extra decode, and the
  // sleep screen renders three passes that must all share one set of points.
  adaptive_tone::Points adaptiveTone;
  std::string cachePath;  // If non-empty, decoder will write pixel cache to this path
  // If non-empty, the decoder ALSO writes the other dither variant of the same decode to this
  // path -- 4-level Bayer when monochromeOutput is set, 1-bit Atkinson when it is not. Cache
  // only: the companion is never drawn, so the framebuffer still shows exactly the variant
  // `monochromeOutput` selects.
  //
  // This is what lets the reader's BW and grayscale .pxc pair come out of ONE inflate. It is
  // only sound because neither variant is tone-mapped (see ImageBlock::render): a tone curve
  // has to be derived from a completed histogram, which a PNG cannot produce without a second
  // full pass, and the two sinks would then need different source samples.
  //
  // Honoured by PngToFramebufferConverter. Other decoders ignore it and write only cachePath;
  // callers must treat the companion as best-effort and check before relying on it.
  std::string companionCachePath;
  // Native-grayscale sink, for panels that resolve more than four levels
  // (HalDisplay::getGrayLevels() > 4). When set, the decoder stores the
  // tone-mapped 8-bit sample here and writes NEITHER the framebuffer nor any
  // cache: dithering to 2 bits is precisely the loss this path exists to avoid,
  // and a 2-bit .pxc could not replay a 16-level frame anyway.
  //
  // One decode, one output. The three-pass BW/LSB/MSB dance the plane pipeline
  // needs has no counterpart here — there are no planes to fill.
  //
  // Honoured by PngToFramebufferConverter. Other decoders ignore it, so callers
  // must check getGrayLevels() AND the format before relying on it.
  DirectGray8Writer* gray8 = nullptr;
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
