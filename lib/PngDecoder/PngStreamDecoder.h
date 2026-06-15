#pragma once

#include <HalStorage.h>     // FsFile
#include <InflateReader.h>  // uzlib streaming inflate

#include <cstdint>

// Streaming PNG decoder built on uzlib (InflateReader) instead of PNGdec.
//
// Why: PNGdec's PNGIMAGE object is ~49.5 KB (32 KB zlib window + ~7 KB inflate
// state + 6.5 KB pixel buffer + 2 KB file buffer), which is *larger* than the
// 48 KB framebuffer the reader frees to make room for it — so `new PNG()` fails
// intermittently under heap fragmentation and images silently vanish. This
// decoder needs only the inherent DEFLATE window (≤32 KB, sized down for small
// images) plus a couple of scanline buffers — ~35 KB peak, comfortably inside
// the freed framebuffer. It reuses the same uzlib path already used for cover
// thumbnails.
//
// Pull/iterator model: begin() parses the header and primes the inflate stream;
// nextRow() returns one source scanline at a time, top to bottom. Callers keep
// their own scaling/dithering/output loops.
//
// Supported: non-interlaced PNG, colour types 0/2/3/4/6, bit depths 1/2/4/8/16,
// all five scanline filters. Alpha (channel alpha or tRNS) is honoured.
class PngStreamDecoder {
 public:
  struct Info {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bitDepth = 0;
    uint8_t colorType = 0;
  };

  PngStreamDecoder() = default;
  ~PngStreamDecoder();
  PngStreamDecoder(const PngStreamDecoder&) = delete;
  PngStreamDecoder& operator=(const PngStreamDecoder&) = delete;

  // Parse signature + IHDR + PLTE/tRNS up to the first IDAT and initialise the
  // inflate stream. Fills `info`. Returns false on a malformed, unsupported
  // (interlaced) or oversized image, or if buffer allocation fails.
  bool begin(FsFile& file, Info& info);

  // Decode the next source scanline (top to bottom).
  //   grayOut  : receives `info.width` 8-bit grayscale samples. When alphaOut is
  //              null, transparent pixels are composited over white (the opaque
  //              "draw onto a blank page" behaviour the reader and BMP paths want).
  //   alphaOut : optional; when non-null, receives `info.width` 8-bit alpha
  //              values and grayOut holds the *uncomposited* brightness, so an
  //              overlay caller can skip transparent pixels and keep the pixels
  //              underneath. Pass null when you don't need alpha.
  // Returns false when all rows have been produced or on a decode error.
  bool nextRow(uint8_t* grayOut, uint8_t* alphaOut = nullptr);

  // Release buffers. Idempotent; also called by the destructor.
  void end();

 private:
  // uzlib read callback. InflateReader (hence its uzlib_uncomp) is the first
  // member, so uzlib_uncomp* casts back to PngStreamDecoder*.
  static int idatReadCallback(uzlib_uncomp* uncomp);
  bool findNextIdatChunk();
  bool decodeFilteredRow();  // inflate + reverse-filter into currentRow_

  InflateReader reader_;  // MUST be first member (see idatReadCallback)
  FsFile* file_ = nullptr;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint8_t bitDepth_ = 0;
  uint8_t colorType_ = 0;
  uint8_t bytesPerPixel_ = 0;  // for the reverse filter (sub-byte depths use 1)
  uint32_t rawRowBytes_ = 0;
  uint32_t rowsProduced_ = 0;

  uint8_t* currentRow_ = nullptr;
  uint8_t* previousRow_ = nullptr;

  uint32_t chunkBytesRemaining_ = 0;
  bool idatFinished_ = false;

  uint8_t readBuf_[2048];  // IDAT feed buffer for uzlib

  uint8_t palette_[256 * 3];
  int paletteSize_ = 0;
  uint8_t paletteAlpha_[256];  // tRNS for indexed; 255 (opaque) by default
  bool hasPaletteAlpha_ = false;

  // tRNS colour-key for grayscale / truecolour images.
  bool hasColorKey_ = false;
  uint16_t keyGray_ = 0;
  uint16_t keyR_ = 0, keyG_ = 0, keyB_ = 0;

  bool started_ = false;
};
