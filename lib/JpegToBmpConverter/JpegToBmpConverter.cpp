#include "JpegToBmpConverter.h"

#include <BuildArena.h>
#include <CooperativeAbort.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>
#include <Memory.h>
#include <ProgressiveJpeg.h>
#include <tjpgd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <optional>

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

struct BmpConvertCtx;  // forward decls for the session below
struct SinkSet;

// TJpgDec session passed through jd->device to the I/O and output callbacks:
// the input callback reads from `file`; the output callback writes via `sinks`
// (set only once the contexts are built, just before jd_decomp).
struct BmpTjpgSession {
  FsFile* file;
  SinkSet* sinks;
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

// One complete source row (srcWidth gray pixels) at source row y: scaled on X into the row
// accumulators and flushed on Y as output rows complete, or written straight through at 1:1.
// Shared by the TJpgDec MCU-row path and the progressive decoder's band path.
static void processSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int y) {
  if (!ctx->needsScaling) {
    // 1:1 — outWidth == srcWidth, write directly
    writeOutputRow(ctx, srcRow, y);
    return;
  }
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

// Every output a decode feeds (memory audit 2026-09, R9 item 2): the Lyra carousel needs two
// thumbnails of each cover, and decoding the JPEG once per size doubled a cold Home's decode time
// (a 1.3 MB progressive cover, ~4.5 s per decode on the X3). One decode now feeds them all: each
// output keeps its own context -- geometry, resampling accumulators, ditherer, BMP row -- and all
// of them take the same source rows, so every output is still resampled from gray and dithered
// once (a dithered image is never rescaled).
constexpr int kMaxOutputs = JpegToBmpConverter::kMaxTargets;
struct SinkSet {
  BmpConvertCtx* ctx[kMaxOutputs] = {};
  int count = 0;
  int srcWidth = 0;
  int srcHeight = 0;
  // TJpgDec path: one MCU row of source pixels (up to MAX_MCU_HEIGHT rows x srcWidth), filled
  // column by column as the output callbacks arrive and shared by every output.
  uint8_t* mcuBuf = nullptr;
  bool error = false;
  // One-shot conversions give way to button input mid-decode (CooperativeAbort) and are restarted
  // later. A JpegThumbSession is sliced by its caller instead, which checks for input between
  // slices; aborting inside one would throw away the work the slicing exists to keep.
  bool pollAbort = true;

  void processRow(const uint8_t* srcRow, const int y) {
    for (int i = 0; i < count; ++i) {
      processSourceRow(ctx[i], srcRow, y);
      if (ctx[i]->error) error = true;
    }
  }
};

// Full progressive decode (ProgressiveJpeg): each band is one block row of full-width gray rows
// at the chosen 1/2^s scale, fed through the same per-row scaling as a TJpgDec MCU row.
static bool progressiveBandOutput(void* user, uint16_t y, const uint8_t* gray, uint16_t width, uint16_t rows,
                                  uint16_t stride) {
  auto* sinks = static_cast<SinkSet*>(user);
  if (!sinks || sinks->error || width != sinks->srcWidth) return false;
  HalSystem::feedWatchdog();
  for (uint16_t r = 0; r < rows; ++r) {
    const int srcY = y + r;
    if (srcY >= sinks->srcHeight) break;
    sinks->processRow(gray + static_cast<size_t>(r) * stride, srcY);
    if (sinks->error) return false;
  }
  return true;
}

static bool progressiveFullShouldAbort(void*) {
  HalSystem::feedWatchdog();  // the index pass reads the whole file before the first band
  if (!CooperativeAbort::shouldAbortLongTask()) return false;
  CooperativeAbort::markAborted();
  return true;
}

// A JpegThumbSession's decoder: its caller slices the work and checks for input between slices.
static bool progressiveSessionPoll(void*) {
  HalSystem::feedWatchdog();
  return false;
}

// TJpgDec output callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG). JRECT is inclusive and the
// grayscale bitmap is packed tightly at the block width. Accumulates columns into
// mcuBuf; once the last column arrives (completing the MCU row), applies scaling +
// dithering and writes packed BMP rows to bmpOut.
int tjpgBmpOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  auto* sinks = static_cast<BmpTjpgSession*>(jd->device)->sinks;
  if (!sinks || sinks->error) return 0;

  // Yield to pending button input: abort the decode so the main loop can service
  // the press. The partial BMP is discarded by the caller and regenerated later.
  // markAborted() distinguishes this deliberate bail from a plain decode failure.
  if (sinks->pollAbort && CooperativeAbort::shouldAbortLongTask()) {
    CooperativeAbort::markAborted();
    sinks->error = true;
    return 0;
  }

  const uint8_t* pixels = static_cast<const uint8_t*>(bitmap);
  const int validW = rect->right - rect->left + 1;
  const int blockH = rect->bottom - rect->top + 1;
  const int stride = validW;  // TJpgDec packs each block tightly at its width
  const int blockX = rect->left;
  const int blockY = rect->top;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= sinks->srcWidth || blockY >= sinks->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, sinks->srcWidth,
            sinks->srcHeight);
    sinks->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= sinks->srcWidth) ? validW : (sinks->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(sinks->mcuBuf + r * sinks->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < sinks->srcWidth) return 1;

  // Process each complete source row in this MCU row, into every output.
  // Clamp to MAX_MCU_HEIGHT so srcRow never indexes past the populated mcuBuf rows.
  const int safeEndRow = blockY + std::min(blockH, MAX_MCU_HEIGHT);
  for (int y = blockY; y < safeEndRow && y < sinks->srcHeight; y++) {
    sinks->processRow(sinks->mcuBuf + (y - blockY) * sinks->srcWidth, y);
  }

  return sinks->error ? 0 : 1;
}

