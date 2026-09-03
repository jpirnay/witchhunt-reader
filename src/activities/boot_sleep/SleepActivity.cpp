#include "SleepActivity.h"

#include <Epub.h>
#include <Epub/Section.h>
#include <Epub/converters/DirectPixelWriter.h>
#include <Epub/converters/PixelCache.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <PngStreamDecoder.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#include "../reader/EpubReaderActivity.h"
#include "../reader/TxtReaderActivity.h"
#include "../reader/XtcReaderActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {

// Sidecar cache for a rendered sleep image: "/sleep/foo.png" -> "/sleep/.foo.png.f3.pxc".
//
// The three render passes below all decode the same source and produce the same
// 2-bit value per pixel -- the render mode only selects which bit-plane receives it
// (see DirectPixelWriter::writePixel). So one cache serves all three, and a sleep
// image is decoded once ever rather than three times per sleep (four with adaptive
// tone analysis).
//
// The filter id is part of the name because tone mapping is baked into the cached
// pixels. Keying by name rather than invalidating on change means switching filters
// back and forth reuses both caches instead of re-decoding each time, and there is
// no hook to miss when the setting is edited. Stale entries for filters the user
// abandoned are a few KB and are overwritten if that filter is chosen again.
//
// The leading dot keeps it out of collectSleepImages(), which skips dotfiles, so a
// cache can never be picked as a sleep image itself.
std::string sleepPixelCachePath(const std::string& imagePath, uint8_t filter) {
  const size_t slash = imagePath.find_last_of('/');
  if (slash == std::string::npos) return "";
  return imagePath.substr(0, slash + 1) + "." + imagePath.substr(slash + 1) + ".f" + std::to_string(filter) + ".pxc";
}

// Replays a cached 2-bit image into the framebuffer under the current render mode.
// Returns false if the cache is missing, stale, or does not match the expected
// geometry, in which case the caller decodes normally. A version or size mismatch
// deletes the file so the next decode rewrites it.
bool renderSleepImageFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                               int expectedHeight) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("SLP", cachePath, cacheFile)) return false;

  uint16_t magic = 0;
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  if (cacheFile.read(&magic, 2) != 2 || cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }
  if (magic != PixelCache::PXC_MAGIC) {
    cacheFile.close();
    LOG_INF("SLP", "Stale sleep pixel cache (0x%04X), deleting: %s", magic, cachePath.c_str());
    Storage.remove(cachePath.c_str());
    return false;
  }
  // Geometry is derived from the panel and the source image, so a mismatch means the
  // image or the display orientation changed under a stale cache.
  if (cachedWidth != expectedWidth || cachedHeight != expectedHeight) {
    cacheFile.close();
    LOG_INF("SLP", "Sleep pixel cache geometry changed (%ux%u vs %dx%d), deleting: %s", cachedWidth, cachedHeight,
            expectedWidth, expectedHeight, cachePath.c_str());
    Storage.remove(cachePath.c_str());
    return false;
  }

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, MSB first
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;

  auto readBuffer = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowsPerRead) * bytesPerRow);
  if (!readBuffer) {
    rowsPerRead = 1;
    readBuffer = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  }
  if (!readBuffer) {
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = static_cast<size_t>(toRead) * bytesPerRow;
      const int bytesRead = cacheFile.read(readBuffer.get(), bytes);
      if (bytesRead < 0 || static_cast<size_t>(bytesRead) != bytes) {
        LOG_ERR("SLP", "Sleep pixel cache read error at row %d", row);
        cacheFile.close();
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    // cppcheck-suppress arithOperationsOnVoidPointer ; get() is uint8_t*, cppcheck
    // mis-deduces the concept-constrained makeUniqueNoThrow<uint8_t[]> return type
    const uint8_t* rowBuffer = readBuffer.get() + static_cast<size_t>(bufferRow) * bytesPerRow;
    bufferRow++;

    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const uint8_t packed = rowBuffer[col >> 2];
      const uint8_t value = (packed >> (6 - (col & 3) * 2)) & 0x03;
      pw.writePixel(x + col, value);
    }
  }

  cacheFile.close();
  return true;
}

// Adaptive tone mapping applies to user-supplied sleep images only. The overlay
// compositing path deliberately does not use it: it draws onto an already-rendered
// screen, where a per-image level stretch would fight the image underneath.
BitmapToneMapping sleepImageToneMapping() {
  switch (SETTINGS.sleepScreenCoverFilter) {
    case CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::ADAPTIVE_TONE:
      return BitmapToneMapping::Adaptive;
    case CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::EQUALIZE_TONE:
      return BitmapToneMapping::Equalize;
    default:
      return BitmapToneMapping::None;
  }
}

// Equalization strength for a sleep image, chosen by how the image will actually
// reach the panel. The conservative default exists because a four-level target
// breaks a flattened gradient into dither banding; a target that resolves more
// has the range to use a stronger curve, and measurably wants one.
int sleepEqualizeBlend(const GfxRenderer& renderer) {
  return renderer.getGrayLevels() > 4 ? adaptive_tone::EQ_BLEND_NUM_DEEP : adaptive_tone::EQ_BLEND_NUM;
}

