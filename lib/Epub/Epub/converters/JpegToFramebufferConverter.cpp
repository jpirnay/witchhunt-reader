#include "JpegToFramebufferConverter.h"

#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>
#include <tjpgd.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
#include <esp_heap_caps.h>
#endif

namespace {

#ifdef ENABLE_BOOT_HEAP_DIAGNOSTICS
// Heap-corruption tripwire for the cover-JPEG decode path (the buildSection warm pass
// where the multi_heap poisoning crash surfaces). Walks the whole heap and names the
// checkpoint; the symbolized crash shows the poison is only DETECTED at cache.abort()'s
// SD-buffer free, so these checks before/after jd_decomp() convict the writer:
//   jpg_decode_entry corrupt   -> corruption is UPSTREAM of the JPEG path (CSS/build)
//   jpg_before_decode corrupt  -> the ditherer/cache-stream setup allocs trashed it
//   jpg_after_decode corrupt   -> the decode itself (band-cache / pixel writes) is the writer
// Same caveat as EpubReaderActivity::checkHeapIntegrity: only for diagnostic builds.
void jpgCheckHeap(const char* checkpoint) {
  if (heap_caps_check_integrity_all(true)) return;
  LOG_ERR("JPG", "HEAP CORRUPT at checkpoint: %s", checkpoint);
}
#else
inline void jpgCheckHeap(const char*) {}
#endif

// Context struct passed through the TJpgDec callbacks (via the TJpgSession in
// jd->device) to avoid global mutable state. The output callback uses it to resample
// and dither each block; the input callback uses the session's FsFile* to read.
struct JpegContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};
  ImageDitherMode effectiveDitherMode{ImageDitherMode::Bayer};

  // Source dimensions after the built-in DCT scaling
  int scaledSrcWidth{0};
  int scaledSrcHeight{0};

  // Final output dimensions
  int dstWidth{0};
  int dstHeight{0};

  // Fine scale in 16.16 fixed-point (ESP32-C3 has no FPU).
  // X and Y use separate scale factors because dstWidth/dstHeight may differ from
  // scaledSrcWidth/scaledSrcHeight in aspect ratio (integer rounding of displayHeight),
  // so a single X-derived factor would map Y rows incorrectly and crop visible content.
  int32_t fineScaleFPX{1 << 16};  // src -> dst mapping (X axis)
  int32_t invScaleFPX{1 << 16};   // dst -> src mapping (X axis)
  int32_t fineScaleFPY{1 << 16};  // src -> dst mapping (Y axis)
  int32_t invScaleFPY{1 << 16};   // dst -> src mapping (Y axis)

  PixelCache cache;
  bool caching{false};

  // See PngContext for the rationale: monochromeOutput requests a 1-bit Atkinson dither
  // emitting only 0/3 so the BW DirectPixelWriter (`pixelValue < 3` rule) maps cleanly.
  int oneBitDitherRow{-1};
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  int currentDitherRow{-1};
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<DiffusedBayerDitherer> diffusedBayerDitherer;
#endif
};

// Advance the 1-bit Atkinson ditherer to the requested destination row.
// Handles non-monotonic row walks (block-based JPEG decode) by reset+replay.
void prepareOneBitDitherRow(JpegContext& ctx, int dstY) {
  if (!ctx.atkinson1BitDitherer) return;

  if (ctx.oneBitDitherRow == -1 || dstY < ctx.oneBitDitherRow) {
    ctx.atkinson1BitDitherer->reset();
    ctx.oneBitDitherRow = dstY;
    return;
  }

  while (ctx.oneBitDitherRow < dstY) {
    ctx.atkinson1BitDitherer->nextRow();
    ctx.oneBitDitherRow++;
  }
}

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
void prepareDitherRow(JpegContext& ctx, int dstY) {
  if (!ctx.config || !ctx.config->useDithering) return;

  if (ctx.currentDitherRow == -1 || dstY < ctx.currentDitherRow) {
    if (ctx.atkinsonDitherer) ctx.atkinsonDitherer->reset();
    if (ctx.diffusedBayerDitherer) ctx.diffusedBayerDitherer->reset();
    ctx.currentDitherRow = dstY;
    return;
  }

  while (ctx.currentDitherRow < dstY) {
    if (ctx.atkinsonDitherer) ctx.atkinsonDitherer->nextRow();
    if (ctx.diffusedBayerDitherer) ctx.diffusedBayerDitherer->nextRow();
    ctx.currentDitherRow++;
  }
}