static bool progressiveBmpOutput(void* user, uint16_t y, const uint8_t* grayscale, uint16_t width) {
  auto* ctx = static_cast<BmpConvertCtx*>(user);
  if (!ctx || ctx->error || width != ctx->outWidth || y >= ctx->outHeight) return false;
  writeOutputRow(ctx, grayscale, y);
  return !ctx->error;
}

// What a progressive cover's decode read, and whether through the read windows.
static void logCoverReads(const ProgressiveJpeg::DecodeStats& stats, const bool windows) {
  LOG_DBG("JPG", "Progressive cover read %u times / %u bytes (%s)", static_cast<unsigned>(stats.reads),
          static_cast<unsigned>(stats.bytesRead), windows ? "read windows" : "shared buffer");
  (void)stats;  // only read by LOG_DBG, which compiles out at lower log levels
  (void)windows;
}

// One output from the 1/8 decode resampled to its size (ProgressiveJpeg::decodeResized): when no
// finer scale's workspace fits, or the 1/8 picture is smaller than the output and the row pipeline
// would enlarge it into blocks. `workspace`: from the caller's region, or null for a heap block.
// `scratch`: that region, for the read windows.
static bool convertResized(FsFile& jpegFile, Print& sink, int targetWidth, int targetHeight, bool oneBit, bool crop,
                           const ProgressiveJpeg::ImageInfo& image, bool eightBit, const uint32_t base,
                           uint8_t* workspace, const size_t workspaceSize, BuildArena* scratch) {
  // One row per write is one file call per row (see BufferedPrint); coalesce them.
  BufferedPrint bmpOut(sink);
  if (image.width == 0 || image.height == 0) return false;

  int outWidth = image.width;
  int outHeight = image.height;
  if (targetWidth > 0 && targetHeight > 0) {
    const float scaleX = static_cast<float>(targetWidth) / image.width;
    const float scaleY = static_cast<float>(targetHeight) / image.height;
    const float scale = crop ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    outWidth = std::max(1, static_cast<int>(image.width * scale));
    outHeight = std::max(1, static_cast<int>(image.height * scale));
  }
  // The resize emits rows at a 16-bit size: a crop-fill of an extreme aspect ratio past it would
  // be cut short, and a BMP with too few rows reported complete.
  if (outWidth > UINT16_MAX || outHeight > UINT16_MAX) return false;

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

  ProgressiveJpeg::DecodeOptions options;
  options.base = base;
  options.shouldAbort = progressiveFullShouldAbort;
  options.workspace = workspace;
  options.workspaceSize = workspaceSize;
  // A scan each to read through, from what the region has left after the workspace; released
  // first, as taken last. Without them the index pass reads the whole file 512 bytes at a time.
  ProgressiveJpeg::ReadWindows windows;
  windows.take(scratch);
  windows.applyTo(options);
  ProgressiveJpeg::DecodeStats stats;
  options.stats = &stats;
  const auto result = ProgressiveJpeg::decodeResized(jpegFile, options, static_cast<uint16_t>(outWidth),
                                                     static_cast<uint16_t>(outHeight), progressiveBmpOutput, &ctx);
  logCoverReads(stats, windows.held());
  if (result != ProgressiveJpeg::Result::Ok) {
    LOG_ERR("JPG", "Resized progressive cover failed: %s", ProgressiveJpeg::resultName(result));
    return false;
  }
  // Fold the final flush into the result: a write that fails here would otherwise be reported
  // as a complete BMP, and the caller would cache a truncated file.
  if (!bmpOut.flushBuffer()) {
    LOG_ERR("JPG", "Failed to flush buffered BMP output");
    return false;
  }
  LOG_DBG("JPG", "Progressive cover resized from 1/8: %ux%u -> %dx%d", image.width, image.height, outWidth, outHeight);
  return true;
}

}  // namespace

// One BMP a decode writes: the raw sink and the box its image is crop-fitted to.
struct OutputTarget {
  Print* out;
  int targetWidth;
  int targetHeight;
};

// The finest DCT pre-scale any output needs: the largest of each output's scale to fit its box
// (the TJpgDec and progressive paths both pick the largest pre-scale that keeps both axes at or
// above the target, so the fine scaler only ever downscales). With several outputs the smaller
// ones then resample from the larger one's source rows.
static float finestTargetScale(const OutputTarget* targets, const int count, const int srcW, const int srcH) {
  float finest = 0.0f;
  for (int i = 0; i < count; ++i) {
    if (targets[i].targetWidth <= 0 || targets[i].targetHeight <= 0) return 1.0f;  // native size wanted
    const float scaleX = static_cast<float>(targets[i].targetWidth) / srcW;
    const float scaleY = static_cast<float>(targets[i].targetHeight) / srcH;
    finest = std::max(finest, scaleX > scaleY ? scaleX : scaleY);
  }
  return finest;
}

