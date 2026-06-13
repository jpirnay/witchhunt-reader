#pragma once

#include <cstdint>
#include <vector>

#include "ImageToFramebufferDecoder.h"

class GifToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);
  // Parse dimensions from already-read header bytes (only needs first 10 bytes).
  static bool getDimensionsFromBuffer(const uint8_t* buf, size_t len, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "GIF"; }

  // Decode the first frame of a GIF file to raw grayscale pixels (0=black, 255=white).
  // Output is row-major, outDims.width * outDims.height bytes.
  // No device dependencies — suitable for use in host unit tests.
  static bool decodeFirstFrameToGrayscale(const std::string& path, std::vector<uint8_t>& outPixels,
                                          ImageDimensions& outDims);
};
