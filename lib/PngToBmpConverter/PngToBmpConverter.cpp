#include "PngToBmpConverter.h"

#include <CooperativeAbort.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PngStreamDecoder.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "BitmapHelpers.h"
#include "BufferedPrint.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Same as JpegToBmpConverter for consistency
// ============================================================================
constexpr bool USE_ATKINSON = true;
constexpr bool USE_FLOYD_STEINBERG = false;
constexpr bool USE_PRESCALE = true;
// ============================================================================

bool PngToBmpConverter::pngFileToBmpStreamInternal(FsFile& pngFile, Print& sink, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop, bool enforceSizeCap, bool eightBit) {
  // One row per write is one file call per row (see BufferedPrint); coalesce them.
  BufferedPrint bmpOut(sink);
  LOG_DBG("PNG", "Converting PNG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : (eightBit ? "8-bit" : "2-bit"),
          targetWidth, targetHeight);

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
  // 800*1200 = 960 kpx decodes in ~10 s on the ESP32-C3; cap there. Skipped for
  // stall-tolerant one-shot callers (enforceSizeCap=false) that have no sliced fallback.
  constexpr uint32_t MAX_PNG_PIXELS = 800u * 1200u;
  if (enforceSizeCap && width * height > MAX_PNG_PIXELS) {
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

  // crop mode overfills the target box in one dimension (scale = larger fit factor);
  // emit only the centered target window so the file is EXACTLY the requested size.
  // Same rationale as JpegToBmpConverter: home themes draw thumbs 1:1, and any size
  // mismatch makes drawBitmap rescale an already-dithered 1-bit image, aliasing the
  // dither pattern into a visible grid.
  int outCropX = 0;
  int outCropY = 0;
  int finalW = outWidth;
  int finalH = outHeight;
  if (crop && targetWidth > 0 && targetHeight > 0) {
    if (outWidth > targetWidth) {
      outCropX = (outWidth - targetWidth) / 2;
      finalW = targetWidth;
    }
    if (outHeight > targetHeight) {
      outCropY = (outHeight - targetHeight) / 2;
      finalH = targetHeight;
    }
  }

  // Write BMP header with the emitted (cropped) dimensions
  int bytesPerRow;
  if (eightBit && !oneBit) {
    bytesPerRow = writeGrayscaleBmpHeader(bmpOut, finalW, finalH, 8);
  } else if (oneBit) {
    bytesPerRow = writeGrayscaleBmpHeader(bmpOut, finalW, finalH, 1);
  } else {
    bytesPerRow = writeGrayscaleBmpHeader(bmpOut, finalW, finalH, 2);
  }

  auto rowBufferOwner = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!rowBufferOwner) {
    LOG_ERR("PNG", "Failed to allocate row buffer");
    return false;
  }
  uint8_t* rowBuffer = rowBufferOwner.get();

  std::unique_ptr<AtkinsonDitherer> atkinsonOwner;
  std::unique_ptr<FloydSteinbergDitherer> fsOwner;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitOwner;

  if (oneBit) {
    atkinson1BitOwner = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
  } else if (!eightBit) {
    if (USE_ATKINSON) {
      atkinsonOwner = makeUniqueNoThrow<AtkinsonDitherer>(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      fsOwner = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
    }
  }
  AtkinsonDitherer* atkinsonDitherer = atkinsonOwner.get();
  FloydSteinbergDitherer* fsDitherer = fsOwner.get();
  Atkinson1BitDitherer* atkinson1BitDitherer = atkinson1BitOwner.get();

  std::unique_ptr<uint32_t[]> rowAccumOwner;
  std::unique_ptr<uint16_t[]> rowCountOwner;
  int currentOutY = 0;
  uint32_t nextOutY_srcStart = 0;

  if (needsScaling) {
    rowAccumOwner = makeUniqueNoThrow<uint32_t[]>(outWidth);
    rowCountOwner = makeUniqueNoThrow<uint16_t[]>(outWidth);
    if (!rowAccumOwner || !rowCountOwner) {
      LOG_ERR("PNG", "Failed to allocate scaling buffers");
      return false;
    }
    nextOutY_srcStart = scaleY_fp;
  }
  uint32_t* rowAccum = rowAccumOwner.get();
  uint16_t* rowCount = rowCountOwner.get();

  auto grayRowOwner = makeUniqueNoThrow<uint8_t[]>(width);
  if (!grayRowOwner) {
    LOG_ERR("PNG", "Failed to allocate grayscale row buffer");
    return false;
  }
  uint8_t* grayRow = grayRowOwner.get();

  bool success = true;

  // Process each scanline
  for (uint32_t y = 0; y < height; y++) {
    // Yield to pending button input: abort so the main loop can service the press.
    // The partial BMP is discarded by the caller and regenerated later.
    // markAborted() distinguishes this deliberate bail from a plain decode failure.
    if (CooperativeAbort::shouldAbortLongTask()) {
      LOG_DBG("PNG", "Aborting decode at scanline %u for pending input", y);
      CooperativeAbort::markAborted();
      success = false;
      break;
    }

    if (!decoder.nextRow(grayRow)) {
      LOG_ERR("PNG", "Failed to decode scanline %u", y);
      success = false;
      break;
    }

    if (!needsScaling) {
      // Direct output (no scaling). The full row is dithered (diffusion state must
      // see every pixel); only crop-window columns are packed, and rows outside the
      // vertical window are dithered-then-dropped.
      memset(rowBuffer, 0, bytesPerRow);

      if (eightBit && !oneBit) {
        for (int x = 0; x < outWidth; x++) {
          const int ox = x - outCropX;
          if (ox >= 0 && ox < finalW) rowBuffer[ox] = adjustPixel(grayRow[x]);
        }
      } else if (oneBit) {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t bit =
              atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(grayRow[x], x) : quantize1bit(grayRow[x], x, y);
          const int ox = x - outCropX;
          if (ox >= 0 && ox < finalW) rowBuffer[ox / 8] |= (bit << (7 - (ox % 8)));
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
          const int ox = x - outCropX;
          if (ox >= 0 && ox < finalW) rowBuffer[(ox * 2) / 8] |= (twoBit << (6 - ((ox * 2) % 8)));
        }
        if (atkinsonDitherer)
          atkinsonDitherer->nextRow();
        else if (fsDitherer)
          fsDitherer->nextRow();
      }
      if (static_cast<int>(y) >= outCropY && static_cast<int>(y) < outCropY + finalH) {
        bmpOut.write(rowBuffer, bytesPerRow);
      }
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

        if (eightBit && !oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            const int ox = x - outCropX;
            if (ox >= 0 && ox < finalW) rowBuffer[ox] = adjustPixel(gray);
          }
        } else if (oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            const uint8_t bit =
                atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x) : quantize1bit(gray, x, currentOutY);
            const int ox = x - outCropX;
            if (ox >= 0 && ox < finalW) rowBuffer[ox / 8] |= (bit << (7 - (ox % 8)));
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
            const int ox = x - outCropX;
            if (ox >= 0 && ox < finalW) rowBuffer[(ox * 2) / 8] |= (twoBit << (6 - ((ox * 2) % 8)));
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }

        if (currentOutY >= outCropY && currentOutY < outCropY + finalH) {
          bmpOut.write(rowBuffer, bytesPerRow);
        }
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

  if (success && !bmpOut.flushBuffer()) {
    LOG_ERR("PNG", "Failed to flush buffered BMP output");
    success = false;
  }
  if (success) {
    LOG_DBG("PNG", "Successfully converted PNG to BMP");
  }
  return success;
}