// Output geometry for one target: scale factors, crop window, BMP header. Everything else of the
// context (row buffers, ditherer) is set by the caller.
static void initOutputGeometry(BmpConvertCtx& ctx, Print& bmpOut, const int effectiveSrcW, const int effectiveSrcH,
                               const int targetWidth, const int targetHeight, const bool oneBit, const bool crop,
                               const bool eightBit) {
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
}

// One scoped block in a lent region, released when the scope goes (LIFO with any block reserved
// after it: whatever holds a later block must go first).
struct ArenaBlockScope {
  BuildArena* arena = nullptr;
  BuildArena::Block block;
  // Reserve a block and take `bytes` from it; null when there is no region or it is full (the
  // block is then released again so the caller can fall back to the heap).
  uint8_t* take(BuildArena* a, const size_t bytes) {
    if (a == nullptr || !a->valid()) return nullptr;
    arena = a;
    block = arena->reserveBlock();
    auto* p = static_cast<uint8_t*>(arena->alloc(bytes));
    if (p == nullptr) arena->release(block);
    return p;
  }
  ~ArenaBlockScope() {
    if (arena != nullptr && block.valid()) arena->release(block);
  }
};

// Everything after the decoder is chosen and the decoded extent (srcW x srcH) is known, for one or
// more outputs: each output's geometry, crop window and BMP header, and the row pipeline's buffers.
// A one-shot conversion holds one around a single decode; a JpegThumbSession holds one across its
// slices. needMcuBuf: the TJpgDec path assembles an MCU row before it can process rows; the
// progressive decoder hands over whole rows.
class RowPipeline {
 public:
  RowPipeline() = default;
  RowPipeline(const RowPipeline&) = delete;
  RowPipeline& operator=(const RowPipeline&) = delete;
  // Frees exactly the heap buffers it took and releases its region block; the BMP streams (whose
  // destructors flush) go after, as members.
  ~RowPipeline();

  bool begin(const OutputTarget* targets, int count, int srcW, int srcH, bool oneBit, bool crop, bool eightBit,
             bool needMcuBuf, BuildArena* scratch);
  SinkSet& sinks() { return sinks_; }
  // Every output has all its rows and is flushed to its stream.
  bool finish();

 private:
  std::optional<BufferedPrint> buffered_[kMaxOutputs];
  BmpConvertCtx ctxs_[kMaxOutputs] = {};
  SinkSet sinks_;
  int count_ = 0;
  bool mcuOnHeap_ = false;
  bool rowOnHeap_[kMaxOutputs] = {};
  bool accumOnHeap_[kMaxOutputs] = {};
  BuildArena* arena_ = nullptr;
  BuildArena::Block block_;
};

RowPipeline::~RowPipeline() {
  for (int i = 0; i < count_; ++i) {
    BmpConvertCtx& ctx = ctxs_[i];
    if (accumOnHeap_[i]) {
      delete[] ctx.rowAccum;
      delete[] ctx.rowCount;
    }
    delete ctx.atkinsonDitherer;
    delete ctx.fsDitherer;
    delete ctx.atkinson1BitDitherer;
    if (rowOnHeap_[i]) free(ctx.bmpRow);
  }
  if (mcuOnHeap_) free(sinks_.mcuBuf);
  if (arena_ != nullptr && block_.valid()) arena_->release(block_);
}