bool renderPngSleepScreen(const std::string& filename, GfxRenderer& renderer, const BookOverlayInfo& overlayInfo) {
  constexpr size_t MIN_FREE_HEAP = 60 * 1024;  // PNG decoder ~42 KB + overhead
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("SLP", "Not enough heap for PNG sleep image: %s", filename.c_str());
    return false;
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  RenderConfig config;
  config.x = 0;
  config.y = 0;
  config.maxWidth = pageWidth;
  config.maxHeight = pageHeight;
  config.useGrayscale = true;
  config.useDithering = true;
  config.ditherMode = ImageDitherMode::Bayer;
  config.performanceMode = false;
  config.useExactDimensions = false;

  // A panel that resolves more than four levels renders this image natively below,
  // in one decode with no plane encoding. The .pxc cache is 2 bits per pixel by
  // construction, so it has nothing to offer that path and is not consulted for it.
  const bool nativeGray = renderer.getGrayLevels() > 4;

  // Mirror the decoder's own output sizing so the cache can be validated against the
  // geometry a fresh decode would produce (see PngToFramebufferConverter: fit to the
  // config box, never upscale).
  const std::string cachePath =
      nativeGray ? std::string() : sleepPixelCachePath(filename, SETTINGS.sleepScreenCoverFilter);
  int cacheW = 0, cacheH = 0, cacheX = 0, cacheY = 0;
  ImageDimensions srcDims{};
  if (!cachePath.empty() && PngToFramebufferConverter::getDimensionsStatic(filename, srcDims) && srcDims.width > 0 &&
      srcDims.height > 0) {
    const float scaleX = static_cast<float>(pageWidth) / srcDims.width;
    const float scaleY = static_cast<float>(pageHeight) / srcDims.height;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale > 1.0f) scale = 1.0f;
    cacheW = std::max(1, static_cast<int>(srcDims.width * scale));
    cacheH = std::max(1, static_cast<int>(srcDims.height * scale));
    // The decoder draws at config.x/config.y (0,0 here) and caches with that same
    // origin, so the replay must not re-centre or the image would shift.
    cacheX = config.x;
    cacheY = config.y;
  }

  // A cached image is already toned and dithered, so neither the analysis pass nor a
  // decode is needed. Probe with the BW pass; if it hits, the other two replay too.
  bool useCache = false;
  if (cacheW > 0) {
    renderer.setRenderMode(GfxRenderer::BW);
    useCache = renderSleepImageFromCache(renderer, cachePath, cacheX, cacheY, cacheW, cacheH);
  }

  // Derived once and shared by all three passes below: a PNG cannot be rewound, so
  // this costs an extra decode, and re-deriving it per pass would triple that. All
  // passes must use the same points anyway, or the BW carrier would disagree with
  // the grey planes layered on top. Skipped entirely on a cache hit.
  const BitmapToneMapping pngToneMapping = sleepImageToneMapping();
  if (!useCache && pngToneMapping != BitmapToneMapping::None) {
    config.adaptiveTone = PngToFramebufferConverter::analyzeAdaptiveTone(filename, toneAnalysisMode(pngToneMapping),
                                                                         sleepEqualizeBlend(renderer));
  }
  // Write the cache on the decoding pass only; the filter is already part of the
  // cache filename, so no separate invalidation is needed.
  if (!useCache && cacheW > 0) config.cachePath = cachePath;

  // Overlay drawing is shared across all three rendering passes (BW + LSB + MSB) so the
  // text appears on every plane. Captured by reference so the lambda sees the renderer.
  const auto drawOverlay = [&]() {
    if (overlayInfo.progressText.empty()) {
      return;
    }
    const int lineHeight12 = renderer.getLineHeight(BOOKERLY_12_FONT_ID);
    const int lineHeight10 = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int lineSpacing = 3;
    constexpr int sectionSpacing = 10;
    const int maxTextWidth = pageWidth - 20;

    int textBlockHeight = 0;
    if (!overlayInfo.title.empty()) {
      textBlockHeight += lineHeight12;
      if (!overlayInfo.author.empty()) {
        textBlockHeight += lineSpacing;
      } else if (!overlayInfo.progressText.empty()) {
        textBlockHeight += sectionSpacing;
      }
    }
    if (!overlayInfo.author.empty()) {
      textBlockHeight += lineHeight10;
      if (!overlayInfo.progressText.empty()) {
        textBlockHeight += sectionSpacing;
      }
    }
    if (!overlayInfo.progressText.empty()) {
      textBlockHeight += lineHeight10;
    }

    const int overlayY = pageHeight - textBlockHeight - (lineHeight12 / 3) - (lineHeight10 * 2 / 3);
    int y = overlayY + (lineHeight12 / 3);
    if (!overlayInfo.title.empty()) {
      const std::string title = renderer.truncatedText(BOOKERLY_12_FONT_ID, overlayInfo.title.c_str(), maxTextWidth);
      renderer.drawText(BOOKERLY_12_FONT_ID, 10, y, title.c_str(), true);
      y += lineHeight12;
      if (!overlayInfo.author.empty()) {
        y += lineSpacing;
      } else if (!overlayInfo.progressText.empty()) {
        y += sectionSpacing;
      }
    }
    if (!overlayInfo.author.empty()) {
      const std::string author = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.author.c_str(), maxTextWidth);
      renderer.drawText(UI_10_FONT_ID, 10, y, author.c_str(), true);
      y += lineHeight10;
      if (!overlayInfo.progressText.empty()) {
        y += sectionSpacing;
      }
    }
    if (!overlayInfo.progressText.empty()) {
      const std::string progress =
          renderer.truncatedText(UI_10_FONT_ID, overlayInfo.progressText.c_str(), maxTextWidth);
      renderer.drawText(UI_10_FONT_ID, 10, y, progress.c_str(), true);
    }
  };

  PngToFramebufferConverter decoder;

  // Native grayscale: one decode, one push, every level the panel has.
  //
  // The three passes below exist only to fill three 1-bit planes. Here the panel
  // keeps its own deeper buffer, so the decoder's tone-mapped sample is stored
  // whole and quantised once, by the panel, at its native depth -- no Bayer
  // dither to four levels on the way out, and no BW carrier for greys to be
  // layered onto. Falling through costs nothing: every ordinary refresh rebuilds
  // the canvas from the framebuffer, so a half-painted canvas cannot outlive the
  // failure that abandoned it.
  if (nativeGray) {
    uint16_t canvasStride = 0;
    if (uint8_t* canvas = renderer.borrowGray8Canvas(&canvasStride)) {
      // White ground: the image is fitted to the box, not stretched to it, so
      // whatever it does not cover has to be something rather than stale pixels.
      for (int row = 0; row < renderer.getDisplayHeight(); row++) {
        memset(canvas + static_cast<uint32_t>(row) * canvasStride, 0xFF, renderer.getDisplayWidth());
      }

      DirectGray8Writer gray8;
      gray8.init(renderer, canvas, canvasStride);
      RenderConfig nativeConfig = config;
      nativeConfig.gray8 = &gray8;
      nativeConfig.cachePath.clear();
      nativeConfig.companionCachePath.clear();

      renderer.setRenderMode(GfxRenderer::BW);
      if (decoder.decodeToFramebuffer(filename, renderer, nativeConfig)) {
        // The overlay is 1-bit text and wants to stay that way: drawn through the
        // normal render path into a cleared framebuffer, then stamped onto the
        // canvas as solid black. Dithering it with the image would only soften it.
        renderer.clearScreen();
        drawOverlay();
        renderer.stampBwOntoGray8Canvas(canvas, canvasStride);
        renderer.displayGray8Canvas();
        LOG_INF("SLP", "Sleep image rendered at %u levels: %s", static_cast<unsigned>(renderer.getGrayLevels()),
                filename.c_str());
        return true;
      }
      LOG_DBG("SLP", "Native grayscale sleep decode failed, falling back: %s", filename.c_str());
    }
  }

  // Pass 1: BW plane — mirrors SleepActivity::renderBitmapSleepScreen so the BW carrier
  // matches the 4-level quantization layered on top via the LSB/MSB planes.
  // On a cache hit the pixels were already replayed above by the probe.
  renderer.setRenderMode(GfxRenderer::BW);
  if (!useCache) {
    renderer.clearScreen();
    if (!decoder.decodeToFramebuffer(filename, renderer, config)) {
      LOG_DBG("SLP", "PNG sleep image decode failed: %s", filename.c_str());
      return false;
    }
    // The decode just wrote the cache, so the remaining passes replay instead of
    // decoding again.
    useCache = !config.cachePath.empty() && Storage.exists(cachePath.c_str());
  }
  drawOverlay();
  // Fire the BW scrub without waiting: the waveform runs on the controller's own RAM,
  // so the LSB decode below (CPU/SD-only work) overlaps it. copyGrayscaleLsbBuffers()
  // drains the pending finish before its SPI plane write.
  renderer.triggerDisplayAsync(HalDisplay::HALF_REFRESH);

  // Passes 2 and 3 replay the cache when one is available; the render mode selects
  // which bit-plane each cached 2-bit value lands in, so no re-decode is needed.
  const auto renderPass = [&](const char* planeName) -> bool {
    if (useCache && renderSleepImageFromCache(renderer, cachePath, cacheX, cacheY, cacheW, cacheH)) return true;
    if (!decoder.decodeToFramebuffer(filename, renderer, config)) {
      LOG_DBG("SLP", "PNG sleep image %s decode failed: %s", planeName, filename.c_str());
      return false;
    }
    return true;
  };

  // Pass 2: GRAYSCALE_LSB plane, rendered while the BW waveform is still running.
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!renderPass("LSB")) {
    renderer.setRenderMode(GfxRenderer::BW);
    return false;
  }
  drawOverlay();
  renderer.copyGrayscaleLsbBuffers();

  // Pass 3: GRAYSCALE_MSB plane.
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!renderPass("MSB")) {
    renderer.setRenderMode(GfxRenderer::BW);
    return false;
  }
  drawOverlay();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

