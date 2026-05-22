#include "PngToFramebufferConverter.h"

#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PNGdec.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the FsFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates

  // tRNS support for grayscale / truecolor PNGs. PNGdec exposes a single transparent
  // sample value (grayscale) or RGB triplet (truecolor) via getTransparentColor() and
  // sets hasAlpha=1 on those pixel types when the chunk is present. Indexed PNGs use
  // the per-entry alpha array stored at palette[768..1023] instead.
  bool hasTrnsKey{false};
  uint8_t trnsGray{0};
  uint8_t trnsR{0}, trnsG{0}, trnsB{0};

  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};

  // When the caller requests monochrome output (RenderConfig::monochromeOutput),
  // we run a proper 1-bit Atkinson dither (matching PngToBmpConverter's BW path)
  // and emit only values 0 or 3, which round-trip cleanly through the BW writer's
  // `pixelValue < 3` rule. The 4-level dither path collapses mid-grays to solid
  // black under that rule.
  int oneBitDitherRow{-1};
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  int currentDitherRow{-1};
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<DiffusedBayerDitherer> diffusedBayerDitherer;
#endif
};

// Advance the 1-bit Atkinson ditherer to the requested destination row.
// Like the 4-level path below, this handles re-decode passes that walk source
// rows non-monotonically by resetting and replaying when needed.
void prepareOneBitDitherRow(PngContext& ctx, int dstY) {
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
void prepareDitherRow(PngContext& ctx, int dstY) {
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

uint8_t ditherGray(PngContext& ctx, uint8_t gray, int localX, int outX, int outY) {
  // BW mode: route through 1-bit Atkinson and emit only 0/3 so the
  // DirectPixelWriter's `pixelValue < 3` rule maps cleanly to black/white.
  if (ctx.atkinson1BitDitherer) {
    return ctx.atkinson1BitDitherer->processPixel(gray, localX) ? 3 : 0;
  }

  if (!ctx.config || !ctx.config->useDithering) {
    return quantizeGray4Level(gray);
  }

  switch (ctx.config->ditherMode) {
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
uint8_t ditherGray(PngContext& ctx, uint8_t gray, int localX, int outX, int outY) {
  if (ctx.atkinson1BitDitherer) {
    return ctx.atkinson1BitDitherer->processPixel(gray, localX) ? 3 : 0;
  }
  (void)localX;
  return applyBayerDither4Level(gray, outX, outY);
}
#endif

// File I/O callbacks use pFile->fHandle to access the FsFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  FsFile* f =
      new FsFile();  // NOLINT(cppcoreguidelines-owning-memory) — ownership transferred via void* to PNGdec callbacks
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;  // NOLINT(cppcoreguidelines-owning-memory)
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;  // NOLINT(cppcoreguidelines-owning-memory)
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// The PNG decoder (PNGdec) is large due to internal zlib decompression buffers
// (~40 KB ucZLIB + 16 KB ucPixels at PNG_MAX_BUFFERED_PIXELS=16416 + smaller
// fields ≈ 60 KB). We heap-allocate it on demand rather than using a static
// instance, so this memory is only consumed while actually decoding/querying
// PNG images. This is critical on the ESP32-C3 where total RAM is ~320 KB.
// Use sizeof(PNG) so the precheck stays accurate if PNG_MAX_BUFFERED_PIXELS
// or other PNGdec buffers are resized.
constexpr size_t PNG_DECODER_APPROX_SIZE = sizeof(PNG);
// Headroom covers heap fragmentation: free heap is the *sum* of all free
// blocks but `new` needs a single contiguous block. 32 KB headroom on a
// ~60 KB allocation has historically been enough on this device.
constexpr size_t MIN_FREE_HEAP_FOR_PNG = PNG_DECODER_APPROX_SIZE + 32 * 1024;

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required storage is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.

// PNG row pitch in bytes — matches PNGdec's internal calculation in PNGParseInfo
// (png.inl). bpp is the per-channel bit depth (1/2/4/8 for grayscale & indexed,
// 8 elsewhere). For sub-byte depths the row is bit-packed: pitch < width.
int pngPitchBytes(int srcWidth, int pixelType, int bpp) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return ((3 * bpp) * srcWidth + 7) / 8;
    case PNG_PIXEL_GRAY_ALPHA:
      return ((2 * bpp) * srcWidth + 7) / 8;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return ((4 * bpp) * srcWidth + 7) / 8;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return (srcWidth * bpp + 7) / 8;
  }
}

int requiredPngInternalBufferBytes(int srcWidth, int pixelType, int bpp) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = pngPitchBytes(srcWidth, pixelType, bpp);
  return ((pitch + 1) * 2) + 32;
}

// Extract the bpp-bit sample at index `x` from a packed scanline (MSB-first).
// Supports bpp values 1, 2, 4, 8 — the only depths PNG allows for grayscale and indexed.
inline uint8_t extractPackedSample(const uint8_t* pPixels, int x, int bpp) {
  if (bpp == 8) return pPixels[x];
  const int pixelsPerByte = 8 / bpp;
  const uint8_t mask = (uint8_t)((1 << bpp) - 1);
  const int byteIdx = x / pixelsPerByte;
  const int shift = (pixelsPerByte - 1 - (x % pixelsPerByte)) * bpp;
  return (pPixels[byteIdx] >> shift) & mask;
}

// Convert entire source line to grayscale with alpha blending to white background.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// `bpp` is the per-channel bit depth from PNGdec (1, 2, 4, or 8 for grayscale/indexed).
// Sub-byte depths require unpacking the bit-packed scanline before lookup; treating each
// byte as a single sample silently corrupts the output (PNGdec reports indexed-with-tRNS
// PNGs at 1/2/4 bpp this way and the consequences are uninitialized palette reads).
// For grayscale and truecolor PNGs, `ctx` carries any tRNS color-key set on the image —
// matching pixels are composited to white instead of their decoded value.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const PngContext& ctx, uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bpp,
                       uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE: {
      // tRNS for grayscale stores the transparent sample value at native bit depth.
      // Compare against the raw packed sample before scaling to 8-bit gray.
      const bool useKey = ctx.hasTrnsKey;
      const uint8_t key = ctx.trnsGray;
      if (bpp == 8) {
        if (useKey) {
          for (int x = 0; x < width; x++) {
            uint8_t sample = pPixels[x];
            grayLine[x] = (sample == key) ? 255 : sample;
          }
        } else {
          memcpy(grayLine, pPixels, width);
        }
      } else {
        const int maxVal = (1 << bpp) - 1;
        for (int x = 0; x < width; x++) {
          uint8_t sample = extractPackedSample(pPixels, x, bpp);
          if (useKey && sample == key) {
            grayLine[x] = 255;
          } else {
            grayLine[x] = (uint8_t)((sample * 255) / maxVal);
          }
        }
      }
      break;
    }

    case PNG_PIXEL_TRUECOLOR: {
      // tRNS for truecolor stores an 8-bit-per-channel RGB triplet to match exactly.
      const bool useKey = ctx.hasTrnsKey;
      const uint8_t kr = ctx.trnsR, kg = ctx.trnsG, kb = ctx.trnsB;
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 3];
        if (useKey && p[0] == kr && p[1] == kg && p[2] == kb) {
          grayLine[x] = 255;
        } else {
          grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        }
      }
      break;
    }

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = extractPackedSample(pPixels, x, bpp);
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t idx = extractPackedSample(pPixels, x, bpp);
            uint8_t* p = &palette[idx * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        // Indexed PNG without palette is malformed (PNGdec should always populate it).
        // Fill white so downstream dithering produces a clean blank rather than treating
        // raw bit-packed data as gray values.
        memset(grayLine, 255, width);
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  // Calculate destination Y with scaling
  int dstY = (int)(srcY * ctx->scale);

  // Skip if we already rendered this destination row (multiple source rows map to same dest)
  if (dstY == ctx->lastDstY) return 1;
  ctx->lastDstY = dstY;

  // Check bounds
  if (dstY >= ctx->dstHeight) return 1;

  int outY = ctx->config->y + dstY;
  if (outY >= ctx->screenHeight) return 1;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(*ctx, pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->iBpp,
                    pDraw->pPalette, pDraw->iHasAlpha);

  // Render scaled row using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool caching = ctx->caching;

  // Pre-compute orientation and render-mode state once per row
  DirectPixelWriter pw;
  pw.init(*ctx->renderer);
  pw.beginRow(outY);

  DirectCacheWriter cw;
  if (caching) {
    cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.originX);
    cw.beginRow(outY, ctx->config->y);
  }

  prepareOneBitDitherRow(*ctx, dstY);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  prepareDitherRow(*ctx, dstY);