bool RowPipeline::begin(const OutputTarget* targets, const int count, const int effectiveSrcW, const int effectiveSrcH,
                        const bool oneBit, const bool crop, const bool eightBit, const bool needMcuBuf,
                        BuildArena* scratch) {
  if (count < 1 || count > kMaxOutputs || count_ != 0) return false;
  count_ = count;
  // One row per write is one file call per row (see BufferedPrint); coalesce them. With several
  // outputs each gets half the buffer, so the heap they cost together stays that of one.
  const size_t bufferBytes = count > 1 ? BufferedPrint::DEFAULT_BUFFER_BYTES / 2 : BufferedPrint::DEFAULT_BUFFER_BYTES;
  for (int i = 0; i < count; ++i) {
    buffered_[i].emplace(*targets[i].out, bufferBytes);
    initOutputGeometry(ctxs_[i], *buffered_[i], effectiveSrcW, effectiveSrcH, targets[i].targetWidth,
                       targets[i].targetHeight, oneBit, crop, eightBit);
  }

  // The row pipeline -- the MCU strip (up to 16 rows of the source width, 16+ KB on a wide
  // cover, shared by every output), each output's BMP row and scaling accumulators -- comes from
  // the lent region when there is one, in a block of its own released on every return path; the
  // heap only when there is not. Home's later visits run this with the reading state's ~35 KB
  // free, and the 28 KB heap reserve for these buffers refused every remaining cover ("Not enough
  // heap for JPEG decoder (27528 free, need 28672)", X3 2026-09-26) while 30 KB of the region sat
  // idle. Each buffer remembers where it came from, so a region that runs out mid-way falls back
  // to the heap for the rest and the cleanup frees exactly what it owns.
  const size_t mcuBytes = needMcuBuf ? static_cast<size_t>(MAX_MCU_HEIGHT) * effectiveSrcW : 0;
  size_t pipelineBytes = mcuBytes + 2 * alignof(std::max_align_t);
  for (int i = 0; i < count; ++i) {
    const size_t accumBytes = ctxs_[i].needsScaling ? static_cast<size_t>(ctxs_[i].outWidth) * sizeof(uint32_t) : 0;
    pipelineBytes += ctxs_[i].bytesPerRow + 2 * accumBytes + 3 * alignof(std::max_align_t);
  }
  if (scratch != nullptr && scratch->valid() && scratch->capacity() - scratch->used() >= pipelineBytes) {
    arena_ = scratch;
    block_ = scratch->reserveBlock();
  }
  sinks_.count = count;
  sinks_.srcWidth = effectiveSrcW;
  sinks_.srcHeight = effectiveSrcH;
  const auto fromRegion = [&](const size_t bytes) -> uint8_t* {
    return arena_ != nullptr ? static_cast<uint8_t*>(arena_->alloc(bytes)) : nullptr;
  };

  if (needMcuBuf) {
    sinks_.mcuBuf = fromRegion(mcuBytes);
    if (sinks_.mcuBuf == nullptr) {
      sinks_.mcuBuf = static_cast<uint8_t*>(malloc(mcuBytes));
      mcuOnHeap_ = sinks_.mcuBuf != nullptr;
    }
    if (!sinks_.mcuBuf) {
      LOG_ERR("JPG", "Failed to allocate MCU buffer (%d bytes)", MAX_MCU_HEIGHT * effectiveSrcW);
      return false;
    }
    memset(sinks_.mcuBuf, 0, mcuBytes);
  }

  for (int i = 0; i < count; ++i) {
    BmpConvertCtx& ctx = ctxs_[i];
    ctx.bmpRow = fromRegion(ctx.bytesPerRow);
    if (ctx.bmpRow == nullptr) {
      ctx.bmpRow = static_cast<uint8_t*>(malloc(ctx.bytesPerRow));
      rowOnHeap_[i] = ctx.bmpRow != nullptr;
    }
    if (!ctx.bmpRow) {
      LOG_ERR("JPG", "Failed to allocate BMP row buffer");
      return false;
    }

    if (ctx.needsScaling) {
      const size_t accumBytes = static_cast<size_t>(ctx.outWidth) * sizeof(uint32_t);
      ctx.rowAccum = reinterpret_cast<uint32_t*>(fromRegion(accumBytes));
      ctx.rowCount = ctx.rowAccum != nullptr ? reinterpret_cast<uint32_t*>(fromRegion(accumBytes)) : nullptr;
      if (ctx.rowAccum != nullptr && ctx.rowCount != nullptr) {
        memset(ctx.rowAccum, 0, accumBytes);
        memset(ctx.rowCount, 0, accumBytes);
      } else {
        ctx.rowAccum = new (std::nothrow) uint32_t[ctx.outWidth]();
        ctx.rowCount = new (std::nothrow) uint32_t[ctx.outWidth]();
        accumOnHeap_[i] = true;
      }
      if (!ctx.rowAccum || !ctx.rowCount) {
        LOG_ERR("JPG", "Failed to allocate scaling buffers");
        return false;
      }
      ctx.nextOutY_srcStart = ctx.scaleY_fp;
    }

    if (oneBit) {
      ctx.atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(ctx.outWidth);
    } else if (!eightBit) {
      if (USE_ATKINSON) {
        ctx.atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(ctx.outWidth);
      } else if (USE_FLOYD_STEINBERG) {
        ctx.fsDitherer = new (std::nothrow) FloydSteinbergDitherer(ctx.outWidth);
      }
    }
    sinks_.ctx[i] = &ctx;
  }

  return true;
}

bool RowPipeline::finish() {
  for (int i = 0; i < count_; ++i) {
    const BmpConvertCtx& ctx = ctxs_[i];
    if (ctx.needsScaling && ctx.currentOutY < ctx.outHeight) {
      LOG_ERR("JPG", "JPEG decode incomplete: %d/%d output rows written", ctx.currentOutY, ctx.outHeight);
      return false;
    }
    if (!buffered_[i]->flushBuffer()) {
      LOG_ERR("JPG", "Failed to flush buffered BMP output");
      return false;
    }
  }
  return true;
}

// A one-shot conversion: the pipeline around `run(sinks)`, which drives whichever decoder feeds
// SinkSet::processRow to the end.
template <typename Run>
static bool convertScaledMulti(const OutputTarget* targets, const int count, const int effectiveSrcW,
                               const int effectiveSrcH, const bool oneBit, const bool crop, const bool eightBit,
                               const bool needMcuBuf, BuildArena* scratch, Run&& run) {
  RowPipeline pipeline;
  if (!pipeline.begin(targets, count, effectiveSrcW, effectiveSrcH, oneBit, crop, eightBit, needMcuBuf, scratch)) {
    return false;
  }
  if (!run(pipeline.sinks())) return false;
  return pipeline.finish();
}

