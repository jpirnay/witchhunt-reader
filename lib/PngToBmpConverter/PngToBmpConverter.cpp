#include "PngToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PngStreamDecoder.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Same as JpegToBmpConverter for consistency
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;
constexpr bool USE_ATKINSON = true;
constexpr bool USE_FLOYD_STEINBERG = false;
constexpr bool USE_PRESCALE = true;
// ============================================================================

// BMP writing helpers (same as JpegToBmpConverter)
inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

namespace {

void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 3) / 4 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 14 + 40 + paletteSize);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 8);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 256);
  write32(bmpOut, 256);

  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(i));
    bmpOut.write(static_cast<uint8_t>(0));
  }
}

void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 62);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 1);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 2);
  write32(bmpOut, 2);

  uint8_t palette[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 70);

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 2);
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 4);
  write32(bmpOut, 4);

  uint8_t palette[16] = {0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x00,
                         0xAA, 0xAA, 0xAA, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

}  // namespace

bool PngToBmpConverter::pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop) {
  LOG_DBG("PNG", "Converting PNG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  // Decode with the shared uzlib-based core (no PNGdec). Scanlines arrive as
  // 8-bit grayscale, top to bottom, with alpha already composited over white.
  PngStreamDecoder decoder;
  PngStreamDecoder::Info info;
  if (!decoder.begin(pngFile, info)) {
    LOG_ERR("PNG", "Failed to start PNG decode");
    return false;
  }
  const uint32_t width = info.width;
  const uint32_t height = info.height;

  // Reject images whose source pixel count would cause the row-by-row decode to
  // stall the main loop for tens of seconds.  Unlike JPEG (which has DCT pre-scaling),
  // the PNG decoder processes every source row at full resolution before downscaling.
  // 800*1200 = 960 kpx decodes in ~10 s on the ESP32-C3; cap there.
  constexpr uint32_t MAX_PNG_PIXELS = 800u * 1200u;
  if (width * height > MAX_PNG_PIXELS) {
    LOG_ERR("PNG", "Source PNG too large for thumbnail (%ux%u, max %u px) — skipping", width, height, MAX_PNG_PIXELS);
    return false;
  }

  // Calculate output dimensions (same logic as JpegToBmpConverter)
  int outWidth = width;
  int outHeight = height;
  uint32_t scaleX_fp = 65536;
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 &&
      (static_cast<int>(width) != targetWidth || static_cast<int>(height) != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / width;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / height;
    float scale = 1.0;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(width * scale);
    outHeight = static_cast<int>(height * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    scaleX_fp = (width << 16) / outWidth;
    scaleY_fp = (height << 16) / outHeight;
    needsScaling = true;

    LOG_DBG("PNG", "Scaling %ux%u -> %dx%d (target %dx%d)", width, height, outWidth, outHeight, targetWidth,
            targetHeight);
  }

  // Write BMP header
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  // Allocate BMP row buffer
  auto* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    LOG_ERR("PNG", "Failed to allocate row buffer");
    return false;
  }

  // Create ditherers (same as JpegToBmpConverter)
  AtkinsonDitherer* atkinsonDitherer = nullptr;
  FloydSteinbergDitherer* fsDitherer = nullptr;
  Atkinson1BitDitherer* atkinson1BitDitherer = nullptr;

  if (oneBit) {
    atkinson1BitDitherer = new Atkinson1BitDitherer(outWidth);
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      fsDitherer = new FloydSteinbergDitherer(outWidth);
    }
  }

  // Scaling accumulators
  uint32_t* rowAccum = nullptr;
  uint16_t* rowCount = nullptr;
  int currentOutY = 0;
  uint32_t nextOutY_srcStart = 0;

  if (needsScaling) {
    rowAccum = new uint32_t[outWidth]();
    rowCount = new uint16_t[outWidth]();
    nextOutY_srcStart = scaleY_fp;
  }

  // Grayscale row buffer — one source scanline at a time from the decoder.
  auto* grayRow = static_cast<uint8_t*>(malloc(width));
  if (!grayRow) {
    LOG_ERR("PNG", "Failed to allocate grayscale row buffer");
    delete[] rowAccum;
    delete[] rowCount;
    delete atkinsonDitherer;
    delete fsDitherer;
    delete atkinson1BitDitherer;
    free(rowBuffer);
    return false;
  }

  bool success = true;

  // Process each scanline
  for (uint32_t y = 0; y < height; y++) {
    if (!decoder.nextRow(grayRow)) {
      LOG_ERR("PNG", "Failed to decode scanline %u", y);
      success = false;
      break;
    }

    if (!needsScaling) {
      // Direct output (no scaling)
      memset(rowBuffer, 0, bytesPerRow);

      if (USE_8BIT_OUTPUT && !oneBit) {
        for (int x = 0; x < outWidth; x++) {
          rowBuffer[x] = adjustPixel(grayRow[x]);
        }
      } else if (oneBit) {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t bit =
              atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(grayRow[x], x) : quantize1bit(grayRow[x], x, y);
          const int byteIndex = x / 8;
          const int bitOffset = 7 - (x % 8);
          rowBuffer[byteIndex] |= (bit << bitOffset);
        }
        if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
      } else {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t gray = adjustPixel(grayRow[x]);
          uint8_t twoBit;
          if (atkinsonDitherer) {
            twoBit = atkinsonDitherer->processPixel(gray, x);
          } else if (fsDitherer) {
            twoBit = fsDitherer->processPixel(gray, x);
          } else {
            twoBit = quantize(gray, x, y);
          }
          const int byteIndex = (x * 2) / 8;
          const int bitOffset = 6 - ((x * 2) % 8);
          rowBuffer[byteIndex] |= (twoBit << bitOffset);
        }
        if (atkinsonDitherer)
          atkinsonDitherer->nextRow();
        else if (fsDitherer)
          fsDitherer->nextRow();
      }
      bmpOut.write(rowBuffer, bytesPerRow);
    } else {
      // Area-averaging scaling (same as JpegToBmpConverter)
      for (int outX = 0; outX < outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * scaleX_fp) >> 16;

        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < static_cast<int>(width); srcX++) {
          sum += grayRow[srcX];
          count++;
        }

        if (count == 0 && srcXStart < static_cast<int>(width)) {
          sum = grayRow[srcXStart];
          count = 1;
        }

        rowAccum[outX] += sum;
        rowCount[outX] += count;
      }

      // Check if we've crossed into the next output row(s)
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;

      // Output all rows whose boundaries we've crossed (handles both up and downscaling)
      while (srcY_fp >= nextOutY_srcStart && currentOutY < outHeight) {
        memset(rowBuffer, 0, bytesPerRow);

        if (USE_8BIT_OUTPUT && !oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            rowBuffer[x] = adjustPixel(gray);
          }
        } else if (oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            const uint8_t bit =
                atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x) : quantize1bit(gray, x, currentOutY);
            const int byteIndex = x / 8;
            const int bitOffset = 7 - (x % 8);
            rowBuffer[byteIndex] |= (bit << bitOffset);
          }
          if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
        } else {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = adjustPixel((rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0);
            uint8_t twoBit;
            if (atkinsonDitherer) {
              twoBit = atkinsonDitherer->processPixel(gray, x);
            } else if (fsDitherer) {
              twoBit = fsDitherer->processPixel(gray, x);
            } else {
              twoBit = quantize(gray, x, currentOutY);
            }
            const int byteIndex = (x * 2) / 8;
            const int bitOffset = 6 - ((x * 2) % 8);
            rowBuffer[byteIndex] |= (twoBit << bitOffset);
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }

        bmpOut.write(rowBuffer, bytesPerRow);
        currentOutY++;

        nextOutY_srcStart = static_cast<uint32_t>(currentOutY + 1) * scaleY_fp;

        // For upscaling: don't reset accumulators if next output row uses same source data
        if (srcY_fp >= nextOutY_srcStart) {
          continue;
        }
        memset(rowAccum, 0, outWidth * sizeof(uint32_t));
        memset(rowCount, 0, outWidth * sizeof(uint16_t));
      }
    }
  }

  // Clean up
  free(grayRow);
  delete[] rowAccum;
  delete[] rowCount;
  delete atkinsonDitherer;
  delete fsDitherer;
  delete atkinson1BitDitherer;
  free(rowBuffer);

  if (success) {
    LOG_DBG("PNG", "Successfully converted PNG to BMP");
  }
  return success;
}