uint8_t ditherGray(JpegContext& ctx, uint8_t gray, int localX, int outX, int outY) {
  if (ctx.atkinson1BitDitherer) {
    return ctx.atkinson1BitDitherer->processPixel(gray, localX) ? 3 : 0;
  }

  if (!ctx.config || !ctx.config->useDithering) {
    return quantizeGray4Level(gray);
  }

  switch (ctx.effectiveDitherMode) {
    case ImageDitherMode::Atkinson:
      if (ctx.atkinsonDitherer) {
        return ctx.atkinsonDitherer->processPixel(gray, localX);
      }
      break;
    case ImageDitherMode::DiffusedBayer:
      if (ctx.diffusedBayerDitherer) {
        return ctx.diffusedBayerDitherer->processPixel(gray, localX, outX, outY);
      }
      break;
    case ImageDitherMode::Bayer:
    case ImageDitherMode::COUNT:
    default:
      break;
  }

  return applyBayerDither4Level(gray, outX, outY);
}
#else
uint8_t ditherGray(JpegContext& ctx, uint8_t gray, int localX, int outX, int outY) {
  if (ctx.atkinson1BitDitherer) {
    return ctx.atkinson1BitDitherer->processPixel(gray, localX) ? 3 : 0;
  }
  (void)localX;
  return applyBayerDither4Level(gray, outX, outY);
}
#endif

// TJpgDec session passed through jd->device to the I/O and output callbacks, giving
// access to the open source file and the shared decode context (no global state).
struct TJpgSession {
  FsFile* file{nullptr};
  JpegContext* ctx{nullptr};
};