// Heap the progressive path needs beyond its decoder workspace: the row pipeline's buffers
// (accumulators, BMP row, a ditherer -- a few KB at thumbnail widths) and a floor for the rest.
constexpr size_t PROGRESSIVE_ROW_PIPELINE_HEAP = 16 * 1024;

// The DCT pre-scale for a full progressive decode into the row pipeline: the largest that keeps
// both axes >= every target (as the TJpgDec path chooses), then coarser while the workspace does
// not fit. False when the cover is the 1/8 decode resized instead (convertResized): no scale fits,
// or only 1/8 does and the pipeline would enlarge it -- a source pixel per output pixel, blocks.
static bool chooseProgressiveShift(const OutputTarget* targets, const int count, const ProgressiveJpeg::ImageInfo& info,
                                   BuildArena* scratch, uint8_t* shiftOut) {
  uint8_t shift = 0;
  const float scaleMax = finestTargetScale(targets, count, info.width, info.height);
  if (scaleMax <= 0.125f) {
    shift = 3;
  } else if (scaleMax <= 0.25f) {
    shift = 2;
  } else if (scaleMax <= 0.5f) {
    shift = 1;
  }
  const bool arenaBacked = scratch != nullptr && scratch->valid();
  for (; shift <= 3; ++shift) {
    const size_t workspace = ProgressiveJpeg::workspaceBytes(info, shift);
    if (workspace == 0) continue;
    // With a lent region the workspace comes from it and the heap only owes the row pipeline.
    const bool fits = arenaBacked ? (scratch->capacity() - scratch->used() >= workspace + alignof(std::max_align_t) &&
                                     ESP.getFreeHeap() >= PROGRESSIVE_ROW_PIPELINE_HEAP)
                                  : ESP.getFreeHeap() >= workspace + PROGRESSIVE_ROW_PIPELINE_HEAP;
    if (fits) break;
  }
  if (shift > 3) {
    LOG_INF("JPG", "Progressive cover: no scale's workspace fits (%u free); the 1/8 decode, resized",
            static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  if (shift == 3 && scaleMax > 0.125f) {
    LOG_INF("JPG", "Progressive cover: only 1/8 fits (%u free); resized rather than enlarged in blocks",
            static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  if ((info.width >> shift) <= 0 || (info.height >> shift) <= 0) return false;
  *shiftOut = shift;
  return true;
}

// Full progressive decode into the BMP pipeline. Returns false without writing anything when
// chooseProgressiveShift() leaves the cover to convertResized().
static bool convertFromProgressive(FsFile& jpegFile, const OutputTarget* targets, const int count,
                                   const ProgressiveJpeg::ImageInfo& info, const bool oneBit, const bool crop,
                                   const bool eightBit, bool* attempted, BuildArena* scratch, const uint32_t base) {
  *attempted = false;
  uint8_t shift = 0;
  if (!chooseProgressiveShift(targets, count, info, scratch, &shift)) return false;
  const int effectiveSrcW = info.width >> shift;
  const int effectiveSrcH = info.height >> shift;
  *attempted = true;
  LOG_DBG("JPG", "Progressive cover %ux%u at 1/%d -> %dx%d (%d output%s)", info.width, info.height, 1 << shift,
          effectiveSrcW, effectiveSrcH, count, count == 1 ? "" : "s");
  const uint8_t scaleShift = shift;
  ArenaBlockScope workspaceScope;
  const size_t workspaceBytes = ProgressiveJpeg::workspaceBytes(info, shift);
  uint8_t* workspace = workspaceScope.take(scratch, workspaceBytes);
  return convertScaledMulti(targets, count, effectiveSrcW, effectiveSrcH, oneBit, crop, eightBit,
                            /*needMcuBuf=*/false, scratch, [&](SinkSet& sinks) {
                              ProgressiveJpeg::DecodeOptions options;
                              options.base = base;
                              options.scaleShift = scaleShift;
                              options.shouldAbort = progressiveFullShouldAbort;
                              options.workspace = workspace;  // null: decode() takes one heap block
                              options.workspaceSize = workspace != nullptr ? workspaceBytes : 0;
                              // A scan each to read through, from what the region has left after the
                              // workspace and the row pipeline; released first, as taken last.
                              ProgressiveJpeg::ReadWindows windows;
                              windows.take(scratch);
                              windows.applyTo(options);
                              ProgressiveJpeg::DecodeStats stats;
                              options.stats = &stats;
                              const auto result =
                                  ProgressiveJpeg::decode(jpegFile, options, progressiveBandOutput, &sinks);
                              if (result != ProgressiveJpeg::Result::Ok || sinks.error) {
                                LOG_ERR("JPG", "Progressive cover decode failed (%s, ctxErr=%d)",
                                        ProgressiveJpeg::resultName(result), sinks.error ? 1 : 0);
                                return false;
                              }
                              logCoverReads(stats, windows.held());
                              return true;
                            });
}

// A baseline decode prepared up to its first MCU: the work pool (lent region or heap), the decoder
// object, its I/O session and the scale the targets need. Holds pointers into itself (jdec.device,
// the pool), so it stays where it was built.
struct TjpgJob {
  ArenaBlockScope poolScope;
  std::unique_ptr<uint8_t[]> heapPool;
  BmpTjpgSession io{nullptr, nullptr};
  JDEC jdec{};
  uint8_t scale = 0;
  int effectiveSrcW = 0;
  int effectiveSrcH = 0;

  TjpgJob() = default;
  TjpgJob(const TjpgJob&) = delete;
  TjpgJob& operator=(const TjpgJob&) = delete;

  bool prepare(FsFile& jpegFile, uint32_t base, const OutputTarget* targets, int count, BuildArena* scratch);
};

bool TjpgJob::prepare(FsFile& jpegFile, const uint32_t base, const OutputTarget* targets, const int count,
                      BuildArena* scratch) {
  // The work pool comes from the lent region when there is one (its allocations are max-aligned,
  // satisfying TJpgDec's word-alignment requirement); the heap then only owes the row pipeline.
  uint8_t* pool = poolScope.take(scratch, TJPG_WORK_POOL_SIZE);
  // With the pool in the region, the row pipeline comes from it too when the room is there (MCU
  // strip 16 KB at most + rows; see RowPipeline), and the heap then owes only the ditherers and
  // the BMP output buffering -- a few KB. The full reserve applies otherwise.
  constexpr size_t ROWS_IN_REGION_MIN_FREE_HEAP = 8 * 1024;
  const bool rowsInRegion =
      pool != nullptr && scratch->capacity() - scratch->used() >= static_cast<size_t>(MAX_MCU_HEIGHT) * 1200 + 8 * 1024;
  const size_t heapFloor = rowsInRegion      ? ROWS_IN_REGION_MIN_FREE_HEAP
                           : pool != nullptr ? MIN_FREE_HEAP - TJPG_WORK_POOL_SIZE
                                             : MIN_FREE_HEAP;
  if (ESP.getFreeHeap() < heapFloor) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(),
            static_cast<unsigned>(heapFloor));
    return false;
  }

  jpegFile.seek(base);

  if (pool == nullptr) {
    // new[] is max-aligned, satisfying TJpgDec's word-alignment requirement.
    heapPool.reset(new (std::nothrow) uint8_t[TJPG_WORK_POOL_SIZE]);
    if (!heapPool) {
      LOG_ERR("JPG", "Failed to allocate TJpgDec work pool (%u bytes)", static_cast<unsigned>(TJPG_WORK_POOL_SIZE));
      return false;
    }
    pool = heapPool.get();
  }

  io.file = &jpegFile;
  io.sinks = nullptr;  // set once the contexts are built, just before the first MCU

  const JRESULT jr = jd_prepare(&jdec, tjpgBmpInput, pool, TJPG_WORK_POOL_SIZE, &io);
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

  // Pick the largest DCT pre-scale that keeps both axes >= every target so the fine scaler
  // always downscales (never upscales) on either axis. `scale` is the TJpgDec scale exponent
  // (0=1/1, 1=1/2, 2=1/4, 3=1/8). Using max(scaleX, scaleY) is safe for both crop=true (uses max
  // scale) and crop=false (uses min scale).
  int jpegScaleDenom = 1;
  {
    const float scaleMax = finestTargetScale(targets, count, srcWidth, srcHeight);
    if (scaleMax <= 0.125f) {
      scale = 3;
      jpegScaleDenom = 8;
    } else if (scaleMax <= 0.25f) {
      scale = 2;
      jpegScaleDenom = 4;
    } else if (scaleMax <= 0.5f) {
      scale = 1;
      jpegScaleDenom = 2;
    }
  }

  // TJpgDec's descaled output is floor(dim / 2^scale): every MCU side (8 or 16 px) is a
  // multiple of the scale denominator, so the per-MCU right/bottom shifts sum to exactly
  // the floor. These MUST match TJpgDec's actual output extent — the output callback only
  // flushes an MCU row once a block reaches `srcWidth`, so an over-estimate (e.g. ceil
  // division on an odd dimension like 333 -> 167 vs TJpgDec's 166) means the last column
  // never arrives and zero rows are ever written.
  effectiveSrcW = srcWidth / jpegScaleDenom;
  effectiveSrcH = srcHeight / jpegScaleDenom;

  if (jpegScaleDenom > 1) {
    LOG_DBG("JPG", "Using 1/%d DCT scale: %dx%d -> %dx%d", jpegScaleDenom, srcWidth, srcHeight, effectiveSrcW,
            effectiveSrcH);
  }
  return true;
}

// Internal implementation with configurable target sizes and bit depth: one decode, one BMP per target.
static bool jpegFileToBmpStreamsInternal(FsFile& jpegFile, const OutputTarget* targets, const int count,
                                         const bool oneBit, const bool crop, const bool eightBit, BuildArena* scratch) {
  if (count < 1 || count > kMaxOutputs) return false;
  // Where the image starts: 0 for a JPEG file of its own, the entry's data offset when a cover
  // stored uncompressed is decoded in place out of the EPUB (Epub::openStoredCoverInPlace). Every
  // rewind below goes back HERE. They used to go to 0 -- the start of the ZIP -- so no stored JPEG
  // cover ever decoded in place ("TJpgDec prepare failed (jr=8)") and each paid the extraction the
  // in-place path exists to skip. Only PNG covers had been tested that way.
  const uint32_t base = static_cast<uint32_t>(jpegFile.position());
  for (int i = 0; i < count; ++i) {
    LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : (eightBit ? "8-bit" : "2-bit"),
            targets[i].targetWidth, targets[i].targetHeight);
  }

  ProgressiveJpeg::ImageInfo full;
  if (ProgressiveJpeg::probe(jpegFile, full, base) == ProgressiveJpeg::Result::Ok) {
    // Every scan, at the scale the targets need. Only 1/8 resolution, upscaled, is what made a
    // 221x324 progressive cover a 27x40 smear on the home screen.
    bool attempted = false;
    const bool ok =
        convertFromProgressive(jpegFile, targets, count, full, oneBit, crop, eightBit, &attempted, scratch, base);
    if (attempted) return ok;
    // The last resort: the 1/8 decode resized to each output, one decode per output -- a 1/8
    // decode is the least work a progressive file takes. Its workspace and read windows from the
    // region, when it has room, as the full decode's would be.
    for (int i = 0; i < count; ++i) {
      jpegFile.seek(base);
      ArenaBlockScope workspaceScope;
      const size_t workspaceBytes = ProgressiveJpeg::workspaceBytes(full, 3);
      uint8_t* workspace = workspaceScope.take(scratch, workspaceBytes);
      if (!convertResized(jpegFile, *targets[i].out, targets[i].targetWidth, targets[i].targetHeight, oneBit, crop,
                          full, eightBit, base, workspace, workspace != nullptr ? workspaceBytes : 0, scratch)) {
        return false;
      }
    }
    return true;
  }

  TjpgJob job;
  if (!job.prepare(jpegFile, base, targets, count, scratch)) return false;
  const bool ok =
      convertScaledMulti(targets, count, job.effectiveSrcW, job.effectiveSrcH, oneBit, crop, eightBit,
                         /*needMcuBuf=*/true, scratch, [&](SinkSet& sinks) {
                           job.io.sinks = &sinks;
                           const JRESULT jr = jd_decomp(&job.jdec, tjpgBmpOutput, job.scale);
                           if (jr != JDR_OK || sinks.error) {
                             LOG_ERR("JPG", "TJpgDec decode failed (jr=%d, ctxErr=%d)", jr, sinks.error ? 1 : 0);
                             return false;
                           }
                           return true;
                         });
  if (ok) {
    LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  }
  return ok;
}

bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& sink, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop, bool eightBit, BuildArena* scratch) {
  const OutputTarget target{&sink, targetWidth, targetHeight};
  return jpegFileToBmpStreamsInternal(jpegFile, &target, 1, oneBit, crop, eightBit, scratch);
}