// Collects full paths of valid image files from /.sleep and /sleep, with no preference between
// the two directories. BMP files are validated by parsing their headers; invalid BMPs are skipped.
// When allowPng is true, .png files are also accepted (PNG validation happens later at decode time).
std::vector<std::string> collectSleepImages(bool allowPng) {
  std::vector<std::string> files;
  for (const char* sleepDir : {"/.sleep", "/sleep"}) {
    auto dir = Storage.open(sleepDir);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    char name[500];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }
      const bool isBmp = FsHelpers::hasBmpExtension(filename);
      const bool isPng = allowPng && FsHelpers::hasPngExtension(filename);
      if (!isBmp && !isPng) {
        file.close();
        continue;
      }
      if (isBmp) {
        Bitmap bmp(file);
        if (bmp.parseHeaders() != BmpReaderError::Ok) {
          LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
          file.close();
          continue;
        }
      }
      files.emplace_back(std::string(sleepDir) + "/" + filename);
      file.close();
    }
    dir.close();
  }
  // Sort by full path so the order is deterministic across reboots — required for sequential
  // pick mode, harmless for random pick mode.
  std::sort(files.begin(), files.end());
  return files;
}

// Picks the next file index based on the user's pick mode.
// RANDOM: uniform random with single reroll to avoid immediate repeats.
// SEQUENTIAL: advances from APP_STATE.lastSleepImage, wrapping at numFiles.
size_t pickSleepImageIndex(size_t numFiles) {
  if (SETTINGS.sleepImagePickMode == CrossPointSettings::SLEEP_IMAGE_PICK_MODE::PICK_SEQUENTIAL) {
    const size_t last = APP_STATE.lastSleepImage;
    if (last == SIZE_MAX || last >= numFiles) return 0;
    return (last + 1) % numFiles;
  }
  size_t idx = static_cast<size_t>(esp_random() % numFiles);
  while (numFiles > 1 && APP_STATE.lastSleepImage != SIZE_MAX && idx == APP_STATE.lastSleepImage) {
    idx = static_cast<size_t>(esp_random() % numFiles);
  }
  return idx;
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);

  // Quick Resume: paint a moon icon over the current page and keep the framebuffer
  // intact for the next wake. Applies always when the user picked Quick Resume as
  // sleep screen, or only on timeout sleeps when "Quick Resume on Timeout" is on.
  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  // No "Entering sleep..." popup here: it shipped a full extra refresh (~500 ms on X3)
  // before the sleep screen's own refresh; the sleep screen appearing is the feedback.
  // The renderers below all expect portrait: the cover BMP is generated portrait-sized
  // (getDisplayHeight x getDisplayWidth) and the custom/default screens are laid out
  // portrait. A timeout sleep bypasses the reader's onExit() orientation reset, so force
  // portrait here or a landscape cover overflows the edges / mis-centers. OVERLAY manages
  // its own orientation (renderOverlaySleepScreen), so leave it untouched here.
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY) {
    renderer.setOrientation(GfxRenderer::Portrait);
  }
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (!APP_STATE.openEpubPath.empty()) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY):
      return renderOverlaySleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  const BookOverlayInfo overlayInfo{};
  const bool shouldLoadOverlayInfo =
      SETTINGS.sleepCoverOverlay != 0 && APP_STATE.lastSleepFromReader && !APP_STATE.openEpubPath.empty();

  // An explicitly selected custom sleep image should override random images from /.sleep or /sleep.
  FsFile explicitSleepFile;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", explicitSleepFile)) {
    Bitmap bitmap(explicitSleepFile, true, sleepImageToneMapping(), sleepEqualizeBlend(renderer));
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading explicit custom sleep image: /sleep.bmp");
      const BookOverlayInfo resolvedOverlayInfo =
          shouldLoadOverlayInfo ? getBookOverlayInfo(APP_STATE.openEpubPath) : overlayInfo;
      renderBitmapSleepScreen(bitmap, resolvedOverlayInfo);
      explicitSleepFile.close();
      return;
    }
    explicitSleepFile.close();
  }
  if (Storage.openFileForRead("SLP", "/sleep.png", explicitSleepFile)) {
    explicitSleepFile.close();
    const BookOverlayInfo resolvedOverlayInfo =
        shouldLoadOverlayInfo ? getBookOverlayInfo(APP_STATE.openEpubPath) : overlayInfo;
    LOG_DBG("SLP", "Loading explicit custom sleep image: /sleep.png");
    if (renderPngSleepScreen("/sleep.png", renderer, resolvedOverlayInfo)) {
      return;
    }
  }

  // Collect valid BMP and PNG files from both /.sleep and /sleep directories (no preference between them)
  const auto files = collectSleepImages(/*allowPng=*/true);
  const auto numFiles = files.size();
  if (numFiles > 0) {
    const auto pickedIndex = pickSleepImageIndex(numFiles);
    APP_STATE.lastSleepImage = pickedIndex;
    APP_STATE.saveToFile();
    const auto& filename = files[pickedIndex];
    LOG_DBG("SLP", "Loading sleep image: %s", filename.c_str());
    const BookOverlayInfo resolvedOverlayInfo =
        shouldLoadOverlayInfo ? getBookOverlayInfo(APP_STATE.openEpubPath) : overlayInfo;
    if (FsHelpers::hasPngExtension(filename)) {
      if (renderPngSleepScreen(filename, renderer, resolvedOverlayInfo)) {
        return;
      }
    } else {
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        delay(100);
        Bitmap bitmap(file, true, sleepImageToneMapping(), sleepEqualizeBlend(renderer));
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap, resolvedOverlayInfo);
          file.close();
          return;
        }
        file.close();
      }
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

