#pragma once

#include <HalStorage.h>
#include <PngStreamDecoder.h>

#include <cstdint>

#include "BitmapHelpers.h"

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
  // targetWidth/targetHeight: desired output size (fit, not crop).
  // Returns false on any setup failure; the session must not be used after a false return.
  bool begin(FsFile& pngFile, FsFile& bmpFile, int targetWidth, int targetHeight);

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

  // Output sink (borrowed — caller keeps the FsFile alive)
  Print* bmpOut_ = nullptr;
};

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true);

 public:
  static bool pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop = true);
  static bool pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
