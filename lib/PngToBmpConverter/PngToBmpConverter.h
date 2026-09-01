#pragma once

#include <HalStorage.h>
#include <PngStreamDecoder.h>

#include <cstdint>
#include <memory>

#include "BitmapHelpers.h"
#include "BufferedPrint.h"

class Print;

// Stateful PNG-to-BMP decode session for sliced execution across multiple loop() calls.
// Usage:
//   auto s = std::make_unique<PngDecodeSession>();
//   if (!s->begin(pngFile, bmpFile, w, h)) { /* error */ }
//   while (true) {
//     auto status = s->continueRows(4);
//     if (status == PngDecodeSession::Status::Running) return;  // yield
//     if (status == PngDecodeSession::Status::Done) { /* success */ break; }
//     /* error */ break;
//   }
class PngDecodeSession {
 public:
  enum class Status { Running, Done, Error };

  PngDecodeSession() = default;
  ~PngDecodeSession() { cleanup(); }
  PngDecodeSession(const PngDecodeSession&) = delete;
  PngDecodeSession& operator=(const PngDecodeSession&) = delete;

  // Open pngFile and bmpFile (already opened for read/write respectively),
  // parse the PNG header, write the BMP header, allocate buffers.
  // targetWidth/targetHeight: desired output size. crop=true fills the target (scale to the
  // LARGER fit factor) and center-crops the overflow, so the BMP is EXACTLY the target size —
  // this mirrors the synchronous pngFileTo1BitBmpStreamWithSize (crop=true) so cover thumbnails
  // are drawn 1:1 with no rescale (a fractional rescale of an already-dithered 1-bit image
  // produces a moiré grid). crop=false fits inside the target (scale to the SMALLER factor).
  // Returns false on any setup failure; the session must not be used after a false return.
  bool begin(FsFile& pngFile, FsFile& bmpFile, int targetWidth, int targetHeight, bool crop = true);

  // Decode up to maxSourceRows scanlines and write the corresponding BMP rows.
  // Returns Running if more rows remain, Done when the image is complete, Error on failure.
  Status continueRows(uint32_t maxSourceRows);

  // Number of source rows decoded so far (for progress logging).
  uint32_t rowsDone() const { return srcY_; }
  uint32_t totalRows() const { return height_; }

 private:
  void cleanup();
  void flushScaledRow();
  void writeOutputRow(const uint8_t* gray);

  PngStreamDecoder decoder_;

  // Output geometry
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  int outWidth_ = 0;
  int outHeight_ = 0;
  int bytesPerRow_ = 0;
  bool needsScaling_ = false;
  uint32_t scaleX_fp_ = 65536;
  uint32_t scaleY_fp_ = 65536;
  // Center-crop window emitted to the BMP (crop=true trims the overfill dimension;
  // see begin()). The full outWidth_ row is still dithered for diffusion correctness.
  int outCropX_ = 0;
  int outCropY_ = 0;
  int finalW_ = 0;
  int finalH_ = 0;

  // Row-loop state
  uint32_t srcY_ = 0;
  int currentOutY_ = 0;
  uint32_t nextOutY_srcStart_ = 0;

  // Heap buffers
  uint8_t* grayRow_ = nullptr;
  uint8_t* rowBuffer_ = nullptr;
  uint32_t* rowAccum_ = nullptr;
  uint16_t* rowCount_ = nullptr;
  Atkinson1BitDitherer* ditherer_ = nullptr;

  // Output sink (borrowed — caller keeps the FsFile alive), wrapped so the per-row writes
  // coalesce (see BufferedPrint). Owned because the session spans many loop() ticks.
  std::unique_ptr<BufferedPrint> bmpOut_;
};

class PngToBmpConverter {
  // enforceSizeCap: reject sources above MAX_PNG_PIXELS before decoding. This guards the ~10 s
  // full-resolution decode stall on the per-tick thumbnail path (whose callers recover via the
  // sliced PngDecodeSession). One-shot, stall-tolerant callers (generateCoverBmp for the sleep /
  // finished-book / OPDS screens, which have no sliced fallback) pass false so a large but
  // otherwise-decodable cover still renders. The per-dimension safety bound in PngStreamDecoder
  // (2048x3072) always applies regardless.
  static bool pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true, bool enforceSizeCap = true, bool eightBit = false);

 public:
  // grayscale8Bit: see JpegToBmpConverter::jpegFileToBmpStream.
  static bool pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop = true, bool grayscale8Bit = false);
  static bool pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