bool JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(FsFile& jpegFile, const BmpTarget* targets, const int count,
                                                           BuildArena* scratch) {
  if (targets == nullptr || count < 1 || count > kMaxTargets) return false;
  OutputTarget converted[kMaxOutputs];
  for (int i = 0; i < count; ++i) {
    if (targets[i].out == nullptr) return false;
    converted[i] = OutputTarget{targets[i].out, targets[i].maxWidth, targets[i].maxHeight};
  }
  return jpegFileToBmpStreamsInternal(jpegFile, converted, count, /*oneBit=*/true, /*crop=*/true,
                                      /*eightBit=*/false, scratch);
}

// Core function: Convert JPEG file to a full-size cover BMP (2-bit, or 8-bit when
// the caller asks for the extra tonal range).
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop, bool grayscale8Bit,
                                             BuildArena* scratch) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop, grayscale8Bit, scratch);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight, BuildArena* scratch) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false, true, false, scratch);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight, BuildArena* scratch, const bool crop) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, crop, false, scratch);
}

// ---- JpegThumbSession ----------------------------------------------------------------------

struct JpegThumbSession::Impl {
  // Region blocks are released in reverse order of declaration: the decoder's working memory
  // (the progressive workspace, or the TJpgDec pool inside `tjpg`) is taken before the row
  // pipeline's block, so it is declared first and goes last.
  ArenaBlockScope workspaceScope;
  TjpgJob tjpg;
  RowPipeline pipeline;
  ProgressiveJpeg::ReadWindows windows;  // taken after the pipeline's block
  ProgressiveJpeg::DecodeStats stats;
  ProgressiveJpeg::Decoder decoder;  // ends (and rewinds the file) before the rest goes
  JDCURSOR cursor{};
  // TJpgDec reads the file as a stream; whatever the caller reads between slices must not move it.
  uint32_t streamPos = 0;
  bool isProgressive = false;
  Status status = Status::Running;
};