// TJpgDec stream input. When buff is non-null, read ndata bytes into it; when null,
// remove (skip) ndata bytes from the stream. Returns the number of bytes consumed.
size_t tjpgInput(JDEC* jd, uint8_t* buff, size_t ndata) {
  FsFile* f = static_cast<TJpgSession*>(jd->device)->file;
  if (!f) return 0;
  if (buff) {
    const int n = f->read(buff, ndata);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  if (!f->seek(f->position() + static_cast<uint32_t>(ndata))) return 0;
  return ndata;
}

// TJpgDec needs <=3092 bytes of work area (header-table dependent); round up to a
// word-aligned 4 KB pool. This is the whole point of the migration — vs JPEGDEC's
// ~20 KB struct + internal buffers.
constexpr size_t TJPG_WORK_POOL_SIZE = 4 * 1024;
// Minimum free heap to attempt a decode: the work pool plus headroom for the
// streaming cache band and the ditherer rows allocated further below.
constexpr size_t MIN_FREE_HEAP_FOR_JPEG = TJPG_WORK_POOL_SIZE + 16 * 1024;

// Optional memory-behavior knobs for embedded targets.
#ifndef JPEG_ENABLE_FIRST_RENDER_NO_CACHE
#define JPEG_ENABLE_FIRST_RENDER_NO_CACHE 1
#endif

#ifndef JPEG_CACHE_MIN_FREE_HEAP_MARGIN
#define JPEG_CACHE_MIN_FREE_HEAP_MARGIN (24 * 1024)
#endif

#ifndef JPEG_DITHER_LOW_MEM_MIN_FREE_HEAP
#define JPEG_DITHER_LOW_MEM_MIN_FREE_HEAP (MIN_FREE_HEAP_FOR_JPEG + 8 * 1024)
#endif

size_t jpegCacheBytes(int width, int height) {
  return static_cast<size_t>((width + 3) / 4) * static_cast<size_t>(height);
}

bool shouldEnableJpegCache(const RenderConfig& config, int width, int height) {
  if (config.cachePath.empty()) return false;

#if JPEG_ENABLE_FIRST_RENDER_NO_CACHE
  if (!Storage.exists(config.cachePath.c_str())) {
    LOG_DBG("JPG", "No existing JPEG cache file on first render; enabling cache write: %s", config.cachePath.c_str());
  }
#endif

  const size_t cacheBytes = jpegCacheBytes(width, height);
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t minFreeForCaching = MIN_FREE_HEAP_FOR_JPEG + JPEG_CACHE_MIN_FREE_HEAP_MARGIN;

  if (freeHeap < minFreeForCaching) {
    LOG_DBG("JPG", "Skipping cache: free heap %u < %u (cache %u bytes)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(minFreeForCaching), static_cast<unsigned>(cacheBytes));
    return false;
  }

  // Don't pre-check maxAlloc: the TJpgDec work pool is already allocated here so
  // maxAlloc already reflects that. Let allocate() attempt malloc and fail gracefully
  // rather than refusing on a conservative margin that double-counts live allocations.
  return true;
}

bool shouldForceBayerDither(const RenderConfig& config) {
  if (!config.useDithering) return false;
  if (config.ditherMode == ImageDitherMode::Bayer) return false;

  // Use getFreeHeap() only — getMaxAllocHeap() walks the TLSF free-block chain
  // and crashes if the heap is corrupt (possible after failed image decodes under
  // pressure). The free-heap threshold is conservative enough as a sole guard here.
  const size_t freeHeap = ESP.getFreeHeap();
  return freeHeap < JPEG_DITHER_LOW_MEM_MIN_FREE_HEAP;
}

// Classify a SOF marker byte into the coding mode used for engine selection.
// Only SOF0 is true baseline (the sole mode TJpgDec accepts); SOF2 is progressive;
// everything else (extended-sequential SOF1, arithmetic, lossless, …) is Other.
// Only Baseline can be decoded; the rest fall back to a placeholder.
JpegToFramebufferConverter::JpegMode classifyJpegMode(uint8_t sofMarker) {
  using JpegMode = JpegToFramebufferConverter::JpegMode;
  if (sofMarker == 0xC0) return JpegMode::Baseline;
  if (sofMarker == 0xC2) return JpegMode::Progressive;
  return JpegMode::Other;
}

bool readJpegDimensionsFromHeader(const std::string& imagePath, ImageDimensions& out,
                                  JpegToFramebufferConverter::JpegMode* outMode = nullptr) {
  FsFile f;
  if (!Storage.openFileForRead("JPG", imagePath, f)) {
    LOG_ERR("JPG", "Failed to open file for dimensions: %s", imagePath.c_str());
    return false;
  }

  auto readByte = [&f](uint8_t& b) -> bool { return f.read(&b, 1) == 1; };
  auto readU16BE = [&f](uint16_t& v) -> bool {
    uint8_t b[2];
    if (f.read(b, 2) != 2) return false;
    v = static_cast<uint16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
    return true;
  };

  uint8_t b0 = 0;
  uint8_t b1 = 0;
  if (!readByte(b0) || !readByte(b1) || b0 != 0xFF || b1 != 0xD8) {
    f.close();
    LOG_ERR("JPG", "Not a JPEG file: %s", imagePath.c_str());
    return false;
  }

  while (f.available()) {
    uint8_t prefix = 0;
    if (!readByte(prefix)) break;
    if (prefix != 0xFF) continue;

    uint8_t marker = 0;
    do {
      if (!readByte(marker)) {
        f.close();
        return false;
      }
    } while (marker == 0xFF);

    if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }

    uint16_t segLen = 0;
    if (!readU16BE(segLen) || segLen < 2) {
      f.close();
      return false;
    }

    const bool isSof = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                       (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
    if (isSof) {
      uint8_t sof[5];
      if (segLen < 7 || f.read(sof, sizeof(sof)) != static_cast<int>(sizeof(sof))) {
        f.close();
        return false;
      }
      uint16_t height = static_cast<uint16_t>((static_cast<uint16_t>(sof[1]) << 8) | sof[2]);
      uint16_t width = static_cast<uint16_t>((static_cast<uint16_t>(sof[3]) << 8) | sof[4]);
      f.close();
      if (width == 0 || height == 0) {
        LOG_ERR("JPG", "Invalid JPEG dimensions %ux%u: %s", width, height, imagePath.c_str());
        return false;
      }

      if (width > static_cast<uint16_t>(std::numeric_limits<int16_t>::max()) ||
          height > static_cast<uint16_t>(std::numeric_limits<int16_t>::max())) {
        LOG_ERR("JPG", "JPEG dimensions out of supported range %ux%u: %s", width, height, imagePath.c_str());
        return false;
      }

      out.width = static_cast<int16_t>(width);
      out.height = static_cast<int16_t>(height);
      if (outMode) *outMode = classifyJpegMode(marker);
      return true;
    }

    const int32_t skip = static_cast<int32_t>(segLen) - 2;
    if (!f.seek(f.position() + skip)) {
      f.close();
      return false;
    }
  }

  f.close();
  LOG_ERR("JPG", "No SOF marker found for dimensions: %s", imagePath.c_str());
  return false;
}

// Choose the coarse DCT downscale factor. Returns the scale denominator (1, 2, 4 or 8)
// and sets tjpgScale to the matching TJpgDec scale exponent (0=1/1, 1=1/2, 2=1/4, 3=1/8).
int chooseJpegScale(float targetScale, uint8_t& tjpgScale) {
  if (targetScale <= 0.125f) {
    tjpgScale = 3;
    return 8;
  }
  if (targetScale <= 0.25f) {
    tjpgScale = 2;
    return 4;
  }
  if (targetScale <= 0.5f) {
    tjpgScale = 1;
    return 2;
  }
  tjpgScale = 0;
  return 1;
}

// Fixed-point 16.16 arithmetic avoids software float emulation on ESP32-C3 (no FPU).
constexpr int FP_SHIFT = 16;
constexpr int32_t FP_ONE = 1 << FP_SHIFT;
constexpr int32_t FP_MASK = FP_ONE - 1;

// Emit one decoded grayscale block into the framebuffer (and the streaming cache),
// applying fine resampling and the active ditherer. Engine-neutral: it takes a raw
// 8-bit grayscale block — densely packed at `stride`, with `validW` valid columns and
// `blockH` rows — at its scaled-source-space origin (blockX, blockY). Consumed by the
// TJpgDec output callback below. Returns 1 to continue decoding (0 would abort),
// matching the engine's callback contract.
int emitGrayBlock(JpegContext& ctxRef, const uint8_t* pixels, int blockX, int blockY, int validW, int blockH,
                  int stride) {
  JpegContext* ctx = &ctxRef;

  // Feed the interrupt WDT every block — large JPEGs can take many seconds.
  esp_task_wdt_reset();

  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  bool caching = ctx->caching;
  const int32_t fineScaleFPX = ctx->fineScaleFPX;
  const int32_t invScaleFPX = ctx->invScaleFPX;
  const int32_t fineScaleFPY = ctx->fineScaleFPY;
  const int32_t invScaleFPY = ctx->invScaleFPY;
  GfxRenderer& renderer = *ctx->renderer;
  const int cfgX = ctx->config->x;
  const int cfgY = ctx->config->y;

  // Determine destination pixel range covered by this source block
  const int srcYEnd = blockY + blockH;
  const int srcXEnd = blockX + validW;

  int dstYStart = (int)((int64_t)blockY * fineScaleFPY >> FP_SHIFT);
  int dstYEnd = (srcYEnd >= ctx->scaledSrcHeight) ? ctx->dstHeight : (int)((int64_t)srcYEnd * fineScaleFPY >> FP_SHIFT);
  int dstXStart = (int)((int64_t)blockX * fineScaleFPX >> FP_SHIFT);
  int dstXEnd = (srcXEnd >= ctx->scaledSrcWidth) ? ctx->dstWidth : (int)((int64_t)srcXEnd * fineScaleFPX >> FP_SHIFT);

  // Pre-clamp destination ranges to screen bounds (eliminates per-pixel screen checks)
  int clampYMax = ctx->dstHeight;
  if (ctx->screenHeight - cfgY < clampYMax) clampYMax = ctx->screenHeight - cfgY;
  if (dstYStart < -cfgY) dstYStart = -cfgY;
  if (dstYEnd > clampYMax) dstYEnd = clampYMax;

  int clampXMax = ctx->dstWidth;
  if (ctx->screenWidth - cfgX < clampXMax) clampXMax = ctx->screenWidth - cfgX;
  if (dstXStart < -cfgX) dstXStart = -cfgX;
  if (dstXEnd > clampXMax) dstXEnd = clampXMax;

  if (dstYStart >= dstYEnd || dstXStart >= dstXEnd) return 1;

  // Pre-compute orientation and render-mode state once per callback invocation
  DirectPixelWriter pw;
  pw.init(renderer);

  // The cache streams to disk one MCU-row band at a time. Flushing rows below
  // this block (raster order guarantees they are final) repositions the band;
  // the band-relative origin/extent passed to init() then map screen rows to
  // the small streaming buffer rather than a full-image one. If a flush write
  // fails, stop caching for the rest of this decode (and let finalize() drop
  // the partial file) rather than writing past the band buffer.
  //
  // Ported from upstream commit d9bcef7a (crosspoint-reader#2230).
  DirectCacheWriter cw;
  if (caching) {
    if (!ctx->cache.advanceTo(dstYStart)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.originX, ctx->config->y + ctx->cache.bandStart,
              ctx->cache.width, ctx->cache.bandRows);
    }
  }

  // === 1:1 fast path: no scaling math ===
  if (fineScaleFPX == FP_ONE && fineScaleFPY == FP_ONE) {
    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      prepareOneBitDitherRow(*ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
      prepareDitherRow(*ctx, dstY);
#endif
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY);
      const uint8_t* row = &pixels[(dstY - blockY) * stride];
      for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        uint8_t gray = row[dstX - blockX];
        uint8_t dithered = ditherGray(*ctx, gray, dstX, outX, outY);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Bilinear interpolation (upscale: fineScale > 1.0) ===
  // Smooths block boundaries that would otherwise create visible banding
  // on progressive JPEG DC-only decode (1/8 resolution upscaled to target).
  if (fineScaleFPX > FP_ONE && fineScaleFPY > FP_ONE) {
    // Pre-compute safe X range where lx0 and lx0+1 are both in [0, validW-1].
    // Only the left/right edge pixels (typically 0-2 and 1-8 respectively) need clamping.
    int safeXStart = (int)(((int64_t)blockX * fineScaleFPX + FP_MASK) >> FP_SHIFT);
    int safeXEnd = (int)((int64_t)(blockX + validW - 1) * fineScaleFPX >> FP_SHIFT);
    if (safeXStart < dstXStart) safeXStart = dstXStart;
    if (safeXEnd > dstXEnd) safeXEnd = dstXEnd;
    if (safeXStart > safeXEnd) safeXEnd = safeXStart;

    for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
      const int outY = cfgY + dstY;
      prepareOneBitDitherRow(*ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
      prepareDitherRow(*ctx, dstY);
#endif
      pw.beginRow(outY);
      if (caching) cw.beginRow(outY);
      const int32_t srcFyFP = dstY * invScaleFPY;
      const int32_t fy = srcFyFP & FP_MASK;
      const int32_t fyInv = FP_ONE - fy;
      int ly0 = (srcFyFP >> FP_SHIFT) - blockY;
      int ly1 = ly0 + 1;
      if (ly0 < 0) ly0 = 0;
      if (ly0 >= blockH) ly0 = blockH - 1;
      if (ly1 >= blockH) ly1 = blockH - 1;

      const uint8_t* row0 = &pixels[ly0 * stride];
      const uint8_t* row1 = &pixels[ly1 * stride];

      // Left edge (with X boundary clamping)
      for (int dstX = dstXStart; dstX < safeXStart; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 < 0) lx0 = 0;
        if (lx1 < 0) lx1 = 0;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered = ditherGray(*ctx, gray, dstX, outX, outY);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }

      // Interior (no X boundary checks — lx0 and lx0+1 guaranteed in bounds)
      for (int dstX = safeXStart; dstX < safeXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        const int lx0 = (srcFxFP >> FP_SHIFT) - blockX;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx0 + 1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx0 + 1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered = ditherGray(*ctx, gray, dstX, outX, outY);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }

      // Right edge (with X boundary clamping)
      for (int dstX = safeXEnd; dstX < dstXEnd; dstX++) {
        const int outX = cfgX + dstX;
        const int32_t srcFxFP = dstX * invScaleFPX;
        const int32_t fx = srcFxFP & FP_MASK;
        const int32_t fxInv = FP_ONE - fx;
        int lx0 = (srcFxFP >> FP_SHIFT) - blockX;
        int lx1 = lx0 + 1;
        if (lx0 >= validW) lx0 = validW - 1;
        if (lx1 >= validW) lx1 = validW - 1;

        int top = ((int)row0[lx0] * fxInv + (int)row0[lx1] * fx) >> FP_SHIFT;
        int bot = ((int)row1[lx0] * fxInv + (int)row1[lx1] * fx) >> FP_SHIFT;
        uint8_t gray = (uint8_t)((top * fyInv + bot * fy) >> FP_SHIFT);

        uint8_t dithered = ditherGray(*ctx, gray, dstX, outX, outY);
        pw.writePixel(outX, dithered);
        if (caching) cw.writePixel(outX, dithered);
      }
    }
    return 1;
  }

  // === Nearest-neighbor (downscale: fineScale < 1.0) ===
  for (int dstY = dstYStart; dstY < dstYEnd; dstY++) {
    const int outY = cfgY + dstY;
    prepareOneBitDitherRow(*ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    prepareDitherRow(*ctx, dstY);
#endif
    pw.beginRow(outY);
    if (caching) cw.beginRow(outY);
    const int32_t srcFyFP = dstY * invScaleFPY;
    int ly = (srcFyFP >> FP_SHIFT) - blockY;
    if (ly < 0) ly = 0;
    if (ly >= blockH) ly = blockH - 1;
    const uint8_t* row = &pixels[ly * stride];

    for (int dstX = dstXStart; dstX < dstXEnd; dstX++) {
      const int outX = cfgX + dstX;
      const int32_t srcFxFP = dstX * invScaleFPX;
      int lx = (srcFxFP >> FP_SHIFT) - blockX;
      if (lx < 0) lx = 0;
      if (lx >= validW) lx = validW - 1;
      uint8_t gray = row[lx];

      uint8_t dithered = ditherGray(*ctx, gray, dstX, outX, outY);
      pw.writePixel(outX, dithered);
      if (caching) cw.writePixel(outX, dithered);
    }
  }

  return 1;
}

// TJpgDec output callback: forward one decoded grayscale MCU block to emitGrayBlock.
// JRECT is inclusive; with JD_FORMAT=2 the bitmap is 8-bit gray packed tightly at the
// block width, and MCUs arrive in raster top-to-bottom order (PixelCache contract).
int tjpgOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  JpegContext* ctx = static_cast<TJpgSession*>(jd->device)->ctx;
  if (!ctx) return 0;
  const int validW = rect->right - rect->left + 1;
  const int blockH = rect->bottom - rect->top + 1;
  return emitGrayBlock(*ctx, static_cast<const uint8_t*>(bitmap), rect->left, rect->top, validW, blockH, validW);
}

// Draw a simple bordered placeholder where an undecodable (non-baseline) JPEG would
// have gone, so the layout shows a framed gap rather than a silent blank.
void drawUnsupportedPlaceholder(GfxRenderer& renderer, const RenderConfig& config) {
  const int w = config.maxWidth;
  const int h = config.maxHeight;
  if (w <= 0 || h <= 0) return;
  if (config.x < 0 || config.y < 0 || config.x + w > renderer.getScreenWidth() ||
      config.y + h > renderer.getScreenHeight()) {
    return;
  }
  constexpr int BORDER = 1;
  renderer.drawRect(config.x, config.y, w, h, BORDER, true);
}

}  // namespace

