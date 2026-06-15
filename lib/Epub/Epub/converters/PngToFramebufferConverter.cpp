#include "PngToFramebufferConverter.h"

#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PngStreamDecoder.h>
#include <esp_task_wdt.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

// PNG decode now runs on uzlib (PngStreamDecoder) instead of PNGdec. The PNGdec
// PNGIMAGE object was ~49.5 KB — larger than the 48 KB framebuffer the reader
// frees to make room for it — so `new PNG()` failed intermittently under heap
// fragmentation and images silently vanished. PngStreamDecoder needs only the
// DEFLATE window (≤32 KB, sized down for small images) plus a couple of scanline
// buffers, so it fits the freed framebuffer with margin.

namespace {

// Ditherer state, mirroring the modes the old PNGdec path supported:
//   monochromeOutput -> 1-bit Atkinson (reader images)
//   else             -> 4-level Bayer (sleep-screen grayscale planes), plus the
//                       optional error-diffusion ditherers behind the extension flag.
struct DitherState {
  const RenderConfig* config{nullptr};
  std::unique_ptr<Atkinson1BitDitherer> atkinson1Bit;
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  std::unique_ptr<AtkinsonDitherer> atkinson4;
  std::unique_ptr<DiffusedBayerDitherer> bayerDiff;
#endif
};

// Map one grayscale sample to a 2-bit value (0..3). Called for every destination
// column — including off-screen ones — so error-diffusion state stays consistent
// across the row; only the framebuffer/cache write is bounds-guarded by the caller.
uint8_t ditherGray(DitherState& d, uint8_t gray, int localX, int outX, int outY) {
  if (d.atkinson1Bit) return d.atkinson1Bit->processPixel(gray, localX) ? 3 : 0;
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  if (d.config->useDithering) {
    switch (d.config->ditherMode) {
      case ImageDitherMode::Atkinson:
        if (d.atkinson4) return d.atkinson4->processPixel(gray, localX);
        break;
      case ImageDitherMode::DiffusedBayer:
        if (d.bayerDiff) return d.bayerDiff->processPixel(gray, localX, outX, outY);
        break;
      default:
        break;
    }
  }
#endif
  return applyBayerDither4Level(gray, outX, outY);
}

void advanceDitherRow(DitherState& d) {
  if (d.atkinson1Bit) d.atkinson1Bit->nextRow();
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  if (d.atkinson4) d.atkinson4->nextRow();
  if (d.bayerDiff) d.bayerDiff->nextRow();
#endif
}

// Below this free heap we don't even start: the uzlib ring (≤32 KB) plus scanline
// buffers won't fit. begin() also fails gracefully if a specific malloc fails.
constexpr size_t PNG_DECODE_HEAP_FLOOR = 36 * 1024;

}  // namespace