#endif

  int srcX = 0;
  int error = 0;

  for (int dstX = 0; dstX < dstWidth; dstX++) {
    int outX = outXBase + dstX;

    // Always run dithering — error-diffusion ditherers carry per-column state across
    // rows, and skipping pixels that fall off the right edge would leave stale error
    // values that bleed into the next row's edge pixels. Only the framebuffer/cache
    // writes are guarded by the screen-bounds check.
    uint8_t gray = ctx->grayLineBuffer[srcX];
    uint8_t ditheredGray = ditherGray(*ctx, gray, dstX, outX, outY);

    if (outX >= 0 && outX < screenWidth) {
      pw.writePixel(outX, ditheredGray);
      if (caching) cw.writePixel(outX, ditheredGray);
    }

    // Bresenham-style stepping: advance srcX based on ratio srcWidth/dstWidth
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }

  return 1;
}

}  // namespace

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  // PNG file layout: 8-byte signature, then chunks. The IHDR chunk is mandatory and
  // must be the first chunk: 4 bytes length + "IHDR" + 13 bytes IHDR data + 4 bytes CRC.
  // Width and height live at bytes 16..23 (big-endian uint32s) of the file.
  // Reading those bytes directly avoids allocating PNGdec's ~42 KB working buffers
  // — important on the C3 where dimension queries can run while decode buffers from
  // a previous image are still pinned. We deliberately don't validate the CRC here;
  // a corrupt IHDR will surface during the actual decode.
  FsFile f;
  if (!Storage.openFileForRead("PNG", imagePath, f)) {
    LOG_ERR("PNG", "Failed to open file for dimensions: %s", imagePath.c_str());
    return false;
  }

  uint8_t hdr[24];
  int n = f.read(hdr, sizeof(hdr));
  f.close();
  if (n < (int)sizeof(hdr)) {
    LOG_ERR("PNG", "Short read on PNG header: %s", imagePath.c_str());
    return false;
  }

  // Validate PNG signature: 89 50 4E 47 0D 0A 1A 0A
  static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(hdr, kPngSig, 8) != 0) {
    LOG_ERR("PNG", "Not a PNG file: %s", imagePath.c_str());
    return false;
  }
  // Chunk type at hdr[12..15] must be "IHDR"
  if (hdr[12] != 'I' || hdr[13] != 'H' || hdr[14] != 'D' || hdr[15] != 'R') {
    LOG_ERR("PNG", "First chunk not IHDR: %s", imagePath.c_str());
    return false;
  }

  uint32_t width = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) | ((uint32_t)hdr[18] << 8) | (uint32_t)hdr[19];
  uint32_t height =
      ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) | ((uint32_t)hdr[22] << 8) | (uint32_t)hdr[23];
  if (width == 0 || height == 0 || width > 0x7FFF || height > 0x7FFF) {
    LOG_ERR("PNG", "Implausible PNG dimensions %ux%u: %s", width, height, imagePath.c_str());
    return false;
  }

  out.width = (int16_t)width;
  out.height = (int16_t)height;
  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    return false;
  }

  // Heap-allocate PNG decoder (~42 KB) - freed at end of function
  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    return false;
  }

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    return false;
  }

  if (!validateImageDimensions(png->getWidth(), png->getHeight(), "PNG")) {
    png->close();
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = png->getWidth();
  ctx.srcHeight = png->getHeight();

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking

  // PNGdec's getBpp() actually returns per-channel bit depth (1/2/4/8), not bits-per-pixel.
  // Capture both type and depth here so we can size buffers and validate up front.
  const int pixelType = png->getPixelType();
  const int channelDepth = png->getBpp();

  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), pixelType=%d channelDepth=%d", ctx.srcWidth, ctx.srcHeight,
          ctx.dstWidth, ctx.dstHeight, ctx.scale, pixelType, channelDepth);

  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType, channelDepth);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR("PNG",
            "PNG row buffer too small: need %d bytes for width=%d type=%d, configured PNG_MAX_BUFFERED_PIXELS=%d",
            requiredInternal, ctx.srcWidth, pixelType, PNG_MAX_BUFFERED_PIXELS);
    LOG_ERR("PNG", "Aborting decode to avoid PNGdec internal buffer overflow");
    png->close();
    return false;
  }

  // Validate per-channel bit depth for the pixel type. Grayscale/indexed allow 1/2/4/8
  // (all unpacked by convertLineToGray); the alpha and truecolor variants only allow 8
  // in practice — PNGdec rejects 16-bit at open() — so anything else here is a surprise.
  if (channelDepth != 8 && pixelType != PNG_PIXEL_GRAYSCALE && pixelType != PNG_PIXEL_INDEXED) {
    warnUnsupportedFeature("bit depth (" + std::to_string(channelDepth) + " bits/channel)", imagePath);
  }

  // Capture tRNS color-key for grayscale / truecolor PNGs. PNGdec sets iHasAlpha=1 on
  // those types when a tRNS chunk is present and stores the transparent value via
  // getTransparentColor(). Indexed PNGs use the per-entry alpha array at palette[768..]
  // and don't need the color-key path.
  if (png->hasAlpha() && pixelType != PNG_PIXEL_INDEXED &&
      (pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_TRUECOLOR)) {
    uint32_t trns = png->getTransparentColor();
    ctx.hasTrnsKey = true;
    if (pixelType == PNG_PIXEL_GRAYSCALE) {
      // PNGdec stores the lower byte of the 2-byte tRNS sample. For 1/2/4 bpp grayscale
      // this matches the bit-packed sample value we extract during line conversion.
      ctx.trnsGray = (uint8_t)(trns & 0xFF);
    } else {
      // Truecolor: R<<16 | G<<8 | B (each component is the lower byte of a 2-byte sample).
      ctx.trnsR = (uint8_t)((trns >> 16) & 0xFF);
      ctx.trnsG = (uint8_t)((trns >> 8) & 0xFF);
      ctx.trnsB = (uint8_t)(trns & 0xFF);
    }
  }

  // Allocate grayscale line buffer on demand (~3.2 KB) - freed after decode
  const size_t grayBufSize = PNG_MAX_BUFFERED_PIXELS / 2;
  ctx.grayLineBuffer = static_cast<uint8_t*>(malloc(grayBufSize));
  if (!ctx.grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    png->close();
    return false;
  }

  // Allocate cache buffer using SCALED dimensions.
  // PNG decode is fast enough (~135ms for 400x600) that caching provides minimal benefit
  // for larger images, while the cache buffer competes with the 44KB PNG decoder for heap.
  // Skip caching when the buffer would exceed the framebuffer size (48KB).
  static constexpr size_t PNG_MAX_CACHE_BYTES = 48000;
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    size_t cacheSize = (size_t)((ctx.dstWidth + 3) / 4) * ctx.dstHeight;
    if (cacheSize > PNG_MAX_CACHE_BYTES) {
      LOG_DBG("PNG", "Skipping cache: %zu bytes exceeds PNG limit (%zu)", cacheSize, PNG_MAX_CACHE_BYTES);
      ctx.caching = false;
    } else if (!ctx.cache.allocate(ctx.dstWidth, ctx.dstHeight, config.x, config.y)) {
      LOG_ERR("PNG", "Failed to allocate cache buffer, continuing without caching");
      ctx.caching = false;
    }
  }

  // When the caller explicitly requests monochrome output, use a 1-bit Atkinson
  // ditherer instead of the 4-level paths below. The 4-level dither produces
  // values 1-2 for mid grays, which DirectPixelWriter then collapses to black
  // under its `< 3` BW rule, making images render very dark in BW-only mode.
  // The 1-bit ditherer emits only 0 or 3 so the BW writer maps cleanly to
  // black/white.
  if (config.monochromeOutput) {
    ctx.atkinson1BitDitherer.reset(new (std::nothrow) Atkinson1BitDitherer(ctx.dstWidth));
    if (!ctx.atkinson1BitDitherer) {
      LOG_ERR("PNG", "Failed to allocate 1-bit Atkinson ditherer, falling back to 4-level dither");
    }
  }

  if (config.useDithering && !ctx.atkinson1BitDitherer) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    switch (config.ditherMode) {
      case ImageDitherMode::Atkinson:
        ctx.atkinsonDitherer.reset(new (std::nothrow) AtkinsonDitherer(ctx.dstWidth));
        if (!ctx.atkinsonDitherer) {
          LOG_ERR("PNG", "Failed to allocate Atkinson ditherer, falling back to Bayer");
        }
        break;
      case ImageDitherMode::DiffusedBayer:
        ctx.diffusedBayerDitherer.reset(new (std::nothrow) DiffusedBayerDitherer(ctx.dstWidth));
        if (!ctx.diffusedBayerDitherer) {
          LOG_ERR("PNG", "Failed to allocate diffused Bayer ditherer, falling back to Bayer");
        }
        break;
      case ImageDitherMode::Bayer:
      case ImageDitherMode::COUNT:
      default:
        break;
    }
#endif
  }

  unsigned long decodeStart = millis();
  rc = png->decode(&ctx, 0);
  unsigned long decodeTime = millis() - decodeStart;

  free(ctx.grayLineBuffer);
  ctx.grayLineBuffer = nullptr;

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Decode failed: %d", rc);
    png->close();
    return false;
  }

  png->close();
  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);

  // Write cache file if caching was enabled and buffer was allocated
  if (ctx.caching) {
    ctx.cache.writeToFile(config.cachePath);
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
