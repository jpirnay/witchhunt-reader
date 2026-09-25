#pragma once

#include <ZipFile.h>
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
  // When needMore is non-null it is set to true iff the walk ran out of buffer with the marker
  // structure still consistent — the header simply continues past `len` (large Exif/IPTC/XMP/ICC
  // segments) and a longer read would reach SOF — and to false for every other failure (not a
  // JPEG, SOS before any SOF, corrupt segment length).
  static bool getDimensionsFromBuffer(const uint8_t* buf, size_t len, ImageDimensions& out, JpegMode* outMode = nullptr,
                                      bool* needMore = nullptr);

  // Stream the SOF marker out of a ZIP entry, skipping over arbitrarily large leading
  // metadata segments (Exif thumbnails, XMP, ICC profiles) that can push the SOF past
  // any fixed header buffer. O(1) memory — uses a resumable ZipFile::EntryReader.
  // When outMode is non-null it is set to the coding mode of the first SOF marker.
  static bool getDimensionsFromZipEntryStreaming(const std::string& epubPath, const std::string& entryPath,
                                                 ImageDimensions& out, JpegMode* outMode = nullptr);
  // The same walk over a reader the caller has already opened on the entry, for a caller that
  // holds the archive and the entry's central-directory stat (the image manifest) — no second
  // ZipFile, no second central-directory scan. The reader's ring is the only memory involved.
  // needMore (optional): set when the stream ENDED with the marker chain still consistent -- a
  // reader opened with an output cap stopped before SOF, and a longer read would find it -- as
  // distinct from a header no read can fix (bad SOI, SOS before SOF, corrupt lengths).
  static bool getDimensionsFromEntryReader(ZipFile::EntryReader& reader, ImageDimensions& out,
                                           JpegMode* outMode = nullptr, bool* needMore = nullptr);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "JPEG"; }
};