// ============================================================================
// PngDecodeSession — sliced 1-bit PNG decode for use in loop()-driven contexts
// ============================================================================

bool PngDecodeSession::begin(FsFile& pngFile, FsFile& bmpFile, int targetWidth, int targetHeight) {
  bmpOut_ = &bmpFile;

  PngStreamDecoder::Info info;
  if (!decoder_.begin(pngFile, info)) {
    LOG_ERR("PNG", "Session begin: failed to start PNG decode");
    return false;
  }
  width_ = info.width;
  height_ = info.height;

  // Output dimensions (fit, no crop — same as pngFileTo1BitBmpStreamWithSize)
  outWidth_ = static_cast<int>(width_);
  outHeight_ = static_cast<int>(height_);
  scaleX_fp_ = 65536;
  scaleY_fp_ = 65536;
  needsScaling_ = false;

  if (targetWidth > 0 && targetHeight > 0 &&
      (outWidth_ != targetWidth || outHeight_ != targetHeight)) {
    const float sw = static_cast<float>(targetWidth) / width_;
    const float sh = static_cast<float>(targetHeight) / height_;
    const float scale = (sw < sh) ? sw : sh;
    outWidth_ = static_cast<int>(width_ * scale);
    outHeight_ = static_cast<int>(height_ * scale);
    if (outWidth_ < 1) outWidth_ = 1;
    if (outHeight_ < 1) outHeight_ = 1;
    scaleX_fp_ = (width_ << 16) / outWidth_;
    scaleY_fp_ = (height_ << 16) / outHeight_;
    needsScaling_ = true;
    LOG_DBG("PNG", "Session: scaling %ux%u -> %dx%d (target %dx%d)", width_, height_, outWidth_, outHeight_,
            targetWidth, targetHeight);
  }

  // 1-bit BMP header
  bytesPerRow_ = (outWidth_ + 31) / 32 * 4;
  writeBmpHeader1bit(bmpFile, outWidth_, outHeight_);

  // Allocate buffers
  grayRow_ = static_cast<uint8_t*>(malloc(width_));
  rowBuffer_ = static_cast<uint8_t*>(malloc(bytesPerRow_));
  if (!grayRow_ || !rowBuffer_) {
    LOG_ERR("PNG", "Session begin: row buffer alloc failed");
    cleanup();
    return false;
  }

  if (needsScaling_) {
    rowAccum_ = new (std::nothrow) uint32_t[outWidth_]();
    rowCount_ = new (std::nothrow) uint16_t[outWidth_]();
    if (!rowAccum_ || !rowCount_) {
      LOG_ERR("PNG", "Session begin: scaling buffer alloc failed");
      cleanup();
      return false;
    }
    nextOutY_srcStart_ = scaleY_fp_;
  }

  ditherer_ = new (std::nothrow) Atkinson1BitDitherer(outWidth_);
  if (!ditherer_) {
    LOG_ERR("PNG", "Session begin: ditherer alloc failed");
    cleanup();
    return false;
  }

  srcY_ = 0;
  currentOutY_ = 0;
  return true;
}