JpegThumbSession::JpegThumbSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
JpegThumbSession::~JpegThumbSession() = default;

bool JpegThumbSession::progressive() const { return impl_->isProgressive; }

std::unique_ptr<JpegThumbSession> JpegThumbSession::begin(FsFile& jpegFile,
                                                          const JpegToBmpConverter::BmpTarget* targets, const int count,
                                                          BuildArena* scratch) {
  if (!jpegFile || targets == nullptr || count < 1 || count > kMaxOutputs) return nullptr;
  OutputTarget converted[kMaxOutputs];
  for (int i = 0; i < count; ++i) {
    if (targets[i].out == nullptr) return nullptr;
    converted[i] = OutputTarget{targets[i].out, targets[i].maxWidth, targets[i].maxHeight};
  }
  const uint32_t base = static_cast<uint32_t>(jpegFile.position());
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl());
  if (!impl) return nullptr;

  ProgressiveJpeg::ImageInfo full;
  if (ProgressiveJpeg::probe(jpegFile, full, base) == ProgressiveJpeg::Result::Ok) {
    uint8_t shift = 0;
    if (!chooseProgressiveShift(converted, count, full, scratch, &shift)) return nullptr;  // resized: one-shot
    const int effectiveSrcW = full.width >> shift;
    const int effectiveSrcH = full.height >> shift;
    const size_t workspaceBytes = ProgressiveJpeg::workspaceBytes(full, shift);
    uint8_t* workspace = impl->workspaceScope.take(scratch, workspaceBytes);
    if (!impl->pipeline.begin(converted, count, effectiveSrcW, effectiveSrcH, /*oneBit=*/true, /*crop=*/true,
                              /*eightBit=*/false, /*needMcuBuf=*/false, scratch)) {
      return nullptr;
    }
    impl->pipeline.sinks().pollAbort = false;
    ProgressiveJpeg::DecodeOptions options;
    options.base = base;
    options.scaleShift = shift;
    options.shouldAbort = progressiveSessionPoll;
    options.workspace = workspace;  // null: the decoder takes one heap block for the session
    options.workspaceSize = workspace != nullptr ? workspaceBytes : 0;
    // A scan each to read through, from what the region has left after the workspace and the pipeline.
    impl->windows.take(scratch);
    impl->windows.applyTo(options);
    options.stats = &impl->stats;
    const auto result = impl->decoder.begin(jpegFile, options, progressiveBandOutput, &impl->pipeline.sinks());
    if (result != ProgressiveJpeg::Result::Ok) {
      LOG_ERR("JPG", "Progressive cover session refused (%s)", ProgressiveJpeg::resultName(result));
      return nullptr;
    }
    impl->isProgressive = true;
    LOG_DBG("JPG", "Sliced progressive cover %ux%u at 1/%d -> %dx%d (%d output%s)", full.width, full.height, 1 << shift,
            effectiveSrcW, effectiveSrcH, count, count == 1 ? "" : "s");
  } else {
    if (!impl->tjpg.prepare(jpegFile, base, converted, count, scratch)) return nullptr;
    if (!impl->pipeline.begin(converted, count, impl->tjpg.effectiveSrcW, impl->tjpg.effectiveSrcH, /*oneBit=*/true,
                              /*crop=*/true, /*eightBit=*/false, /*needMcuBuf=*/true, scratch)) {
      return nullptr;
    }
    impl->pipeline.sinks().pollAbort = false;
    impl->tjpg.io.sinks = &impl->pipeline.sinks();
    impl->streamPos = static_cast<uint32_t>(jpegFile.position());
    LOG_DBG("JPG", "Sliced baseline cover -> %dx%d (%d output%s)", impl->tjpg.effectiveSrcW, impl->tjpg.effectiveSrcH,
            count, count == 1 ? "" : "s");
  }
  return std::unique_ptr<JpegThumbSession>(new (std::nothrow) JpegThumbSession(std::move(impl)));
}