bool JpegToFramebufferConverter::getDimensionsFromBuffer(const uint8_t* buf, const size_t len, ImageDimensions& out,
                                                         JpegMode* outMode) {
  if (!buf || len < 4) return false;
  if (buf[0] != 0xFF || buf[1] != 0xD8) return false;

  size_t pos = 2;
  while (pos + 3 < len) {
    if (buf[pos] != 0xFF) {
      pos++;
      continue;
    }
    pos++;
    uint8_t marker = buf[pos++];
    while (marker == 0xFF && pos < len) marker = buf[pos++];
    if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (pos + 1 >= len) break;
    const uint16_t segLen = (static_cast<uint16_t>(buf[pos]) << 8) | buf[pos + 1];
    if (segLen < 2) break;
    const bool isSof = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                       (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
    if (isSof) {
      if (pos + 6 >= len) return false;
      const uint16_t h = (static_cast<uint16_t>(buf[pos + 3]) << 8) | buf[pos + 4];
      const uint16_t w = (static_cast<uint16_t>(buf[pos + 5]) << 8) | buf[pos + 6];
      if (w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF) return false;
      out.width = static_cast<int16_t>(w);
      out.height = static_cast<int16_t>(h);
      if (outMode) *outMode = classifyJpegMode(marker);
      return true;
    }
    pos += segLen;
  }
  return false;
}

bool JpegToFramebufferConverter::getModeFromHeader(const std::string& imagePath, JpegMode& out) {
  ImageDimensions dims{};
  return readJpegDimensionsFromHeader(imagePath, dims, &out);
}

bool JpegToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  if (!readJpegDimensionsFromHeader(imagePath, out)) {
    return false;
  }
  LOG_DBG("JPG", "Image dimensions: %dx%d", out.width, out.height);
  return true;
}