bool PngToFramebufferConverter::getDimensionsFromBuffer(const uint8_t* buf, const size_t len, ImageDimensions& out) {
  if (!buf || len < 24) return false;
  static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(buf, kPngSig, 8) != 0) return false;
  if (buf[12] != 'I' || buf[13] != 'H' || buf[14] != 'D' || buf[15] != 'R') return false;
  const uint32_t w =
      ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16) | ((uint32_t)buf[18] << 8) | (uint32_t)buf[19];
  const uint32_t h =
      ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16) | ((uint32_t)buf[22] << 8) | (uint32_t)buf[23];
  if (w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF) return false;
  out.width = static_cast<int16_t>(w);
  out.height = static_cast<int16_t>(h);
  return true;
}

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  // PNG file layout: 8-byte signature, then chunks. The IHDR chunk is mandatory and
  // must be the first chunk: 4 bytes length + "IHDR" + 13 bytes IHDR data + 4 bytes CRC.
  // Width and height live at bytes 16..23 (big-endian uint32s) of the file. Reading
  // those bytes directly avoids allocating any decode buffers.
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

  static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(hdr, kPngSig, 8) != 0) {
    LOG_ERR("PNG", "Not a PNG file: %s", imagePath.c_str());
    return false;
  }
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

  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < PNG_DECODE_HEAP_FLOOR) {
    LOG_ERR("PNG", "Not enough heap for PNG decode (%u free, need %u)", (unsigned)freeHeap,
            (unsigned)PNG_DECODE_HEAP_FLOOR);
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("PNG", imagePath, file)) {
    LOG_ERR("PNG", "Failed to open PNG: %s", imagePath.c_str());
    return false;
  }

  auto decoder = std::unique_ptr<PngStreamDecoder>(new (std::nothrow) PngStreamDecoder());
  if (!decoder) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    file.close();
    return false;
  }
  PngStreamDecoder::Info info;
  if (!decoder->begin(file, info)) {
    LOG_ERR("PNG", "Failed to start PNG decode: %s", imagePath.c_str());
    file.close();
    return false;
  }

  const int srcWidth = static_cast<int>(info.width);
  const int srcHeight = static_cast<int>(info.height);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Output dimensions (same policy as the old decoder).
  int dstWidth, dstHeight;
  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    dstWidth = config.maxWidth;
    dstHeight = config.maxHeight;
  } else {
    const float scaleX = (float)config.maxWidth / srcWidth;
    const float scaleY = (float)config.maxHeight / srcHeight;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale > 1.0f) scale = 1.0f;  // never upscale
    dstWidth = (int)(srcWidth * scale);
    dstHeight = (int)(srcHeight * scale);
    if (dstWidth < 1) dstWidth = 1;
    if (dstHeight < 1) dstHeight = 1;
  }
  // Aspect ratio is preserved by the caller's sizing, so a single factor maps both axes.
  const float scale = (float)dstWidth / srcWidth;

  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), colorType=%d bitDepth=%d", srcWidth, srcHeight, dstWidth, dstHeight,
          scale, info.colorType, info.bitDepth);

  auto grayLine = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[srcWidth]);
  if (!grayLine) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    file.close();
    return false;
  }

  DitherState dither;
  dither.config = &config;
  if (config.monochromeOutput) {
    dither.atkinson1Bit.reset(new (std::nothrow) Atkinson1BitDitherer(dstWidth));
  }
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  else if (config.useDithering) {
    switch (config.ditherMode) {
      case ImageDitherMode::Atkinson:
        dither.atkinson4.reset(new (std::nothrow) AtkinsonDitherer(dstWidth));
        break;
      case ImageDitherMode::DiffusedBayer:
        dither.bayerDiff.reset(new (std::nothrow) DiffusedBayerDitherer(dstWidth));
        break;
      default:
        break;
    }
  }
#endif

  // Stream the 2-bit pixel cache to disk one row band at a time.
  PixelCache cache;
  bool caching = !config.cachePath.empty();
  if (caching && !cache.begin(config.cachePath, dstWidth, dstHeight, config.x, config.y, 1)) {
    LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
    caching = false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  bool ok = true;
  int lastDstY = -1;
  const unsigned long decodeStart = millis();

  for (int srcY = 0; srcY < srcHeight; srcY++) {
    if (!decoder->nextRow(grayLine.get())) {
      LOG_ERR("PNG", "Decode failed at row %d", srcY);
      ok = false;
      break;
    }
    // Feed the WDT periodically: a large image can take seconds to inflate.
    if ((srcY & 31) == 0) esp_task_wdt_reset();

    const int dstY = (int)(srcY * scale);
    if (dstY == lastDstY) continue;  // multiple source rows collapse to one dest row
    if (dstY >= dstHeight) break;
    lastDstY = dstY;

    const int outY = config.y + dstY;
    if (outY >= screenHeight) break;

    pw.beginRow(outY);

    DirectCacheWriter cw;
    bool rowCaching = caching;
    if (rowCaching) {
      if (!cache.advanceTo(dstY)) {
        caching = false;
        rowCaching = false;
      } else {
        cw.init(cache.buffer, cache.bytesPerRow, cache.originX, config.y + cache.bandStart, cache.width,
                cache.bandRows);
        cw.beginRow(outY);
      }
    }

    // Bresenham-style horizontal scaling: advance srcX by srcWidth/dstWidth per dst column.
    int srcX = 0;
    int error = 0;
    for (int dstX = 0; dstX < dstWidth; dstX++) {
      const int outX = config.x + dstX;
      const uint8_t value = ditherGray(dither, grayLine[srcX], dstX, outX, outY);
      if (outX >= 0 && outX < screenWidth) {
        pw.writePixel(outX, value);
        if (rowCaching) cw.writePixel(outX, value);
      }
      error += srcWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
    }
    advanceDitherRow(dither);
  }

  decoder->end();
  file.close();

  if (caching) {
    if (ok) {
      cache.finalize();
    } else {
      cache.abort();
    }
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", millis() - decodeStart);
  return ok;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