JpegThumbSession::Status JpegThumbSession::continueSteps(const uint16_t units) {
  Impl& s = *impl_;
  if (s.status != Status::Running) return s.status;
  SinkSet& sinks = s.pipeline.sinks();
  bool finished = false;
  if (s.isProgressive) {
    const auto result = s.decoder.step(units);
    if (result == ProgressiveJpeg::Result::Pending && !sinks.error) return Status::Running;
    if (result != ProgressiveJpeg::Result::Ok || sinks.error) {
      LOG_ERR("JPG", "Progressive cover decode failed (%s, ctxErr=%d)", ProgressiveJpeg::resultName(result),
              sinks.error ? 1 : 0);
      s.decoder.end();
      s.status = Status::Error;
      return s.status;
    }
    s.decoder.end();
    logCoverReads(s.stats, s.windows.held());
    finished = true;
  } else {
    FsFile& file = *s.tjpg.io.file;
    if (static_cast<uint32_t>(file.position()) != s.streamPos && !file.seek(s.streamPos)) {
      LOG_ERR("JPG", "Cover session: cannot seek back to %u", static_cast<unsigned>(s.streamPos));
      s.status = Status::Error;
      return s.status;
    }
    const JRESULT jr = jd_decomp_rows(&s.tjpg.jdec, tjpgBmpOutput, s.tjpg.scale, &s.cursor, units == 0 ? 1 : units);
    s.streamPos = static_cast<uint32_t>(file.position());
    if (jr != JDR_OK || sinks.error) {
      LOG_ERR("JPG", "TJpgDec decode failed (jr=%d, ctxErr=%d)", jr, sinks.error ? 1 : 0);
      s.status = Status::Error;
      return s.status;
    }
    finished = s.cursor.done != 0;
  }
  if (!finished) return Status::Running;
  s.status = s.pipeline.finish() ? Status::Done : Status::Error;
  return s.status;
}
