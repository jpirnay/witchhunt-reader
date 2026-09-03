#pragma once

#include "ImageToFramebufferDecoder.h"

class PngToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);
  // Parse dimensions from already-read header bytes (only needs first 24 bytes).
  static bool getDimensionsFromBuffer(const uint8_t* buf, size_t len, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  // Decode from an ALREADY-OPEN file whose read position is the PNG's first byte. The decoder
  // only ever reads forward and seeks relatively, so the stream does not have to start at offset
  // 0 of the file -- which is what lets a PNG stored uncompressed inside an EPUB be decoded in
  // place, with no extraction to SD first (see Epub::getStoredItemRange).
  //
  // `label` names the source in logs only. The caller owns the file and must keep it open for
  // the whole call; this never closes it.
  static bool decodeOpenFile(FsFile& file, const std::string& label, GfxRenderer& renderer, const RenderConfig& config);

  // Builds a luminance histogram for adaptive tone mapping and returns the derived
  // black/white points, or an inactive result if the image does not need (or cannot
  // support) the correction.
  //
  // Costs a FULL EXTRA DECODE: unlike a BMP, a PNG cannot be rewound, so there is no
  // cheap pre-pass -- inflate must run over every row. Call this once and reuse the
  // result across render passes; do not call it per pass.
  // `mode` selects the endpoint stretch or the CDF equalization; see AdaptiveTone.h.
  // `eqBlendNum` is the Equalize strength; see adaptive_tone::EQ_BLEND_NUM_DEEP for
  // when a caller should raise it.
  static adaptive_tone::Points analyzeAdaptiveTone(const std::string& imagePath,
                                                   adaptive_tone::Mode mode = adaptive_tone::Mode::Stretch,
                                                   int eqBlendNum = adaptive_tone::EQ_BLEND_NUM);

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "PNG"; }
};