// ============================================================================
// PngDecodeSession — sliced 1-bit PNG decode for use in loop()-driven contexts
// ============================================================================

bool PngDecodeSession::begin(FsFile& pngFile, FsFile& bmpFile, int targetWidth, int targetHeight, bool crop) {
  bmpOut_ = makeUniqueNoThrow<BufferedPrint>(bmpFile);
  if (!bmpOut_) {
    LOG_ERR("PNG", "Session begin: output buffer alloc failed");
    return false;
  }

  PngStreamDecoder::Info info;
  if (!decoder_.begin(pngFile, info)) {
    LOG_ERR("PNG", "Session begin: failed to start PNG decode");
    return false;
  }
  width_ = info.width;
  height_ = info.height;

  // Output dimensions — same policy as pngFileTo1BitBmpStreamWithSize. crop=true scales to the
  // LARGER fit factor (fill), so the binding dimension lands exactly on the target and the caller
  // draws the thumb 1:1 (no fractional rescale of a 1-bit dithered image → no moiré). crop=false
  // scales to the SMALLER factor (fit inside).
  outWidth_ = static_cast<int>(width_);
  outHeight_ = static_cast<int>(height_);
  scaleX_fp_ = 65536;
  scaleY_fp_ = 65536;
  needsScaling_ = false;

  if (targetWidth > 0 && targetHeight > 0 && (outWidth_ != targetWidth || outHeight_ != targetHeight)) {
    const float sw = static_cast<float>(targetWidth) / width_;
    const float sh = static_cast<float>(targetHeight) / height_;
    const float scale = crop ? ((sw > sh) ? sw : sh) : ((sw < sh) ? sw : sh);
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

  // Center-crop the overfill dimension so the BMP is exactly the target size
  // (see the crop note on begin() — drawn 1:1, never rescaled).
  outCropX_ = 0;
  outCropY_ = 0;
  finalW_ = outWidth_;
  finalH_ = outHeight_;
  if (crop && targetWidth > 0 && targetHeight > 0) {
    if (outWidth_ > targetWidth) {
      outCropX_ = (outWidth_ - targetWidth) / 2;
      finalW_ = targetWidth;
    }
    if (outHeight_ > targetHeight) {
      outCropY_ = (outHeight_ - targetHeight) / 2;
      finalH_ = targetHeight;
    }
  }

  // 1-bit BMP header
  bytesPerRow_ = writeGrayscaleBmpHeader(*bmpOut_, finalW_, finalH_, 1);

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

// Full row is dithered (diffusion state must see every pixel); only crop-window
// columns are packed, and rows outside the vertical window are dropped after
// dithering. In the non-scaled case the output row index equals srcY_.
void PngDecodeSession::writeOutputRow(const uint8_t* gray) {
  memset(rowBuffer_, 0, bytesPerRow_);
  for (int x = 0; x < outWidth_; x++) {
    const uint8_t bit = ditherer_->processPixel(gray[x], x);
    const int ox = x - outCropX_;
    if (ox >= 0 && ox < finalW_) rowBuffer_[ox / 8] |= (bit << (7 - (ox % 8)));
  }
  ditherer_->nextRow();
  const int outY = static_cast<int>(srcY_);
  if (outY >= outCropY_ && outY < outCropY_ + finalH_) {
    bmpOut_->write(rowBuffer_, bytesPerRow_);
  }
}

void PngDecodeSession::flushScaledRow() {
  memset(rowBuffer_, 0, bytesPerRow_);
  for (int x = 0; x < outWidth_; x++) {
    const uint8_t gray = (rowCount_[x] > 0) ? static_cast<uint8_t>(rowAccum_[x] / rowCount_[x]) : 0;
    const uint8_t bit = ditherer_->processPixel(gray, x);
    const int ox = x - outCropX_;
    if (ox >= 0 && ox < finalW_) rowBuffer_[ox / 8] |= (bit << (7 - (ox % 8)));
  }
  ditherer_->nextRow();
  if (currentOutY_ >= outCropY_ && currentOutY_ < outCropY_ + finalH_) {
    bmpOut_->write(rowBuffer_, bytesPerRow_);
  }
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
        if (count == 0 && srcXStart < static_cast<int>(width_)) {
          sum = grayRow_[srcXStart];
          count = 1;
        }
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
  // Last chance to surface a write failure: after this the caller treats the file as complete.
  if (!bmpOut_->flushBuffer()) {
    LOG_ERR("PNG", "Session: failed to flush buffered BMP output");
    return Status::Error;
  }
  LOG_DBG("PNG", "Session: decode complete (%ux%u -> %dx%d)", width_, height_, outWidth_, outHeight_);
  return Status::Done;
}

void PngDecodeSession::cleanup() {
  free(grayRow_);
  grayRow_ = nullptr;
  free(rowBuffer_);
  rowBuffer_ = nullptr;
  delete[] rowAccum_;
  rowAccum_ = nullptr;
  delete[] rowCount_;
  rowCount_ = nullptr;
  delete ditherer_;
  ditherer_ = nullptr;
  bmpOut_.reset();  // flushes whatever a bailed-out session had pending, then drops the buffer
  decoder_.end();
}

// ============================================================================

bool PngToBmpConverter::pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop, bool grayscale8Bit) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  // Full-screen cover render for one-shot screens (sleep / finished-book / OPDS): stall-tolerant
  // and with no sliced fallback, so bypass the thumbnail size cap and decode large covers directly.
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetWidth, targetHeight, /*oneBit=*/false, crop,
                                    /*enforceSizeCap=*/false, grayscale8Bit);
}

bool PngToBmpConverter::pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                   int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
