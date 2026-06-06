#include "ImageBlock.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include "../../../../src/fontIds.h"
#include "../../Epub.h"
#include "../converters/DirectPixelWriter.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/JpegToFramebufferConverter.h"
#include "../converters/PngToFramebufferConverter.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height, const std::string& altText)
    : imagePath(imagePath), altText(altText), width(width), height(height) {}

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height, const std::string& altText,
                       const std::string& epubFilePath, const std::string& epubEntryPath)
    : imagePath(imagePath),
      altText(altText),
      width(width),
      height(height),
      epubFilePath_(epubFilePath),
      epubEntryPath_(epubEntryPath) {}

bool ImageBlock::ensureExtracted() const {
  if (Storage.exists(imagePath.c_str())) return true;
  if (epubFilePath_.empty() || epubEntryPath_.empty()) {
    LOG_ERR("IMG", "Image missing and no EPUB source: %s", imagePath.c_str());
    return false;
  }
  LOG_DBG("IMG", "Lazy-extracting image: %s -> %s", epubEntryPath_.c_str(), imagePath.c_str());
  Epub epub(epubFilePath_, "/.crosspoint");
  if (!epub.extractItemToFile(epubEntryPath_, imagePath)) {
    LOG_ERR("IMG", "Lazy extraction failed: %s", epubEntryPath_.c_str());
    return false;
  }
  LOG_DBG("IMG", "Lazy extraction done: %s", imagePath.c_str());
  return true;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

// BW-plane cache: 1-bit Atkinson, only values 0/3, for AA-off rendering.
std::string getBwCachePath(const std::string& imagePath) {
  size_t dot = imagePath.rfind('.');
  if (dot != std::string::npos) return imagePath.substr(0, dot) + ".1bit.pxc";
  return imagePath + ".1bit.pxc";
}

// Grayscale cache: 4-level Bayer (0–3), replayed in GRAYSCALE_LSB/MSB passes when AA is on.
std::string getGrayscaleCachePath(const std::string& imagePath) {
  size_t dot = imagePath.rfind('.');
  if (dot != std::string::npos) return imagePath.substr(0, dot) + ".bayer.pxc";
  return imagePath + ".bayer.pxc";
}

// srcYOffset: first source row to render (0 = top of image).
// srcHeight:  number of rows to render (0 = full image from srcYOffset).
bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, int srcYOffset = 0, int srcHeight = 0) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }

  // Verify width is close (allow 1 pixel tolerance for rounding differences).
  // Height tolerance is widened to allow cropped renders (srcHeight < cachedHeight).
  if (abs(cachedWidth - expectedWidth) > 1) {
    LOG_ERR("IMG", "Cache width mismatch: %d vs %d", cachedWidth, expectedWidth);
    cacheFile.close();
    return false;
  }

  // Resolve crop window against actual cached dimensions
  if (srcYOffset < 0) srcYOffset = 0;
  if (srcYOffset >= static_cast<int>(cachedHeight)) {
    cacheFile.close();
    return false;
  }
  // Cap rows by both the cache height and the caller's expected height to prevent
  // overrunning the framebuffer when a 1-pixel cache rounding difference occurs.
  const int maxRows = std::min(static_cast<int>(cachedHeight) - srcYOffset, expectedHeight - srcYOffset);
  const int rowsToRender = (srcHeight > 0) ? std::min(srcHeight, maxRows) : maxRows;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d) srcY=%d rows=%d", cachePath.c_str(), cachedWidth, cachedHeight,
          srcYOffset, rowsToRender);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // Seek directly to the first row of interest — no need to iterate skipped rows.
  // Cache layout: 4-byte header (width + height) followed by rows in order.
  if (srcYOffset > 0) {
    const uint32_t seekPos = 4u + static_cast<uint32_t>(srcYOffset) * static_cast<uint32_t>(bytesPerRow);
    if (!cacheFile.seekSet(seekPos)) {
      LOG_ERR("IMG", "Cache seek failed to row %d", srcYOffset);
      cacheFile.close();
      return false;
    }
  }

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // hundreds of tiny reads through the storage mutex + SdFat each time — the
  // dominant cost of displaying an image page. Batching rows into a ~4KB buffer
  // cuts that down dramatically without holding the whole image.
  // (Ported from upstream commit d9bcef7a, crosspoint-reader#2230.)
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > rowsToRender) rowsPerRead = rowsToRender;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < rowsToRender; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (rowsToRender - row < rowsPerRead) ? (rowsToRender - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != bytes) {
        LOG_ERR("IMG", "Cache read error at row %d", srcYOffset + row);
        free(readBuffer);
        cacheFile.close();
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  cacheFile.close();
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::isLargeImage() const {
  // width and height are always set at construction from the ZIP manifest or
  // image header — no file read needed. The SD cache file may not exist yet
  // (lazy extraction), so reading it here would produce spurious error logs.
  return int32_t(width) * int32_t(height) > LARGE_IMAGE_PIXEL_THRESHOLD;
}

bool ImageBlock::hasPixelCache() const { return Storage.exists(getBwCachePath(imagePath).c_str()); }

bool ImageBlock::hasGrayscaleCache() const { return Storage.exists(getGrayscaleCachePath(imagePath).c_str()); }

bool ImageBlock::wouldShowPlaceholder(bool forceLoad, bool monochromeOutput) const {
  if (forceLoad) return false;
  if (!isLargeImage()) return false;
  // Check only the cache variant that render() will actually use for this mode.
  const std::string& cachePath = monochromeOutput ? getBwCachePath(imagePath) : getGrayscaleCachePath(imagePath);
  return !Storage.exists(cachePath.c_str());
}

void ImageBlock::renderGrayscaleFromCache(GfxRenderer& renderer, const int x, const int y) const {
  renderFromCache(renderer, getGrayscaleCachePath(imagePath), x, y, width, height, srcYOffset_, srcHeight_);
}

std::unique_ptr<ImageBlock> ImageBlock::makeCrop(const int16_t srcYOffset, const int16_t srcHeight) const {
  auto crop =
      std::unique_ptr<ImageBlock>(new ImageBlock(imagePath, width, height, altText, epubFilePath_, epubEntryPath_));
  crop->srcYOffset_ = srcYOffset;
  crop->srcHeight_ = srcHeight;
  return crop;
}

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  constexpr int BORDER = 1;
  constexpr int PADDING = 6;

  renderer.drawRect(x, y, width, height, BORDER, true);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const bool hasAlt = !altText.empty();
  const int lineCount = hasAlt ? 3 : 2;
  const int totalTextH = lineH * lineCount;

  if (lineH > 0 && width > PADDING * 2 && height > totalTextH + PADDING * 2) {
    const int textX = x + PADDING;
    const int textY = y + (height - totalTextH) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, tr(STR_LARGE_IMAGE));
    if (hasAlt) {
      renderer.drawText(UI_10_FONT_ID, textX, textY + lineH, altText.c_str());
    }
    renderer.drawText(UI_10_FONT_ID, textX, textY + lineH * (lineCount - 1), tr(STR_PRESS_CONFIRM_TO_LOAD));
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y, const bool forceLoad,
                        const bool monochromeOutput) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  // (Ported from upstream commit d9bcef7a, crosspoint-reader#2230.)
  if (renderer.isFontCacheScanning()) return;

  const int renderedHeight = srcHeight_ > 0 ? srcHeight_ : height;
  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d) srcY=%d rendH=%d mono=%d", x, y, imagePath.c_str(), width,
          height, srcYOffset_, renderedHeight, monochromeOutput ? 1 : 0);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check against the rendered (cropped) height, not the full image height.
  if (x < 0 || y < 0 || x + width > screenWidth || y + renderedHeight > screenHeight) {
    LOG_ERR("IMG", "Render bounds rejected: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, renderedHeight,
            screenWidth, screenHeight);
    return;
  }

  // Select cache path based on rendering mode
  const std::string cachePath = monochromeOutput ? getBwCachePath(imagePath) : getGrayscaleCachePath(imagePath);

  // Try to render from pixel cache first (always, regardless of forceLoad)
  if (renderFromCache(renderer, cachePath, x, y, width, renderedHeight, srcYOffset_, srcHeight_)) {
    return;
  }

  // No pixel cache — check if this is a large image that should show a placeholder
  if (wouldShowPlaceholder(forceLoad, monochromeOutput)) {
    LOG_DBG("IMG", "Large image placeholder at %d,%d (%dx%d): %s", x, y, width, height, imagePath.c_str());
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Ensure the image is extracted to SD (lazy extraction if not already present).
  if (!ensureExtracted()) {
    LOG_ERR("IMG", "Image unavailable: %s", imagePath.c_str());
    return;
  }

  // Proceed with full decode
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found after extraction: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.monochromeOutput = monochromeOutput;
  config.performanceMode = false;
  config.useExactDimensions = true;
  config.cachePath = cachePath;

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
  }
}

bool ImageBlock::serialize(FsFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  serialization::writeString(file, altText);
  serialization::writeString(file, epubFilePath_);
  serialization::writeString(file, epubEntryPath_);
  serialization::writePod(file, srcYOffset_);
  serialization::writePod(file, srcHeight_);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  std::string alt, epubFile, epubEntry;
  serialization::readString(file, alt);
  serialization::readString(file, epubFile);
  serialization::readString(file, epubEntry);
  int16_t srcYOffset = 0, srcHeight = 0;
  serialization::readPod(file, srcYOffset);
  serialization::readPod(file, srcHeight);
  auto block = std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h, alt, epubFile, epubEntry));
  block->srcYOffset_ = srcYOffset;
  block->srcHeight_ = srcHeight;
  return block;
}