bool JpegToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                     const RenderConfig& config) {
  LOG_DBG("JPG", "Decoding JPEG: %s", imagePath.c_str());
  jpgCheckHeap("jpg_decode_entry");

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_JPEG) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_JPEG);
    return false;
  }

  // Validate JPEG markers before handing off to the decoder. A truncated file left
  // by a prior crashed extraction session will have valid SOF headers but no EOI,
  // which can fault the decoder on the garbage entropy data. Delete and bail so
  // the next ensureExtracted() re-extracts the file cleanly.
  {
    FsFile f;
    if (Storage.openFileForRead("JPG", imagePath, f)) {
      const size_t fileSize = f.size();
      uint8_t magic[2] = {0, 0};
      bool valid = false;
      if (fileSize >= 4 && f.read(magic, 2) == 2 && magic[0] == 0xFF && magic[1] == 0xD8) {
        if (f.seek(fileSize - 2)) {
          uint8_t tail[2] = {0, 0};
          if (f.read(tail, 2) == 2 && tail[0] == 0xFF && tail[1] == 0xD9) {
            valid = true;
          }
        }
      }
      f.close();
      if (!valid) {
        LOG_ERR("JPG", "JPEG integrity check failed (truncated/corrupt): %s — deleting for re-extraction",
                imagePath.c_str());
        Storage.remove(imagePath.c_str());
        return false;
      }
    }
  }

  JpegContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.effectiveDitherMode = config.ditherMode;

  // Engine selection: TJpgDec decodes baseline (SOF0) only. Progressive and other
  // modes have no low-RAM full-quality decode path on this device (the only option
  // would be a 1/8 DC-only approximation), so render a placeholder instead.
  JpegMode mode = JpegMode::Baseline;
  if (!getModeFromHeader(imagePath, mode)) {
    LOG_ERR("JPG", "Could not determine JPEG mode (no SOF marker): %s", imagePath.c_str());
    return false;
  }
  if (mode != JpegMode::Baseline) {
    LOG_INF("JPG", "Unsupported JPEG mode (%s) — drawing placeholder: %s",
            mode == JpegMode::Progressive ? "progressive" : "other", imagePath.c_str());
    drawUnsupportedPlaceholder(renderer, config);
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("JPG", imagePath, file)) {
    LOG_ERR("JPG", "Failed to open JPEG for decode: %s", imagePath.c_str());
    return false;
  }

  TJpgSession session;
  session.file = &file;
  session.ctx = &ctx;

  // new[] is max-aligned, satisfying TJpgDec's word-alignment requirement; freed on scope exit.
  std::unique_ptr<uint8_t[]> pool(new (std::nothrow) uint8_t[TJPG_WORK_POOL_SIZE]);
  if (!pool) {
    LOG_ERR("JPG", "Failed to allocate TJpgDec work pool (%u bytes)", static_cast<unsigned>(TJPG_WORK_POOL_SIZE));
    file.close();
    return false;
  }

  JDEC jdec;
  JRESULT jr = jd_prepare(&jdec, tjpgInput, pool.get(), TJPG_WORK_POOL_SIZE, &session);
  if (jr != JDR_OK) {
    LOG_ERR("JPG", "TJpgDec prepare failed (jr=%d): %s", jr, imagePath.c_str());
    file.close();
    return false;
  }

  int srcWidth = jdec.width;
  int srcHeight = jdec.height;
  if (srcWidth <= 0 || srcHeight <= 0) {
    LOG_ERR("JPG", "Invalid JPEG dimensions: %dx%d", srcWidth, srcHeight);
    file.close();
    return false;
  }

  // Calculate overall target scale
  float targetScale;
  int destWidth, destHeight;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    destWidth = config.maxWidth;
    destHeight = config.maxHeight;
    targetScale = (float)destWidth / srcWidth;
  } else {
    float scaleX = (config.maxWidth > 0 && srcWidth > config.maxWidth) ? (float)config.maxWidth / srcWidth : 1.0f;
    float scaleY = (config.maxHeight > 0 && srcHeight > config.maxHeight) ? (float)config.maxHeight / srcHeight : 1.0f;
    targetScale = (scaleX < scaleY) ? scaleX : scaleY;
    if (targetScale > 1.0f) targetScale = 1.0f;

    destWidth = (int)(srcWidth * targetScale);
    destHeight = (int)(srcHeight * targetScale);
  }

  // Coarse DCT downscale via TJpgDec's built-in 1/1..1/8 scaling; the fine resampler
  // in emitGrayBlock covers the residual ratio.
  uint8_t tjpgScale;
  int jpegScaleDenom = chooseJpegScale(targetScale, tjpgScale);

  ctx.scaledSrcWidth = (srcWidth + jpegScaleDenom - 1) / jpegScaleDenom;
  ctx.scaledSrcHeight = (srcHeight + jpegScaleDenom - 1) / jpegScaleDenom;

  // Validate memory footprint against the post-scaling decode size, not raw dimensions.
  // A 1447x2200 image decoded at 1/4 scale is only ~362x550 — well within limits.
  if (!validateImageDimensions(ctx.scaledSrcWidth, ctx.scaledSrcHeight, "JPEG")) {
    file.close();
    return false;
  }
  ctx.dstWidth = destWidth;
  ctx.dstHeight = destHeight;
  if (destWidth <= 0 || destHeight <= 0) {
    LOG_ERR("JPG", "Zero-sized output (%dx%d), aborting", destWidth, destHeight);
    file.close();
    return false;
  }
  ctx.fineScaleFPX = (int32_t)((int64_t)destWidth * FP_ONE / ctx.scaledSrcWidth);
  ctx.invScaleFPX = (int32_t)((int64_t)ctx.scaledSrcWidth * FP_ONE / destWidth);
  ctx.fineScaleFPY = (int32_t)((int64_t)destHeight * FP_ONE / ctx.scaledSrcHeight);
  ctx.invScaleFPY = (int32_t)((int64_t)ctx.scaledSrcHeight * FP_ONE / destHeight);

  LOG_DBG("JPG", "JPEG %dx%d -> %dx%d (scale %.2f, jpegScale 1/%d, fineScale %.2f)", srcWidth, srcHeight, destWidth,
          destHeight, targetScale, jpegScaleDenom, (float)destWidth / ctx.scaledSrcWidth);

  // Start streaming the pixel cache to disk. The band only needs to hold the
  // tallest single decode block: a TJpgDec MCU is at most 16 scaled-source rows
  // tall, which our fine scale maps to this many output rows.
  // (See PixelCache for why streaming replaced a full-image buffer; ported from
  // upstream commit d9bcef7a, crosspoint-reader#2230.)
  ctx.caching = shouldEnableJpegCache(config, destWidth, destHeight);
