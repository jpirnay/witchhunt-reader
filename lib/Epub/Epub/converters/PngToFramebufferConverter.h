#pragma once

#include "ImageToFramebufferDecoder.h"

class PngToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);
  // Parse dimensions from already-read header bytes (only needs first 24 bytes).
  static bool getDimensionsFromBuffer(const uint8_t* buf, size_t len, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  // Builds a luminance histogram for adaptive tone mapping and returns the derived
  // black/white points, or an inactive result if the image does not need (or cannot
  // support) the correction.
  //
  // Costs a FULL EXTRA DECODE: unlike a BMP, a PNG cannot be rewound, so there is no
  // cheap pre-pass -- inflate must run over every row. Call this once and reuse the
  // result across render passes; do not call it per pass.
  static adaptive_tone::Points analyzeAdaptiveTone(const std::string& imagePath);

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "PNG"; }
};