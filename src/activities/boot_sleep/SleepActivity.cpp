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
#include <ImageScale.h>
#include <Memory.h>
#include <PngStreamDecoder.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <new>

#include "../reader/EpubProgressRecord.h"
#include "../reader/EpubReaderActivity.h"
#include "../reader/KOReaderAutoSync.h"
#include "../reader/TxtReaderActivity.h"
#include "../reader/XtcReaderActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SleepImagePlacement.h"
#include "SleepPixelCacheStamp.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"
#include "util/PrintfDouble.h"

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
//
// The name says nothing about which file was at the image's path, so the cache ends
// with a stamp of the PNG it was decoded from (SleepPixelCacheStamp.h).
std::string sleepPixelCachePath(const std::string& imagePath, uint8_t filter) {
  const size_t slash = imagePath.find_last_of('/');
  if (slash == std::string::npos) return "";
  return imagePath.substr(0, slash + 1) + "." + imagePath.substr(slash + 1) + ".f" + std::to_string(filter) + ".pxc";
}

// Header and rows of a width x height sleep pixel cache: where its source stamp starts.
size_t sleepPixelCacheBytes(const int width, const int height) {
  return PixelCache::PXC_HEADER_BYTES + static_cast<size_t>((width + 3) / 4) * static_cast<size_t>(height);
}

bool readSleepSourceStamp(const std::string& imagePath, sleep_pixel_cache::SourceStamp& out) {
  FsFile source;
  if (!Storage.openFileForRead("SLP", imagePath, source)) return false;
  out = sleep_pixel_cache::stampOf(source);
  source.close();
  return true;
}

// Stamps the cache the decode has just written with its source. A cache that cannot take the stamp is removed rather
// than left to be thrown away by the next sleep. False when there is no stamped cache to replay.
bool stampSleepPixelCache(const std::string& cachePath, const int width, const int height,
                          const sleep_pixel_cache::SourceStamp& source) {
  if (!Storage.exists(cachePath.c_str())) return false;  // the decode wrote none
  FsFile cacheFile;
  bool stamped = false;
  if (Storage.openFileForUpdate("SLP", cachePath, cacheFile)) {
    stamped = sleep_pixel_cache::append(cacheFile, sleepPixelCacheBytes(width, height), source);
    cacheFile.close();
  }
  if (!stamped) {
    LOG_ERR("SLP", "Could not stamp sleep pixel cache, removing: %s", cachePath.c_str());
    Storage.remove(cachePath.c_str());
  }
  return stamped;
}