BookOverlayInfo SleepActivity::getBookOverlayInfo(const std::string& bookPath) const {
  BookOverlayInfo info;

  if (FsHelpers::checkFileExtension(bookPath, ".xtc") || FsHelpers::checkFileExtension(bookPath, ".xtch")) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (xtc.load()) {
      info.title = xtc.getTitle();
      info.author = xtc.getAuthor();

      FsFile f;
      if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                                 (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
          uint32_t totalPages = xtc.getPageCount();
          float progress = xtc.calculateProgress(currentPage) * 100.0f;
          char buf[64];
          snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1, totalPages,
                   progress);
          info.progressText = buf;
        }
        f.close();
      }
    }
  } else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    if (txt.load()) {
      info.title = txt.getTitle();

      FsFile f;
      if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          uint32_t currentPage = data[0] + (data[1] << 8);

          uint32_t totalPages = 0;
          FsFile indexFile;
          if (Storage.openFileForRead("SLP", txt.getCachePath() + "/index.bin", indexFile)) {
            uint32_t magic;
            serialization::readPod(indexFile, magic);
            uint8_t version;
            serialization::readPod(indexFile, version);
            static constexpr uint32_t INDEX_CACHE_MAGIC = 0x54585449;  // "TXTI"
            static constexpr uint8_t INDEX_CACHE_VERSION = 2;
            if (magic == INDEX_CACHE_MAGIC && version == INDEX_CACHE_VERSION) {
              indexFile.seek(32);
              serialization::readPod(indexFile, totalPages);
            }
            indexFile.close();
          }

          if (totalPages > 0) {
            float progress = (currentPage + 1) * 100.0f / totalPages;
            char buf[64];
            snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1, totalPages,
                     progress);
            info.progressText = buf;
          } else {
            char buf[64];
            snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS_NO_TOTAL), (unsigned long)currentPage + 1);
            info.progressText = buf;
          }
        }
        f.close();
      }
    }
  } else if (FsHelpers::checkFileExtension(bookPath, ".epub")) {
    Epub epub(bookPath, "/.crosspoint");
    if (epub.load(true, true)) {
      info.title = epub.getTitle();
      info.author = epub.getAuthor();

      FsFile f;
      if (Storage.openFileForRead("SLP", epub.getCachePath() + "/progress.bin", f)) {
        uint8_t data[6];
        const int dataSize = f.read(data, 6);
        if (dataSize == 4 || dataSize == 6) {
          int currentSpineIndex = data[0] + (data[1] << 8);
          int currentPage = data[2] + (data[3] << 8);
          int pageCount = (dataSize == 6) ? (data[4] + (data[5] << 8)) : 0;
          if (pageCount > 0) {
            float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
            float bookProgress = epub.calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;

            // Pull the printed-page label (NCX <pageList> / EPUB 3 nav page-list /
            // EPUB 2.01 page-map / inline doc-pagebreak) directly from the section
            // cache so the sleep overlay can show e.g. "(42)" without instantiating
            // a Section + render parameters.
            std::string printedPagePrefix;
            if (const auto label = Section::getPrintedPageLabelFromCache(
                    epub.getCachePath() + "/sections", currentSpineIndex, static_cast<uint16_t>(currentPage))) {
              printedPagePrefix = *label + " ";
            }

            const int tocIndex = epub.getTocIndexForSpineIndex(currentSpineIndex);
            if (tocIndex != -1) {
              const auto tocItem = epub.getTocItem(tocIndex);
              info.chapterName = tocItem.title;
              char suffix[64];
              snprintf(suffix, sizeof(suffix), tr(STR_OVERLAY_CHAPTER_PAGE_SUFFIX), currentPage + 1, pageCount,
                       bookProgress);
              info.progressSuffix = printedPagePrefix + suffix;
              info.progressText = info.chapterName + info.progressSuffix;
            } else {
              char buf[80];
              snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1,
                       (unsigned)pageCount, bookProgress);
              info.progressText = printedPagePrefix + buf;
            }
          } else {
            char buf[64];
            snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS_NO_TOTAL), (unsigned long)currentPage + 1);
            info.progressText = buf;
          }
        }
        f.close();
      }
    }
  }

  return info;
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const BookOverlayInfo& overlayInfo) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  // The tone filters are greyscale modes too: they change how levels are derived, not
  // whether they exist. Omitting them here would silently drop those images to the
  // 1-bit half-refresh path.
  const bool hasGreyscale =
      bitmap.hasGreyscale() &&
      (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER ||
       SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::ADAPTIVE_TONE ||
       SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::EQUALIZE_TONE);

  // On a panel that resolves more than four levels the image is painted straight
  // into the panel's own canvas at full depth further down, and this 2-bit pass
  // would be a whole extra read of the BMP off the SD card for a rendition that is
  // then discarded. The framebuffer is still wanted -- cleared, carrying the
  // overlay alone -- so the overlay can be composited over the grey image.
  //
  // hasGreyscale gates it for the same reason it gates the plane path below: the
  // 1-bit and inverted filters have no levels to preserve, and the inversion
  // immediately below works on the framebuffer this path does not display.
  const bool nativeGray = hasGreyscale && renderer.getGrayLevels() > 4;

  if (!nativeGray) {
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

    if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
  }

  const uint8_t overlayMode = SETTINGS.sleepCoverOverlay;
  // Set by drawOverlay() to the block it actually drew, and left at zero height
  // when it draws nothing. The native path below composites this rectangle rather
  // than stamping black pixels, because the block is opaque and its text may be
  // white -- neither survives a stamp. See compositeBwRectOntoGray8Canvas.
  int overlayRectY = 0;
  int overlayRectHeight = 0;
  const auto drawOverlay = [&]() {
    const bool hasTitle = !overlayInfo.title.empty();
    const bool hasProgress = !overlayInfo.progressText.empty();
    const bool hasAuthor = !overlayInfo.author.empty();
    // If there is no overlay progress text, do not draw the overlay background block.
    if (!hasProgress) {
      return;
    }

    const int lineHeight12 = renderer.getLineHeight(BOOKERLY_12_FONT_ID);
    const int lineHeight10 = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int lineSpacing = 3;
    constexpr int sectionSpacing = 10;
    const int availableWidth = pageWidth - 20;

    int textBlockHeight = lineHeight10;  // progress line (always present here)
    if (hasTitle) {
      textBlockHeight += lineHeight12;
      textBlockHeight += hasAuthor ? lineSpacing : sectionSpacing;
    }
    if (hasAuthor) {
      textBlockHeight += lineHeight10 + sectionSpacing;
    }

    const bool textBlack = (overlayMode != 3);
    const int topPadding = lineHeight12 / 3;
    const int bottomPadding = lineHeight10 * 2 / 3;
    const int overlayHeight = textBlockHeight + topPadding + bottomPadding;
    const int overlayY = pageHeight - overlayHeight;
    overlayRectY = overlayY;
    overlayRectHeight = overlayHeight;

    if (overlayMode == 2) {
      renderer.fillRectDither(0, overlayY, pageWidth, overlayHeight, Color::LightGray);
    } else {
      renderer.fillRect(0, overlayY, pageWidth, overlayHeight, overlayMode == 3);
    }

    int currentY = overlayY + topPadding;

    if (hasTitle) {
      const std::string titleStr =
          renderer.truncatedText(BOOKERLY_12_FONT_ID, overlayInfo.title.c_str(), availableWidth, EpdFontFamily::BOLD);
      renderer.drawCenteredText(BOOKERLY_12_FONT_ID, currentY, titleStr.c_str(), textBlack, EpdFontFamily::BOLD);
      currentY += lineHeight12 + (hasAuthor ? lineSpacing : sectionSpacing);
    }

    if (hasAuthor) {
      const std::string authorStr = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.author.c_str(), availableWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, currentY, authorStr.c_str(), textBlack);
      currentY += lineHeight10 + sectionSpacing;
    }

    std::string progressStr;
    if (!overlayInfo.chapterName.empty()) {
      const int suffixWidth = renderer.getTextWidth(UI_10_FONT_ID, overlayInfo.progressSuffix.c_str());
      const int maxChapterWidth = availableWidth - suffixWidth;
      const std::string truncatedChapter =
          maxChapterWidth > 0 ? renderer.truncatedText(UI_10_FONT_ID, overlayInfo.chapterName.c_str(), maxChapterWidth)
                              : "";
      progressStr = truncatedChapter + overlayInfo.progressSuffix;
    } else {
      progressStr = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.progressText.c_str(), availableWidth);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, currentY, progressStr.c_str(), textBlack);
  };

  drawOverlay();

  // Native grayscale: the image goes into the panel's own canvas at full depth,
  // and the framebuffer -- cleared above, and holding nothing but the overlay
  // drawOverlay() just drew -- is composited on top of it.
  if (nativeGray) {
    uint16_t canvasStride = 0;
    if (uint8_t* canvas = renderer.borrowGray8Canvas(&canvasStride)) {
      for (int row = 0; row < renderer.getDisplayHeight(); row++) {
        memset(canvas + static_cast<uint32_t>(row) * canvasStride, 0xFF, renderer.getDisplayWidth());
      }
      const GfxRenderer::Gray8Target target{canvas, canvasStride};
      bitmap.rewindToData();
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, &target);
      // Composite the overlay's whole block rather than stamping its dark pixels:
      // the block is opaque, and in mode 3 it is black with WHITE text, so a
      // stamp would drop the fill and the text with it.
      if (overlayRectHeight > 0) {
        renderer.compositeBwRectOntoGray8Canvas(target, 0, overlayRectY, pageWidth, overlayRectHeight);
      }
      renderer.displayGray8Canvas();
      LOG_INF("SLP", "Sleep bitmap rendered at %u levels", static_cast<unsigned>(renderer.getGrayLevels()));
      return;
    }
  }

  if (!hasGreyscale) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    // Fire the BW scrub without waiting: the waveform runs on the controller's own RAM,
    // so the LSB draw below (CPU/SD-only work) overlaps it. copyGrayscaleLsbBuffers()
    // drains the pending finish before its SPI plane write.
    renderer.triggerDisplayAsync(HalDisplay::HALF_REFRESH);
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    drawOverlay();
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    drawOverlay();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  const bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // generateCoverBmp() runs the full-size PNG/JPEG decoder, whose inflate ring and pixel buffers
  // need a large contiguous block; on a big cover (e.g. a 1200x1848 PNG) that malloc fails under
  // sleep-time heap pressure and the cover silently falls back to /sleep.bmp. Free the ~52 KB
  // secondary framebuffer for headroom (same lever the Home cover loader uses). No realloc: the
  // sleep render below draws via the grayscale planes / controller RAM, not the secondary buffer,
  // and enterDeepSleep() tears everything down (chip reset on wake) immediately after.
  if (renderer.hasSecondaryBuffer()) renderer.releaseSecondaryBuffer();

  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (lastXtc.load() && lastXtc.generateCoverBmp()) {
      coverBmpPath = lastXtc.getCoverBmpPath();
    } else {
      LOG_ERR("SLP", "Failed to load/generate XTC cover bmp");
    }
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (lastTxt.load() && lastTxt.generateCoverBmp()) {
      coverBmpPath = lastTxt.getCoverBmpPath();
    } else {
      LOG_ERR("SLP", "No cover image found for TXT file");
    }
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here.
    if (lastEpub.load(true, true) && lastEpub.generateCoverBmp(cropped)) {
      coverBmpPath = lastEpub.getCoverBmpPath(cropped);
    } else {
      LOG_ERR("SLP", "Failed to load/generate EPUB cover bmp");
    }
  }

  if (coverBmpPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    // EPUB covers are cached as 8-bit greyscale, so dithering happens here rather
    // than at generation time, and the adaptive filter has real tonal data to work
    // with. Covers still cached as 2-bit (or TXT/XTC covers) take the native-palette
    // path inside Bitmap and are unaffected by either flag.
    Bitmap bitmap(file, /*dithering=*/true, sleepImageToneMapping(), sleepEqualizeBlend(renderer));
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      const uint8_t overlayMode = SETTINGS.sleepCoverOverlay;
      const BookOverlayInfo coverOverlayInfo =
          overlayMode != 0 ? getBookOverlayInfo(APP_STATE.openEpubPath) : BookOverlayInfo{};
      renderBitmapSleepScreen(bitmap, coverOverlayInfo);
      file.close();
      return;
    }
    file.close();
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderLastScreenSleepScreen() const {
  // Keep whatever is currently in the framebuffer (the reader page) and overlay a small moon
  // icon to signal sleep. main.cpp persists the framebuffer to SD so the next wake can restore
  // it before the boot screen would otherwise paint.
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderOverlaySleepScreen() const {
  // Overlay pictures always use portrait orientation regardless of the reader's orientation preference.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Step 1: Ensure the frame buffer contains the reader page.
  // When coming from a reader activity the frame buffer already holds the page.
  // When coming from a non-reader activity we re-render it from the saved progress.
  if (!APP_STATE.lastSleepFromReader && !APP_STATE.openEpubPath.empty()) {
    const auto& path = APP_STATE.openEpubPath;
    bool rendered = false;

    if (FsHelpers::checkFileExtension(path, ".xtc") || FsHelpers::checkFileExtension(path, ".xtch")) {
      rendered = XtcReaderActivity::drawCurrentPageToBuffer(path, renderer);
    } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
      rendered = TxtReaderActivity::drawCurrentPageToBuffer(path, renderer);
    } else if (FsHelpers::checkFileExtension(path, ".epub")) {
      rendered = EpubReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }

    if (!rendered) {
      LOG_DBG("SLP", "Page re-render failed, using white background");
      renderer.clearScreen();
    }
  }

  // Step 2: Load the overlay image using the same selection logic as renderCustomSleepScreen.
  // BMP: white pixels are skipped (transparent via drawBitmap), black pixels composited on top.
  // PNG: pixels with alpha < 128 are skipped; opaque pixels are drawn with their grayscale value.
  auto tryDrawOverlay = [&](const std::string& filename) -> bool {
    FsFile file;
    if (!Storage.openFileForRead("SLP", filename, file)) return false;
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      file.close();
      return false;
    }

    int x, y;
    float cropX = 0, cropY = 0;
    if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
      float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
      if (ratio > screenRatio) {
        x = 0;
        y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      } else {
        x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        y = 0;
      }
    } else {
      x = (pageWidth - bitmap.getWidth()) / 2;
      y = (pageHeight - bitmap.getHeight()) / 2;
    }

    // Draw without clearScreen so the reader page remains in the frame buffer beneath
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    file.close();
    return true;
  };

  auto tryDrawPngOverlay = [&](const std::string& filename) -> bool {
    constexpr size_t MIN_FREE_HEAP = 36 * 1024;  // uzlib ring (≤32 KB) + scanline buffers
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
      LOG_ERR("SLP", "Not enough heap for PNG overlay decoder");
      return false;
    }

    FsFile file;
    if (!Storage.openFileForRead("SLP", filename, file)) {
      LOG_DBG("SLP", "PNG open failed: %s", filename.c_str());
      return false;
    }
    auto decoder = std::make_unique<PngStreamDecoder>();
    PngStreamDecoder::Info info;
    if (!decoder->begin(file, info)) {
      LOG_DBG("SLP", "PNG decode start failed: %s", filename.c_str());
      file.close();
      return false;
    }

    const int srcW = static_cast<int>(info.width), srcH = static_cast<int>(info.height);
    float yScale = 1.0f;
    int dstW = srcW, dstH = srcH;
    if (srcW > pageWidth || srcH > pageHeight) {
      const float scaleX = (float)pageWidth / srcW, scaleY = (float)pageHeight / srcH;
      const float scale = (scaleX < scaleY) ? scaleX : scaleY;
      dstW = (int)(srcW * scale);
      dstH = (int)(srcH * scale);
      yScale = (float)dstH / srcH;
    }
    const int dstX = (pageWidth - dstW) / 2;
    const int dstY = (pageHeight - dstH) / 2;

    // Per-pixel alpha lets transparent pixels show the reader page beneath; opaque
    // pixels draw in their grayscale brightness (dark → black, light → white).
    std::unique_ptr<uint8_t[]> grayRow(new (std::nothrow) uint8_t[srcW]);
    std::unique_ptr<uint8_t[]> alphaRow(new (std::nothrow) uint8_t[srcW]);
    if (!grayRow || !alphaRow) {
      file.close();
      return false;
    }

    bool ok = true;
    int lastDstY = -1;
    for (int srcY = 0; srcY < srcH; srcY++) {
      if (!decoder->nextRow(grayRow.get(), alphaRow.get())) {
        ok = false;
        break;
      }
      const int destY = dstY + (int)(srcY * yScale);
      if (destY == lastDstY) continue;  // skip duplicate rows from Y scaling
      lastDstY = destY;
      if (destY < 0 || destY >= pageHeight) continue;

      int srcX = 0, error = 0;
      for (int dx = 0; dx < dstW; dx++) {
        const int outX = dstX + dx;
        if (outX >= 0 && outX < pageWidth && alphaRow[srcX] >= 128) {
          renderer.drawPixel(outX, destY, grayRow[srcX] < 128);  // true = black, false = white
        }
        // Bresenham-style X stepping (handles downscaling; 1:1 when srcW == dstW)
        error += srcW;
        while (error >= dstW) {
          error -= dstW;
          srcX++;
        }
      }
    }

    decoder->end();
    file.close();
    return ok;
  };

  // Collect images from both /.sleep and /sleep directories (no preference between them).
  // Accepts both .bmp and .png files; .bmp headers are validated during the scan.
  bool overlayDrawn = false;
  const auto files = collectSleepImages(/*allowPng=*/true);
  const auto numFiles = files.size();
  if (numFiles > 0) {
    const auto pickedIndex = pickSleepImageIndex(numFiles);
    APP_STATE.lastSleepImage = pickedIndex;
    APP_STATE.saveToFile();
    const std::string& selected = files[pickedIndex];
    if (FsHelpers::hasPngExtension(selected)) {
      overlayDrawn = tryDrawPngOverlay(selected);
    } else {
      overlayDrawn = tryDrawOverlay(selected);
    }
  }

  if (!overlayDrawn) {
    overlayDrawn = tryDrawOverlay("/sleep.bmp");
  }
  if (!overlayDrawn) {
    overlayDrawn = tryDrawPngOverlay("/sleep.png");
  }

  if (!overlayDrawn) {
    LOG_DBG("SLP", "No overlay image found, displaying page without overlay");
  }

  renderer.setOrientation(savedOrientation);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
