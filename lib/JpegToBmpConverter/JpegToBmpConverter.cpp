#include "JpegToBmpConverter.h"

#include <CooperativeAbort.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ProgressiveJpegDc.h>
#include <tjpgd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "BitmapHelpers.h"
#include "BufferedPrint.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
// 8-bit output is now a per-call choice (see jpegFileToBmpStream's grayscale8Bit),
// not a build-wide constant.
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
// TJpgDec work area. With JD_FASTDECODE=2 the huffman LUTs (~6 KB for a colour JPEG)
// come from this pool on top of the ~3 KB base; 12 KB leaves headroom.
constexpr size_t TJPG_WORK_POOL_SIZE = 12 * 1024;
constexpr size_t MIN_FREE_HEAP = TJPG_WORK_POOL_SIZE + 28 * 1024;

struct BmpConvertCtx;  // forward decl for the session below

// TJpgDec session passed through jd->device to the I/O and output callbacks:
// the input callback reads from `file`; the output callback writes via `ctx`
// (set only once the context is built, just before jd_decomp).
struct BmpTjpgSession {
  FsFile* file;
  BmpConvertCtx* ctx;
};

// TJpgDec stream input: read ndata bytes into buff, or skip ndata bytes when buff is null.
size_t tjpgBmpInput(JDEC* jd, uint8_t* buff, size_t ndata) {
  FsFile* f = static_cast<BmpTjpgSession*>(jd->device)->file;
  if (!f) return 0;
  if (buff) {
    const int n = f->read(buff, ndata);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  if (!f->seek(f->position() + static_cast<uint32_t>(ndata))) return 0;
  return ndata;
}

// Context shared with the TJpgDec output callback via the session in jd->device
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  // Emit 8-bit grayscale instead of quantizing to 4 levels here. Mutually
  // exclusive with oneBit; see jpegFileToBmpStream()'s grayscale8Bit parameter.
  bool eightBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;

  // Center-crop window emitted to the BMP (crop mode overfills the target box in
  // one dimension by design; the excess must be trimmed HERE, not by the drawing
  // code — rescaling an already-dithered 1-bit image at draw time aliases the
  // dither pattern into a visible grid). The full outWidth row is still dithered
  // so error diffusion stays correct; only columns [outCropX, outCropX+finalW)
  // and rows [outCropY, outCropY+finalH) reach the file.
  int outCropX;
  int outCropY;
  int finalW;
  int finalH;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as TJpgDec output callbacks arrive for the same MCU row
  uint8_t* mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  uint32_t* rowAccum;
  uint32_t* rowCount;

  uint8_t* bmpRow;

  AtkinsonDitherer* atkinsonDitherer;
  FloydSteinbergDitherer* fsDitherer;
  Atkinson1BitDitherer* atkinson1BitDitherer;

  bool error;
};

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP.
// The whole row is dithered (diffusion state must see every pixel), but only the
// crop window columns are packed, and rows outside the vertical window are
// dithered-then-dropped (see BmpConvertCtx::outCropX).
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (ctx->eightBit && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const int ox = x - ctx->outCropX;
      if (ox >= 0 && ox < ctx->finalW) ctx->bmpRow[ox] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      const int ox = x - ctx->outCropX;
      if (ox >= 0 && ox < ctx->finalW) ctx->bmpRow[ox / 8] |= (bit << (7 - (ox % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      const int ox = x - ctx->outCropX;
      if (ox >= 0 && ox < ctx->finalW) ctx->bmpRow[(ox * 2) / 8] |= (twoBit << (6 - ((ox * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  if (outY >= ctx->outCropY && outY < ctx->outCropY + ctx->finalH) {
    ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY.
// Same crop-window rules as writeOutputRow.
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (ctx->eightBit && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const int ox = x - ctx->outCropX;
      if (ox >= 0 && ox < ctx->finalW) ctx->bmpRow[ox] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      const int ox = x - ctx->outCropX;
      if (ox >= 0 && ox < ctx->finalW) ctx->bmpRow[ox / 8] |= (bit << (7 - (ox % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      const int ox = x - ctx->outCropX;
      if (ox >= 0 && ox < ctx->finalW) ctx->bmpRow[(ox * 2) / 8] |= (twoBit << (6 - ((ox * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  if (ctx->currentOutY >= ctx->outCropY && ctx->currentOutY < ctx->outCropY + ctx->finalH) {
    ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
  }
  ctx->currentOutY++;
}

// TJpgDec output callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG). JRECT is inclusive and the
// grayscale bitmap is packed tightly at the block width. Accumulates columns into
// mcuBuf; once the last column arrives (completing the MCU row), applies scaling +
// dithering and writes packed BMP rows to bmpOut.
int tjpgBmpOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  auto* ctx = static_cast<BmpTjpgSession*>(jd->device)->ctx;
  if (!ctx || ctx->error) return 0;

  // Yield to pending button input: abort the decode so the main loop can service
  // the press. The partial BMP is discarded by the caller and regenerated later.
  // markAborted() distinguishes this deliberate bail from a plain decode failure.
  if (CooperativeAbort::shouldAbortLongTask()) {
    CooperativeAbort::markAborted();
    ctx->error = true;
    return 0;
  }

  const uint8_t* pixels = static_cast<const uint8_t*>(bitmap);
  const int validW = rect->right - rect->left + 1;
  const int blockH = rect->bottom - rect->top + 1;
  const int stride = validW;  // TJpgDec packs each block tightly at its width
  const int blockX = rect->left;
  const int blockY = rect->top;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row.
  // Clamp to MAX_MCU_HEIGHT so srcRow never indexes past the populated mcuBuf rows.
  const int safeEndRow = blockY + std::min(blockH, MAX_MCU_HEIGHT);
  for (int y = blockY; y < safeEndRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf + (y - blockY) * ctx->srcWidth;

    if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum, 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount, 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}

static bool progressiveBmpShouldAbort(void*) {
  if (!CooperativeAbort::shouldAbortLongTask()) return false;
  CooperativeAbort::markAborted();
  return true;
}

static bool progressiveBmpOutput(void* user, uint16_t y, const uint8_t* grayscale, uint16_t width) {
  auto* ctx = static_cast<BmpConvertCtx*>(user);
  if (!ctx || ctx->error || width != ctx->outWidth || y >= ctx->outHeight) return false;
  writeOutputRow(ctx, grayscale, y);
  return !ctx->error;
}

static bool decodeProgressiveJpeg(FsFile& jpegFile, Print& sink, int targetWidth, int targetHeight, bool oneBit,
                                  bool crop, const ProgressiveJpegDc::ImageInfo& image, bool eightBit) {
  // One row per write is one file call per row (see BufferedPrint); coalesce them.
  BufferedPrint bmpOut(sink);
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (image.width == 0 || image.height == 0 || image.width > MAX_IMAGE_WIDTH || image.height > MAX_IMAGE_HEIGHT) {
    return false;
  }

  int outWidth = image.width;
  int outHeight = image.height;
  if (targetWidth > 0 && targetHeight > 0) {
    const float scaleX = static_cast<float>(targetWidth) / image.width;
    const float scaleY = static_cast<float>(targetHeight) / image.height;
    const float scale = crop ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    outWidth = std::max(1, static_cast<int>(image.width * scale));
    outHeight = std::max(1, static_cast<int>(image.height * scale));
  }

  int outCropX = 0;
  int outCropY = 0;
  int finalWidth = outWidth;
  int finalHeight = outHeight;
  if (crop && targetWidth > 0 && targetHeight > 0) {
    if (outWidth > targetWidth) {
      outCropX = (outWidth - targetWidth) / 2;
      finalWidth = targetWidth;
    }
    if (outHeight > targetHeight) {
      outCropY = (outHeight - targetHeight) / 2;
      finalHeight = targetHeight;
    }
  }

  int bytesPerRow;
  if (eightBit && !oneBit) {
    bytesPerRow = writeGrayscaleBmpHeader(bmpOut, finalWidth, finalHeight, 8);
  } else if (oneBit) {
    bytesPerRow = writeGrayscaleBmpHeader(bmpOut, finalWidth, finalHeight, 1);
  } else {
    bytesPerRow = writeGrayscaleBmpHeader(bmpOut, finalWidth, finalHeight, 2);
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = outWidth;
  ctx.srcHeight = outHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.eightBit = eightBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.outCropX = outCropX;
  ctx.outCropY = outCropY;
  ctx.finalW = finalWidth;
  ctx.finalH = finalHeight;
  auto bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!bmpRow) return false;
  ctx.bmpRow = bmpRow.get();

  std::unique_ptr<Atkinson1BitDitherer> oneBitDitherer;
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  if (oneBit) {
    oneBitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    ctx.atkinson1BitDitherer = oneBitDitherer.get();
  } else if (!eightBit && USE_ATKINSON) {
    atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth);
    ctx.atkinsonDitherer = atkinsonDitherer.get();
  } else if (!eightBit && USE_FLOYD_STEINBERG) {
    fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
    ctx.fsDitherer = fsDitherer.get();
  }

  ProgressiveJpegDc::DecodeOptions options;
  options.outputWidth = outWidth;
  options.outputHeight = outHeight;
  options.shouldAbort = progressiveBmpShouldAbort;
  const auto result = ProgressiveJpegDc::decode(jpegFile, options, progressiveBmpOutput, &ctx);

  if (result != ProgressiveJpegDc::Result::Ok) {
    LOG_ERR("JPG", "Progressive JPEG preview failed: %s", ProgressiveJpegDc::resultName(result));
    return false;
  }
  // Fold the final flush into the result: a write that fails here would otherwise be reported
  // as a complete BMP, and the caller would cache a truncated file.
  if (!bmpOut.flushBuffer()) {
    LOG_ERR("JPG", "Failed to flush buffered BMP output");
    return false;
  }
  LOG_DBG("JPG", "Progressive JPEG preview decoded: %ux%u -> %dx%d", image.width, image.height, outWidth, outHeight);
  return true;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& sink, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop, bool eightBit) {
  // One row per write is one file call per row (see BufferedPrint); coalesce them.
  BufferedPrint bmpOut(sink);
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : (eightBit ? "8-bit" : "2-bit"),
          targetWidth, targetHeight);

  ProgressiveJpegDc::ImageInfo image;
  if (ProgressiveJpegDc::probe(jpegFile, image) == ProgressiveJpegDc::Result::Ok) {
    return decodeProgressiveJpeg(jpegFile, bmpOut, targetWidth, targetHeight, oneBit, crop, image, eightBit);
  }

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  jpegFile.seek(0);

  // new[] is max-aligned, satisfying TJpgDec's word-alignment requirement.
  std::unique_ptr<uint8_t[]> pool(new (std::nothrow) uint8_t[TJPG_WORK_POOL_SIZE]);
  if (!pool) {
    LOG_ERR("JPG", "Failed to allocate TJpgDec work pool (%u bytes)", static_cast<unsigned>(TJPG_WORK_POOL_SIZE));
    return false;
  }

  BmpTjpgSession session;
  session.file = &jpegFile;
  session.ctx = nullptr;  // set once the context is built, just before jd_decomp

  JDEC jdec;
  JRESULT jr = jd_prepare(&jdec, tjpgBmpInput, pool.get(), TJPG_WORK_POOL_SIZE, &session);
  if (jr != JDR_OK) {
    LOG_ERR("JPG", "TJpgDec prepare failed (jr=%d)", jr);
    return false;
  }

  const int srcWidth = jdec.width;
  const int srcHeight = jdec.height;
  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return false;
  }

  // Pick the largest DCT pre-scale that keeps both axes >= target so the fine scaler
  // always downscales (never upscales) on either axis. tjpgScale is the TJpgDec scale
  // exponent (0=1/1, 1=1/2, 2=1/4, 3=1/8). Using max(scaleX, scaleY) is safe for both
  // crop=true (uses max scale) and crop=false (uses min scale).
  uint8_t tjpgScale = 0;
  int jpegScaleDenom = 1;
  if (targetWidth > 0 && targetHeight > 0) {
    const float scaleX = static_cast<float>(targetWidth) / srcWidth;
    const float scaleY = static_cast<float>(targetHeight) / srcHeight;
    const float scaleMax = scaleX > scaleY ? scaleX : scaleY;
    if (scaleMax <= 0.125f) {
      tjpgScale = 3;
      jpegScaleDenom = 8;
    } else if (scaleMax <= 0.25f) {
      tjpgScale = 2;
      jpegScaleDenom = 4;
    } else if (scaleMax <= 0.5f) {
      tjpgScale = 1;
      jpegScaleDenom = 2;
    }
  }

  // TJpgDec's descaled output is floor(dim / 2^scale): every MCU side (8 or 16 px) is a
  // multiple of the scale denominator, so the per-MCU right/bottom shifts sum to exactly
  // the floor. These MUST match TJpgDec's actual output extent — the output callback only
  // flushes an MCU row once a block reaches `srcWidth`, so an over-estimate (e.g. ceil
  // division on an odd dimension like 333 -> 167 vs TJpgDec's 166) means the last column
  // never arrives and zero rows are ever written.
  const int effectiveSrcW = srcWidth / jpegScaleDenom;
  const int effectiveSrcH = srcHeight / jpegScaleDenom;

  if (jpegScaleDenom > 1) {
    LOG_DBG("JPG", "Using 1/%d DCT scale: %dx%d -> %dx%d", jpegScaleDenom, srcWidth, srcHeight, effectiveSrcW,
            effectiveSrcH);
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = effectiveSrcW;
  int outHeight = effectiveSrcH;
  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (effectiveSrcW != targetWidth || effectiveSrcH != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / effectiveSrcW;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / effectiveSrcH;
    float scale = 1.0f;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(effectiveSrcW * scale);
    outHeight = static_cast<int>(effectiveSrcH * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    scaleX_fp = (static_cast<uint32_t>(effectiveSrcW) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(effectiveSrcH) << 16) / outHeight;
    needsScaling = true;

    LOG_DBG("JPG", "Fine-scaling %dx%d -> %dx%d (target %dx%d)", effectiveSrcW, effectiveSrcH, outWidth, outHeight,
            targetWidth, targetHeight);
  }

  // crop mode scales by the LARGER fit factor, so the scaled image overfills the
  // target box in one dimension (e.g. a taller-than-box cover overfills vertically).
  // Emit only the centered target window: the BMP file must be EXACTLY the size its
  // callers asked for (and name it, e.g. thumb_340x540.bmp), because the home themes
  // draw these 1:1 — any dimension mismatch makes GfxRenderer::drawBitmap rescale an
  // already-dithered 1-bit image, which aliases the dither into a visible grid
  // (observed on-device: a 340x561 BMP in thumb_340x540.bmp, decimated to 96%).
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

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = effectiveSrcW;
  ctx.srcHeight = effectiveSrcH;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.eightBit = eightBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.outCropX = outCropX;
  ctx.outCropY = outCropY;
  ctx.finalW = finalW;
  ctx.finalH = finalH;
  ctx.error = false;

  // RAII guard: frees all heap resources on any return path (the TJpgDec work pool is
  // owned by the `pool` unique_ptr above and freed on scope exit).
  struct Cleanup {
    BmpConvertCtx& ctx;
    ~Cleanup() {
      delete[] ctx.rowAccum;
      delete[] ctx.rowCount;
      delete ctx.atkinsonDitherer;
      delete ctx.fsDitherer;
      delete ctx.atkinson1BitDitherer;
      free(ctx.mcuBuf);
      free(ctx.bmpRow);
    }
  } cleanup{ctx};

  ctx.mcuBuf = static_cast<uint8_t*>(malloc(MAX_MCU_HEIGHT * effectiveSrcW));
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "Failed to allocate MCU buffer (%d bytes)", MAX_MCU_HEIGHT * effectiveSrcW);
    return false;
  }
  memset(ctx.mcuBuf, 0, MAX_MCU_HEIGHT * effectiveSrcW);

  ctx.bmpRow = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "Failed to allocate BMP row buffer");
    return false;
  }

  if (needsScaling) {
    ctx.rowAccum = new (std::nothrow) uint32_t[outWidth]();
    ctx.rowCount = new (std::nothrow) uint32_t[outWidth]();
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "Failed to allocate scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(outWidth);
  } else if (!eightBit) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = new (std::nothrow) FloydSteinbergDitherer(outWidth);
    }
  }

  session.ctx = &ctx;
  jr = jd_decomp(&jdec, tjpgBmpOutput, tjpgScale);

  if (jr != JDR_OK || ctx.error) {
    LOG_ERR("JPG", "TJpgDec decode failed (jr=%d, ctxErr=%d)", jr, ctx.error ? 1 : 0);
    return false;
  }

  if (ctx.needsScaling && ctx.currentOutY < ctx.outHeight) {
    LOG_ERR("JPG", "JPEG decode incomplete: %d/%d output rows written", ctx.currentOutY, ctx.outHeight);
    return false;
  }

  if (!bmpOut.flushBuffer()) {
    LOG_ERR("JPG", "Failed to flush buffered BMP output");
    return false;
  }
  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to a full-size cover BMP (2-bit, or 8-bit when
// the caller asks for the extra tonal range).
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop, bool grayscale8Bit) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop, grayscale8Bit);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