#ifdef JPG_DIAG_DISABLE_CACHE
  // Diagnostic bisect: skip the streaming band-cache writer (DirectCacheWriter into
  // ctx.cache.buffer) entirely. Define in platformio.local.ini (build_flags) for a
  // dedicated run, never in normal builds.
  ctx.caching = false;
#endif
  if (ctx.caching) {
    const int maxBlockDstRows = (int)(((int64_t)16 * ctx.fineScaleFPY) >> FP_SHIFT) + 2;
    if (!ctx.cache.begin(config.cachePath, destWidth, destHeight, config.x, config.y, maxBlockDstRows)) {
      LOG_ERR("JPG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  if (shouldForceBayerDither(config)) {
    LOG_DBG("JPG", "Low-memory mode: forcing Bayer dithering (%u free)", static_cast<unsigned>(ESP.getFreeHeap()));
    ctx.effectiveDitherMode = ImageDitherMode::Bayer;
  }

  // See PngToFramebufferConverter for rationale: BW-only display needs a 1-bit
  // dither so mid-grays don't collapse to black under DirectPixelWriter's `< 3` rule.
  if (config.monochromeOutput) {
    ctx.atkinson1BitDitherer.reset(new (std::nothrow) Atkinson1BitDitherer(destWidth));
    if (!ctx.atkinson1BitDitherer) {
      LOG_ERR("JPG", "Failed to allocate 1-bit Atkinson ditherer, falling back to 4-level dither");
    }
  }

  if (config.useDithering && !ctx.atkinson1BitDitherer) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    switch (ctx.effectiveDitherMode) {
      case ImageDitherMode::Atkinson:
        ctx.atkinsonDitherer.reset(new (std::nothrow) AtkinsonDitherer(destWidth));
        if (!ctx.atkinsonDitherer) {
          LOG_ERR("JPG", "Failed to allocate Atkinson ditherer, falling back to Bayer");
        }
        break;
      case ImageDitherMode::DiffusedBayer:
        ctx.diffusedBayerDitherer.reset(new (std::nothrow) DiffusedBayerDitherer(destWidth));
        if (!ctx.diffusedBayerDitherer) {
          LOG_ERR("JPG", "Failed to allocate diffused Bayer ditherer, falling back to Bayer");
        }
        break;
      case ImageDitherMode::Bayer:
      case ImageDitherMode::COUNT:
      default:
        break;
    }
#endif
  }

  jpgCheckHeap("jpg_before_decode");
  unsigned long decodeStart = millis();
  jr = jd_decomp(&jdec, tjpgOutput, tjpgScale);
  unsigned long decodeTime = millis() - decodeStart;
  // Check before abort() so a corrupt reading is attributed to the decode itself
  // rather than to the SD-buffer free inside cache cleanup. Runs on both paths.
  jpgCheckHeap("jpg_after_decode");
  file.close();

  if (jr != JDR_OK) {
    LOG_ERR("JPG", "TJpgDec decode failed (jr=%d): %s", jr, imagePath.c_str());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("JPG", "JPEG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache file. Note: a flush failure mid-decode clears
  // ctx.caching (the partial file is dropped), so re-read the flag here.
  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool JpegToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasJpgExtension(extension);
}
