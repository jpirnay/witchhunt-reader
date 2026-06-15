#pragma once

#include <stdint.h>

#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  // Coarse JPEG coding mode, derived from the SOF marker. Drives engine selection:
  // only Baseline (SOF0) is decodable by TJpgDec — it rejects everything else — so
  // Progressive (SOF2) and Other (extended-sequential, arithmetic, …) render a placeholder.
  enum class JpegMode : uint8_t { Baseline, Progressive, Other };

  // Determine the coding mode from already-read header bytes. Returns false if no
  // SOF marker is found within the buffer (then *out is left untouched).
  static bool getModeFromHeader(const std::string& imagePath, JpegMode& out);

  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);
  // Parse dimensions from already-read header bytes (no file I/O). Needs ~4 KB for typical JPEGs.
  // When outMode is non-null it is set to the coding mode of the first SOF marker.
  static bool getDimensionsFromBuffer(const uint8_t* buf, size_t len, ImageDimensions& out,
                                      JpegMode* outMode = nullptr);

  // Stream the SOF marker out of a ZIP entry, skipping over arbitrarily large leading
  // metadata segments (Exif thumbnails, XMP, ICC profiles) that can push the SOF past
  // any fixed header buffer. O(1) memory — uses a resumable ZipFile::EntryReader.
  // When outMode is non-null it is set to the coding mode of the first SOF marker.
  static bool getDimensionsFromZipEntryStreaming(const std::string& epubPath, const std::string& entryPath,
                                                 ImageDimensions& out, JpegMode* outMode = nullptr);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "JPEG"; }
};