void PngDecodeSession::writeOutputRow(const uint8_t* gray) {
  memset(rowBuffer_, 0, bytesPerRow_);
  for (int x = 0; x < outWidth_; x++) {
    const uint8_t bit = ditherer_->processPixel(gray[x], x);
    rowBuffer_[x / 8] |= (bit << (7 - (x % 8)));
  }
  ditherer_->nextRow();
  bmpOut_->write(rowBuffer_, bytesPerRow_);
}

void PngDecodeSession::flushScaledRow() {
  memset(rowBuffer_, 0, bytesPerRow_);
  for (int x = 0; x < outWidth_; x++) {
    const uint8_t gray = (rowCount_[x] > 0) ? static_cast<uint8_t>(rowAccum_[x] / rowCount_[x]) : 0;
    const uint8_t bit = ditherer_->processPixel(gray, x);
    rowBuffer_[x / 8] |= (bit << (7 - (x % 8)));
  }
  ditherer_->nextRow();
  bmpOut_->write(rowBuffer_, bytesPerRow_);
  currentOutY_++;
}

PngDecodeSession::Status PngDecodeSession::continueRows(uint32_t maxSourceRows) {
  for (uint32_t processed = 0; processed < maxSourceRows && srcY_ < height_; processed++, srcY_++) {
    if (!decoder_.nextRow(grayRow_)) {
      LOG_ERR("PNG", "Session: decode failed at row %u", srcY_);
      return Status::Error;
    }

    if (!needsScaling_) {
      writeOutputRow(grayRow_);
    } else {
      // Accumulate source row into X-averaged output columns
      for (int outX = 0; outX < outWidth_; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * scaleX_fp_) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * scaleX_fp_) >> 16;
        int sum = 0, count = 0;
        for (int sx = srcXStart; sx < srcXEnd && sx < static_cast<int>(width_); sx++) {
          sum += grayRow_[sx];
          count++;
        }
        if (count == 0 && srcXStart < static_cast<int>(width_)) { sum = grayRow_[srcXStart]; count = 1; }
        rowAccum_[outX] += sum;
        rowCount_[outX] += count;
      }

      // Flush output rows whose Y boundary we've passed
      const uint32_t srcY_fp = (srcY_ + 1) << 16;
      while (srcY_fp >= nextOutY_srcStart_ && currentOutY_ < outHeight_) {
        flushScaledRow();
        nextOutY_srcStart_ = static_cast<uint32_t>(currentOutY_ + 1) * scaleY_fp_;
        if (srcY_fp >= nextOutY_srcStart_) continue;
        memset(rowAccum_, 0, outWidth_ * sizeof(uint32_t));
        memset(rowCount_, 0, outWidth_ * sizeof(uint16_t));
      }
    }
  }

  if (srcY_ < height_) return Status::Running;

  // Verify all output rows were written
  if (needsScaling_ && currentOutY_ < outHeight_) {
    LOG_ERR("PNG", "Session: incomplete — %d/%d output rows written", currentOutY_, outHeight_);
    return Status::Error;
  }
  LOG_DBG("PNG", "Session: decode complete (%ux%u -> %dx%d)", width_, height_, outWidth_, outHeight_);
  return Status::Done;
}

void PngDecodeSession::cleanup() {
  free(grayRow_);   grayRow_ = nullptr;
  free(rowBuffer_); rowBuffer_ = nullptr;
  delete[] rowAccum_; rowAccum_ = nullptr;
  delete[] rowCount_; rowCount_ = nullptr;
  delete ditherer_;   ditherer_ = nullptr;
  decoder_.end();
}

// ============================================================================

bool PngToBmpConverter::pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetWidth, targetHeight, false, crop);
}

bool PngToBmpConverter::pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