// Replays a cached 2-bit image into the framebuffer under the current render mode.
// Returns false if the cache is missing, stale, does not match the expected
// geometry or was decoded from another file than `source`, in which case the caller
// decodes normally. A version, size or source mismatch deletes the file so the next
// decode rewrites it.
bool renderSleepImageFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                               int expectedHeight, const sleep_pixel_cache::SourceStamp& source) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("SLP", cachePath, cacheFile)) return false;

  uint16_t magic = 0;
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  if (cacheFile.read(&magic, 2) != 2 || cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }
  if (!PixelCache::magicIsValid(magic)) {
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
  // Same name and geometry, but perhaps another picture: a PNG replaced by one of the same dimensions.
  if (!sleep_pixel_cache::matches(cacheFile, sleepPixelCacheBytes(cachedWidth, cachedHeight), source,
                                  PixelCache::PXC_HEADER_BYTES)) {
    cacheFile.close();
    LOG_INF("SLP", "Sleep pixel cache is not of the image now at its path, deleting: %s", cachePath.c_str());
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
  const bool deep = renderer.getGrayLevels() > 4 || renderer.grayLevelPlan() != nullptr;
  return deep ? adaptive_tone::EQ_BLEND_NUM_DEEP : adaptive_tone::EQ_BLEND_NUM;
}

// Ships the BW base of a grey sleep image, ahead of its LSB/MSB planes.
//
// Overlapping the LSB render with that waveform is only safe while the secondary framebuffer is
// resident. X3 and the X4 Pro controllers re-read the frame after the waveform (displayFinish()
// syncs DTM1 from it; an X3 full sync also pushes it to the glass again), and the buffer swap is
// what keeps the BW frame intact for them while the next plane is drawn into the write buffer.
// Once the secondary is released or lent there is no swap, and the plane would overwrite the very
// frame they re-read, so the base has to finish before anything draws again.
void shipSleepGrayBase(const GfxRenderer& renderer) {
  if (renderer.hasSecondaryBuffer()) {
    renderer.triggerDisplayAsync(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

// Where a sleep BMP goes and how it is cut: drawBitmap()'s arguments, kept together so a plane can draw it again.
struct SleepPicture {
  const Bitmap& bitmap;
  int x;
  int y;
  int maxWidth;
  int maxHeight;
  int visibleWidth;
  int visibleHeight;
};

// The sleep picture as a gray-level plan's steps (GrayLevelPlan.h): the B/W base, then each pass over its planes,
// every plane drawn from the picture again (the quantizer gives each pixel the same ink every time). `overlay`
// lands the overlay block on a plane.
//
// The secondary buffer is LENT here, not released: a pass's second plane is drawn into it, so one read of the
// picture fills a pass. Where it is not resident (renderCustomSleepScreen frees it for PNG decoding), each plane
// is a read of its own. The block goes back unchanged, so returning it cannot fail.
//
// False before the base when the plan cannot start; false after it when a pass fails. Either way the caller draws
// the 4-level picture, which replaces whatever reached the glass.
bool renderSleepLevelPlanes(const GfxRenderer& renderer, const gray_levels::Plan& plan, const SleepPicture& picture,
                            const std::function<void(const GfxRenderer::LevelPlaneTarget&)>& overlay) {
  if (!gray_levels::isUsable(plan)) {
    LOG_ERR("SLP", "Gray-level plan unusable (%u levels, %u passes)", static_cast<unsigned>(plan.levels),
            static_cast<unsigned>(plan.passCount));
    return false;
  }
  auto quantizer = makeUniqueNoThrow<GrayLevelQuantizer>();
  if (!quantizer || !quantizer->init(plan.lightness, plan.levels)) {
    LOG_ERR("SLP", "Gray-level quantizer: %s", quantizer ? "bad lightness table" : "no memory");
    return false;
  }

  size_t lentSize = 0;
  uint8_t* lent = renderer.hasSecondaryBuffer() ? renderer.borrowSecondaryBuffer(&lentSize) : nullptr;
  if (lent && lentSize < renderer.getBufferSize()) {
    renderer.returnSecondaryBuffer();
    lent = nullptr;
  }

  const auto draw = [&](const gray_levels::Plane& first, const gray_levels::Plane* second) {
    GfxRenderer::LevelPlaneTarget target;
    target.quantizer = quantizer.get();
    target.first = first;
    if (second != nullptr && lent != nullptr) {
      target.second = lent;
      target.secondPlane = *second;
    }
    renderer.clearLevelPlanes(target);
    picture.bitmap.rewindToData();
    renderer.drawBitmap(picture.bitmap, picture.x, picture.y, picture.maxWidth, picture.maxHeight, picture.visibleWidth,
                        picture.visibleHeight, nullptr, &target);
    overlay(target);
  };

  bool ok = true;
  draw(plan.base, nullptr);
  if (!renderer.displayGrayLevelBase()) {
    LOG_ERR("SLP", "Gray-level base refused");
    ok = false;
  }
  for (uint8_t p = 0; ok && p < plan.passCount; ++p) {
    const gray_levels::Pass& pass = plan.passes[p];
    if (pass.planeCount == 2 && lent != nullptr) {
      draw(pass.planes[0], &pass.planes[1]);
      renderer.copyGrayscaleLsbBuffers();
      renderer.copyGrayscaleMsbBuffers(lent);
    } else {
      draw(pass.planes[0], nullptr);
      renderer.copyGrayscaleLsbBuffers();
      if (pass.planeCount == 2) {
        draw(pass.planes[1], nullptr);
        renderer.copyGrayscaleMsbBuffers();
      }
    }
    if (!renderer.displayGrayLevelPass(p, /*lastPass=*/p + 1 == plan.passCount)) {
      LOG_ERR("SLP", "Gray-level pass %u refused", static_cast<unsigned>(p));
      ok = false;
    }
  }
  if (lent != nullptr) renderer.returnSecondaryBuffer();
  return ok;
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
  //
  // Only in a build whose PNG decoder can paint the canvas: it does so only where
  // kMayLendGray8Canvas (the T5S3). The Picco reports 16 levels for its BMP
  // covers, and there the decoder would draw into the framebuffer instead, which
  // the native path then clears: a blank sleep screen with only the overlay.
  const bool nativeGray = HalDisplay::kMayLendGray8Canvas && renderer.getGrayLevels() > 4;

  // Mirror the decoder's own output sizing so the cache can be validated against the
  // geometry a fresh decode would produce (see PngToFramebufferConverter: fit to the
  // config box, never upscale).
  const std::string cachePath =
      // cppcheck-suppress knownConditionTrueFalse ; false only in builds whose PNG decoder cannot paint a canvas
      nativeGray ? std::string() : sleepPixelCachePath(filename, SETTINGS.sleepScreenCoverFilter);
  int cacheW = 0, cacheH = 0, cacheX = 0, cacheY = 0;
  ImageDimensions srcDims{};
  sleep_pixel_cache::SourceStamp sourceStamp;
  if (!cachePath.empty() && PngToFramebufferConverter::getDimensionsStatic(filename, srcDims) && srcDims.width > 0 &&
      srcDims.height > 0 && readSleepSourceStamp(filename, sourceStamp)) {
    const ImageScale scale =
        ImageScale::smaller(ImageScale::fit(srcDims.width, srcDims.height, pageWidth, pageHeight), ImageScale());
    cacheW = std::max(1, scale.apply(srcDims.width));
    cacheH = std::max(1, scale.apply(srcDims.height));
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
    useCache = renderSleepImageFromCache(renderer, cachePath, cacheX, cacheY, cacheW, cacheH, sourceStamp);
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
  // cache filename, and the source stamp goes on once the decode has written it.
  if (!useCache && cacheW > 0) config.cachePath = cachePath;

  // Overlay drawing is shared across all three rendering passes (BW + LSB + MSB) so the
  // text appears on every plane. Captured by reference so the lambda sees the renderer.
  const auto drawOverlay = [&]() {
    if (overlayInfo.progressText.empty()) {
      return;
    }
    const int lineHeight12 = renderer.getLineHeight(SERIF_12_FONT_ID);
    const int lineHeight10 = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int lineSpacing = 3;
    constexpr int sectionSpacing = 10;
    const int maxTextWidth = pageWidth - 20;

    int textBlockHeight = 0;
    if (!overlayInfo.title.empty()) {
      textBlockHeight += lineHeight12;
      textBlockHeight += overlayInfo.author.empty() ? sectionSpacing : lineSpacing;
    }
    if (!overlayInfo.author.empty()) {
      textBlockHeight += lineHeight10 + sectionSpacing;
    }
    textBlockHeight += lineHeight10;

    const int overlayY = pageHeight - textBlockHeight - (lineHeight12 / 3) - (lineHeight10 * 2 / 3);
    int y = overlayY + (lineHeight12 / 3);
    if (!overlayInfo.title.empty()) {
      const std::string title = renderer.truncatedText(SERIF_12_FONT_ID, overlayInfo.title.c_str(), maxTextWidth);
      renderer.drawText(SERIF_12_FONT_ID, 10, y, title.c_str(), true);
      y += lineHeight12;
      y += overlayInfo.author.empty() ? sectionSpacing : lineSpacing;
    }
    if (!overlayInfo.author.empty()) {
      const std::string author = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.author.c_str(), maxTextWidth);
      renderer.drawText(UI_10_FONT_ID, 10, y, author.c_str(), true);
      y += lineHeight10 + sectionSpacing;
    }
    const std::string progress = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.progressText.c_str(), maxTextWidth);
    renderer.drawText(UI_10_FONT_ID, 10, y, progress.c_str(), true);
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
  // cppcheck-suppress knownConditionTrueFalse ; false only in builds whose PNG decoder cannot paint a canvas
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
    // decoding again, once it carries its source's stamp.
    useCache = !config.cachePath.empty() && stampSleepPixelCache(cachePath, cacheW, cacheH, sourceStamp);
  }
  drawOverlay();
  // Fire the BW scrub without waiting where that is safe (see shipSleepGrayBase): the
  // waveform runs on the controller's own RAM, so the LSB decode below (CPU/SD-only work)
  // overlaps it. copyGrayscaleLsbBuffers() drains the pending finish before its SPI plane write.
  shipSleepGrayBase(renderer);

  // Passes 2 and 3 replay the cache when one is available; the render mode selects
  // which bit-plane each cached 2-bit value lands in, so no re-decode is needed.
  const auto renderPass = [&](const char* planeName) -> bool {
    if (useCache && renderSleepImageFromCache(renderer, cachePath, cacheX, cacheY, cacheW, cacheH, sourceStamp)) {
      return true;
    }
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

// True when renderCoverSleepScreen() is about to BUILD this book's sleep cover rather than just
// stream the one already on the SD card. Constructing the book object only hashes its path (the
// cache path is derived in the constructor), so this answers before any load() — which is the
// point: the answer decides whether the user is left staring at an unchanged screen.
//
// A book whose cover turns out to be missing altogether answers true as well. Nothing cheaper can
// tell "this will take seconds" from "this will fail immediately", and announcing an attempt that
// then falls back to the default sleep screen is the lesser wrong.
bool sleepCoverNeedsPreparing(const std::string& bookPath, bool cropped) {
  if (FsHelpers::hasXtcExtension(bookPath)) return !Xtc(bookPath, "/.crosspoint").coverBmpReady();
  if (FsHelpers::hasTxtExtension(bookPath)) return !Txt(bookPath, "/.crosspoint").coverBmpReady();
  if (FsHelpers::hasEpubExtension(bookPath)) return !Epub(bookPath, "/.crosspoint").coverBmpReady(cropped);
  return false;  // not a format with a cover: nothing to prepare, so nothing to announce
}

}  // namespace

void SleepActivity::onEnter() {
#if CROSSPOINT_KOREADER_AUTOSYNC
  const uint64_t sleepPushSeq = KOReaderAutoSync::pushStashedSleepPushAsync();
#endif

  Activity::onEnter();
  renderSleepScreen();

#if CROSSPOINT_KOREADER_AUTOSYNC
  KOReaderAutoSync::joinStashedSleepPush(sleepPushSeq);
#endif
}

// Applies always when the user picked Quick Resume as sleep screen, or only on timeout sleeps when "Quick Resume on
// Timeout" is on.
bool SleepActivity::keepsLastScreen() const {
  return SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
         (fromTimeout &&
          SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
}

bool SleepActivity::showsCustomImageOnly() {
  switch (static_cast<CrossPointSettings::SLEEP_SCREEN_MODE>(SETTINGS.sleepScreen)) {
    case CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM:
      return true;
    case CrossPointSettings::SLEEP_SCREEN_MODE::COVER:
    case CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM:
      return APP_STATE.openEpubPath.empty();  // renderSleepScreen()'s fallback without a book
    case CrossPointSettings::SLEEP_SCREEN_MODE::DARK:
    case CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT:
    case CrossPointSettings::SLEEP_SCREEN_MODE::BLANK:
    case CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY:
    case CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME:
    case CrossPointSettings::SLEEP_SCREEN_MODE::SLEEP_SCREEN_MODE_COUNT:
      return false;
  }
  return false;
}

void SleepActivity::renderSleepScreen() {
  RenderLock lock(*this);

  // Quick Resume: paint a moon icon over the current page and keep the framebuffer
  // intact for the next wake.
  if (keepsLastScreen()) {
    return renderLastScreenSleepScreen();
  }

  // No "Entering sleep..." popup here: it shipped a full extra refresh (~500 ms on X3)
  // before the sleep screen's own refresh; the sleep screen appearing is the feedback.
  // The one exception is a cover that still has to be built — renderCoverSleepScreen()
  // puts the popup up there, because that path is seconds away from its own refresh.
  // The renderers below all expect portrait: the cover BMP is generated portrait-sized
  // (getDisplayHeight x getDisplayWidth) and the custom/default screens are laid out
  // portrait. The reader's onExit() puts portrait back before this runs, on a timeout sleep
  // too (goToSleep() replaces the activity, which runs it); forced here as well, so a
  // landscape cover cannot overflow the edges / mis-center whatever the outgoing activity
  // left. OVERLAY manages its own orientation (renderOverlaySleepScreen), so leave it
  // untouched here.
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
  // A PNG needs a ~32 KB contiguous inflate ring, and renderPngSleepScreen() refuses to start
  // under 60 KB free. With the ~48-52 KB secondary framebuffer still resident, a sleep straight out
  // of a book can be short of both, and the picture silently gave way to the default screen
  // (#377). Free it, as renderCoverSleepScreen() does: nothing below needs the previous frame
  // (shipSleepGrayBase() covers the drivers that re-read it), and enterDeepSleep() powers down
  // right after. COVER_CUSTOM falls back to this screen with the buffer already gone.
  if (renderer.hasSecondaryBuffer()) renderer.releaseSecondaryBuffer();

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
    const BookOverlayInfo resolvedOverlayInfo =
        shouldLoadOverlayInfo ? getBookOverlayInfo(APP_STATE.openEpubPath) : overlayInfo;
    const auto renderSleepImage = [&](const std::string& filename) -> bool {
      if (FsHelpers::hasPngExtension(filename)) {
        return renderPngSleepScreen(filename, renderer, resolvedOverlayInfo);
      }
      FsFile file;
      if (!Storage.openFileForRead("SLP", filename, file)) return false;
      delay(100);
      Bitmap bitmap(file, true, sleepImageToneMapping(), sleepEqualizeBlend(renderer));
      const bool parsed = bitmap.parseHeaders() == BmpReaderError::Ok;
      if (parsed) renderBitmapSleepScreen(bitmap, resolvedOverlayInfo);
      file.close();
      return parsed;
    };

    // The picked image first, then the ones after it. A PNG is only validated by decoding it, so
    // a truncated or unsupported one should cost the user that one picture, not all of them. A
    // failed PNG can take two decodes (tone analysis, then the render) and the device sleeps only
    // after this returns, so a folder of broken images gets a few tries, not one per file.
    constexpr size_t MAX_SLEEP_IMAGE_ATTEMPTS = 3;
    const auto pickedIndex = pickSleepImageIndex(numFiles);
    const size_t attempts = std::min(numFiles, MAX_SLEEP_IMAGE_ATTEMPTS);
    for (size_t attempt = 0; attempt < attempts; ++attempt) {
      const size_t index = (pickedIndex + attempt) % numFiles;
      // Saved before the attempt rather than after it, as before: an image that takes the device
      // down mid-decode is then not the next pick as well.
      APP_STATE.lastSleepImage = index;
      APP_STATE.saveToFile();
      const auto& filename = files[index];
      LOG_DBG("SLP", "Loading sleep image: %s", filename.c_str());
      if (renderSleepImage(filename)) return;
      LOG_ERR("SLP", "Sleep image failed: %s", filename.c_str());
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
          // Already a whole percent (Xtc::calculateProgress), not a fraction.
          const uint8_t percent = xtc.calculateProgress(currentPage);
          char buf[64];
          snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1, totalPages,
                   printfDouble(percent));
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
            const int percent = ProgressPpb::of(currentPage + 1, totalPages).percentHalfEven();
            char buf[64];
            snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1, totalPages,
                     printfDouble(percent));
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
        uint8_t data[EpubProgressRecord::kMaxSize];
        const int dataSize = f.read(data, sizeof(data));
        f.close();
        if (const auto record = EpubProgressRecord::decode(data, dataSize > 0 ? static_cast<size_t>(dataSize) : 0)) {
          const int currentSpineIndex = record->spineIndex;
          const int currentPage = record->page;
          const int pageCount = record->pageCount;
          if (pageCount > 0) {
            // Rounded as the "%.0f" of the translated strings below rounded the float it was.
            const double bookPercent = printfDouble(
                epub.calculateProgress(currentSpineIndex, ProgressPpb::of(currentPage, pageCount)).percentHalfEven());

            // The reader records what its status bar showed when it closed the book: the page within
            // the whole TOC chapter (#325) and the printed page. Records without it (older ones, a
            // sync restore, a bookmark jump) count the page within its spine item, and pull the
            // printed-page label (NCX <pageList> / EPUB 3 nav page-list / EPUB 2.01 page-map /
            // inline doc-pagebreak) from the section cache without instantiating a Section +
            // render parameters.
            const auto& shown = record->shown;
            const bool shownChapter = shown && shown->chapterTotal > 0;
            std::string printedPagePrefix;
            if (shown) {
              if (!shown->printedPage.empty()) printedPagePrefix = shown->printedPage + " ";
            } else if (const auto label = Section::getPrintedPageLabelFromCache(epub.getCachePath(), currentSpineIndex,
                                                                                static_cast<uint16_t>(currentPage))) {
              printedPagePrefix = *label + " ";
            }

            const int tocIndex = epub.getTocIndexForSpineIndex(currentSpineIndex);
            if (tocIndex != -1) {
              const auto tocItem = epub.getTocItem(tocIndex);
              info.chapterName = tocItem.title;
              const int chapterPage = shownChapter ? shown->chapterPage : currentPage + 1;
              const int chapterTotal = shownChapter ? shown->chapterTotal : pageCount;
              char suffix[64];
              snprintf(suffix, sizeof(suffix), tr(STR_OVERLAY_CHAPTER_PAGE_SUFFIX), chapterPage, chapterTotal,
                       bookPercent);
              info.progressSuffix = printedPagePrefix + suffix;
              info.progressText = info.chapterName + info.progressSuffix;
            } else {
              char buf[80];
              snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1,
                       (unsigned)pageCount, bookPercent);
              info.progressText = printedPagePrefix + buf;
            }
          } else {
            char buf[64];
            snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS_NO_TOTAL), (unsigned long)currentPage + 1);
            info.progressText = buf;
          }
        }
      }
    }
  }

  return info;
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const BookOverlayInfo& overlayInfo) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto placement =
      SleepImagePlacement::place(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight,
                                 SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP);
  const int x = placement.x;
  const int y = placement.y;
  const BoxCut& draw = placement.draw;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d: drawing to %d x %d, shown %d x %d within %d x %d", bitmap.getWidth(),
          bitmap.getHeight(), pageWidth, pageHeight, x, y, draw.visibleWidth, draw.visibleHeight, draw.maxWidth,
          draw.maxHeight);
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
  // A panel without that canvas may still take more than four levels as a gray-level plan's passes
  // (GrayLevelPlan.h), drawn plane by plane further down. The 2-bit pass is skipped for it as well.
  // Compiled out with the rest of the plan path where no driver has one (HalDisplay::kMayRunGrayLevelPlan).
  const gray_levels::Plan* levelPlan = nullptr;
  if constexpr (HalDisplay::kMayRunGrayLevelPlan) {
    if (hasGreyscale && !nativeGray) levelPlan = renderer.grayLevelPlan();
  }

  const auto drawTwoBitPicture = [&]() {
    renderer.drawBitmap(bitmap, x, y, draw.maxWidth, draw.maxHeight, draw.visibleWidth, draw.visibleHeight);

    if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
  };
  // cppcheck-suppress knownConditionTrueFalse ; levelPlan stays nullptr only in builds without kMayRunGrayLevelPlan
  if (!nativeGray && levelPlan == nullptr) drawTwoBitPicture();

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

    const int lineHeight12 = renderer.getLineHeight(SERIF_12_FONT_ID);
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
          renderer.truncatedText(SERIF_12_FONT_ID, overlayInfo.title.c_str(), availableWidth, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SERIF_12_FONT_ID, currentY, titleStr.c_str(), textBlack, EpdFontFamily::BOLD);
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

  // The 2-bit picture after a deeper path could not run: those skipped it, and the framebuffer holds the
  // overlay alone (or a plane's bits). Everything below then works as on a 4-level panel.
  const auto redrawTwoBit = [&]() {
    renderer.clearScreen();
    bitmap.rewindToData();
    drawTwoBitPicture();
    drawOverlay();
  };

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
      renderer.drawBitmap(bitmap, x, y, draw.maxWidth, draw.maxHeight, draw.visibleWidth, draw.visibleHeight, &target);
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
    LOG_ERR("SLP", "No gray8 canvas, drawing 4 levels");
    redrawTwoBit();
  }

  if constexpr (HalDisplay::kMayRunGrayLevelPlan) {
    if (levelPlan != nullptr) {
      // drawOverlay() above, on the cleared framebuffer, has found the block's rectangle. On a plane it gets a
      // white ground first, because the light-gray fill (mode 2) skips the rows it leaves white and the plane's
      // bits would show through there; then it lands as solid inks.
      const auto overlayOnPlane = [&](const GfxRenderer::LevelPlaneTarget& target) {
        if (overlayRectHeight <= 0) return;
        renderer.fillRect(0, overlayRectY, pageWidth, overlayRectHeight, false);
        drawOverlay();
        renderer.bwRectToLevelPlanes(target, 0, overlayRectY, pageWidth, overlayRectHeight);
      };
      if (renderSleepLevelPlanes(
              renderer, *levelPlan,
              SleepPicture{bitmap, x, y, draw.maxWidth, draw.maxHeight, draw.visibleWidth, draw.visibleHeight},
              overlayOnPlane)) {
        LOG_INF("SLP", "Sleep bitmap rendered at %u levels (gray-level plan)",
                static_cast<unsigned>(levelPlan->levels));
        return;
      }
      LOG_ERR("SLP", "Gray-level plan failed, drawing 4 levels");
      // renderCoverSleepScreen() kept the secondary buffer for the plan; the 4-level path runs as it always has,
      // without it.
      if (renderer.hasSecondaryBuffer()) renderer.releaseSecondaryBuffer();
      redrawTwoBit();
    }
  }

  if (!hasGreyscale) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    // Absolute planes for the BMP sleep screen follow crosspoint-reader PR
    // #3469 (Bryan O'Sullivan / @bos), including its boundary: preserved-
    // background and transparent-overlay sleep screens stay differential.
    //
    // Absolute planes carry every pixel instead of masking grey over the B/W
    // base; on the UC8279 X3 that runs the long XTH4 waveform, which is what
    // clears the vertical banding a dithered image shows under the short nudge.
    //
    // It costs the overlap below. An absolute pass needs its base settled
    // before the planes land -- both X3 drivers report asyncBase=false -- and
    // beginAbsoluteGrayPass() does that base push itself, blocking. The
    // differential path keeps the async scrub.
    const bool panelHasAbsolute = renderer.supportsAbsoluteGrayPlanes();
    // A reader exit arms a HALF via setNextDisplayRefreshMode(); beginAbsoluteGrayPass() below
    // consumes it as its base mode. Logged BEFORE that consume, so the line still says whether one
    // was pending (it was, on every reader -> cover sleep captured on X3, X4 and X4 Pro).
    LOG_DBG("SLP", "Gray base: overridePending=%d", renderer.hasRefreshOverridePending() ? 1 : 0);
    const bool absolutePass = panelHasAbsolute && renderer.beginAbsoluteGrayPass();
    LOG_DBG("SLP", "Grayscale planes: %s",
            absolutePass ? "absolute"
                         : (panelHasAbsolute ? "differential (panel declined the absolute pass)"
                                             : "differential (panel has no absolute encoding)"));
    if (!absolutePass) {
      // Fire the BW scrub without waiting where that is safe (see shipSleepGrayBase): the
      // waveform runs on the controller's own RAM, so the LSB draw below (CPU/SD-only work)
      // overlaps it. copyGrayscaleLsbBuffers() drains the pending finish before its SPI plane write.
      shipSleepGrayBase(renderer);
    }

    // A differential plane starts empty and lets the B/W base supply black and
    // white. An absolute plane carries the whole frame, so it starts white --
    // a clear bit is no longer "leave as is" -- and the overlay goes in as
    // ordinary BW, which is already the right bits when white is 11 and black
    // 00 across the pair.
    const auto renderPlane = [&](const GfxRenderer::RenderMode mode) {
      bitmap.rewindToData();
      renderer.clearScreen(absolutePass ? 0xFF : 0x00);
      renderer.setRenderMode(mode);
      renderer.setAbsoluteGrayPlanes(absolutePass);
      renderer.drawBitmap(bitmap, x, y, draw.maxWidth, draw.maxHeight, draw.visibleWidth, draw.visibleHeight);
      // setRenderMode(BW) also clears the absolute flag, hence re-arming it per
      // plane above.
      if (absolutePass) renderer.setRenderMode(GfxRenderer::BW);
      drawOverlay();
    };

    renderPlane(GfxRenderer::GRAYSCALE_LSB);
    renderer.copyGrayscaleLsbBuffers();

    renderPlane(GfxRenderer::GRAYSCALE_MSB);
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

  // onEnter() skips the "going to sleep" popup because the sleep screen itself is the feedback,
  // one refresh away. That reasoning does not survive a first-time cover: extracting it from the
  // book and running the full-size PNG/JPEG decoder takes seconds, during which the screen still
  // shows the last page and the device looks ignored. So announce it — but only then.
  const bool coverNeedsPreparing = sleepCoverNeedsPreparing(APP_STATE.openEpubPath, cropped);
  if (coverNeedsPreparing) {
    // Grab the on-screen frame while it is still reachable: it lives in the secondary buffer, and
    // the release below frees exactly that. drawPopup() then composes the box onto the copy
    // (overlayDisplayedFrame = false) instead of doing a sync that would by then be a no-op.
    renderer.syncWriteBufferFromDisplayed();
  }

  // generateCoverBmp() runs the full-size PNG/JPEG decoder, whose inflate ring and pixel buffers
  // need a large contiguous block; on a big cover (e.g. a 1200x1848 PNG) that malloc fails under
  // sleep-time heap pressure and the cover silently falls back to /sleep.bmp. Free the ~52 KB
  // secondary framebuffer for headroom (same lever the Home cover loader uses). No realloc: the
  // sleep render below draws via the grayscale planes / controller RAM, not the secondary buffer,
  // and enterDeepSleep() tears everything down (chip reset on wake) immediately after.
  //
  // Except where the cover is going out as a gray-level plan and is already built: the plan then borrows the
  // block for a pass's second plane (renderSleepLevelPlanes), which saves one read of the cover per pass. A cover
  // still to be built needs the heap, so the block is freed for it as before.
  const bool keepForLevelPlan = renderer.grayLevelPlan() != nullptr && !coverNeedsPreparing;
  if (renderer.hasSecondaryBuffer() && !keepForLevelPlan) renderer.releaseSecondaryBuffer();

  if (coverNeedsPreparing) {
    // Ship the popup without waiting out its waveform. The panel repaints from the controller's
    // own RAM, and the cover build below is pure SD -> SD streaming that never reads or writes the
    // framebuffer, so the two overlap: the popup costs the moment it takes to push the frame, not
    // the ~600 ms it takes to appear. It is armed after the release because the release drains a
    // refresh in flight (FreeInkDisplay::releaseSecondaryBuffer): armed before it, the popup's
    // waveform would be waited out there and nothing would overlap. finishDisplayAsync() below
    // closes the window again.
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP), /*overlayDisplayedFrame=*/false, PopupShip::Async);
  }

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

  // Everything below writes the framebuffer or drives the panel, so the overlapped popup refresh
  // has to be finished first.
  if (coverNeedsPreparing) renderer.finishDisplayAsync();

  if (coverBmpPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    // EPUB and TXT covers converted from a JPEG or PNG are cached as 8-bit greyscale,
    // so dithering happens here, with this panel's table, rather than at generation
    // time, and the adaptive filter has real tonal data to work with. 2-bit covers (XTC
    // covers, a 2-bit BMP beside a TXT book) take the native-palette path inside Bitmap
    // and are unaffected by either flag.
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
  renderer.displayBuffer(DarkMode::moonGoesOutFast(renderer.isOutputInverted()) ? HalDisplay::FAST_REFRESH
                                                                                : HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderOverlaySleepScreen() const {
  // Overlay pictures always use portrait orientation regardless of the reader's orientation preference.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Step 1: Ensure the frame buffer contains the reader page.
  // When coming from a reader activity the frame buffer already holds the page.
  // When coming from a non-reader activity we re-render it from the saved progress. So too when the reader drew it
  // for Dark Mode, its pictures inverted (GfxRenderer::keepsImagePolarity): this screen shows in normal polarity.
  const bool pageDrawnForDarkMode = APP_STATE.lastSleepFromReader && pageWentOutInverted;
  if ((!APP_STATE.lastSleepFromReader || pageDrawnForDarkMode) && !APP_STATE.openEpubPath.empty()) {
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

    const auto placement =
        SleepImagePlacement::place(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, /*crop=*/false);

    // Draw without clearScreen so the reader page remains in the frame buffer beneath
    renderer.drawBitmap(bitmap, placement.x, placement.y, pageWidth, pageHeight, 0, 0);
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
    // Fit the page, never up. Rows map by dstH / srcH, the scale the height actually got.
    const ImageScale fit = ImageScale::smaller(ImageScale::fit(srcW, srcH, pageWidth, pageHeight), ImageScale());
    const int dstW = fit.apply(srcW), dstH = fit.apply(srcH);
    const ImageScale yScale(dstH, srcH);
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
      const int destY = dstY + yScale.apply(srcY);
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
