#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <FrameRegion.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition
#include "ScreenshotName.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

namespace {
ScreenshotRequest requests;
}  // namespace

bool ScreenshotUtil::request(const Target target) {
  if (requests.request(target, millis())) return true;
  // A user's POWER+DOWN screenshot lost this way is worth seeing in the log.
  LOG_INF("SCR", "%s screenshot skipped: a host screenshot is still pending", target == Target::Host ? "Host" : "Card");
  return false;
}

void ScreenshotUtil::requestCardFromAnyTask() { requests.askForCard(); }

void ScreenshotUtil::service(GfxRenderer& renderer, void (*sendToHost)(const uint8_t* frame, uint32_t size, int width,
                                                                       int height, bool busy)) {
  if (requests.takeCardAsk()) request(Target::Card);
  const Target target = requests.pending();
  if (target == Target::None) return;
  if (requests.expired(millis())) {
    requests.clear();
    LOG_ERR("SCR", "Screenshot skipped: the display stayed busy for %lu ms", ScreenshotRequest::kWaitMs);
    if (target == Target::Host) sendToHost(nullptr, 0, 0, 0, /*busy=*/true);
    return;
  }
  if (RenderLock::peek()) return;  // a render pass, or the reader's AA pass, has the buffers
  RenderLock lock;
  // A refresh still uses them. Not isComposingFrame(): render passes hold the lock while they draw, and the
  // reader's pre-render clears the write buffer without ever displaying it, so that marker stays set
  // until it goes stale (2 s), holding every capture back for nothing.
  if (renderer.isRefreshPending()) return;
  requests.clear();
  // The write buffer is not the frame on the panel: after a two-buffer refresh it holds an older
  // frame, and the reader pre-renders its next page into it. So first the panel's frame as the
  // display keeps it, then the activity's correction on top, in the order the light drawer uses
  // (ActivityManager::dispatchLightPanelGesture). The other way round, the copy undid the reader's
  // redraw -- and that copy cannot be trusted while a pre-render is pending: a buffer handed back
  // after a Background-B borrow is seeded from the write buffer, pre-rendered page and all, which
  // is how a capture came out as the next page without its status bar.
  if (renderer.keepsPreviousFrame()) renderer.syncWriteBufferFromDisplayed();
  activityManager.prepareFramebufferForCapture();
  if (target == Target::Host) {
    // In Dark Mode the panel shows the frame inverted (HalDisplay::setInverted): the capture shows what it shows.
    const bool inverted = renderer.isOutputInverted();
    if (inverted) renderer.invertScreen();
    sendToHost(renderer.getFrameBuffer(), renderer.getBufferSize(), renderer.getDisplayWidth(),
               renderer.getDisplayHeight(), /*busy=*/false);
    if (inverted) renderer.invertScreen();
  } else {
    takeScreenshot(renderer);
  }
}

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (fb) {
    // A reader names the book, chapter, page and progress; any other screen gets screenshot-<millis>.bmp.
    char filename[256];
    screenshot_name::build(activityManager.screenshotInfo(), millis(), filename, sizeof(filename));
    const bool inverted = renderer.isOutputInverted();  // as for the host capture in service()
    if (inverted) renderer.invertScreen();
    // Upright as the reader is held: the held orientation, not the live one a hint strip flips to Portrait mid-draw.
    const bool saved = ScreenshotUtil::saveFramebufferAsBmp(filename, renderer, renderer.getHeldOrientation());
    if (inverted) renderer.invertScreen();
    if (saved) {
      LOG_DBG("SCR", "Screenshot saved to %s", filename);
    } else {
      LOG_ERR("SCR", "Failed to save screenshot");
    }
  } else {
    LOG_ERR("SCR", "Framebuffer not available");
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    renderer.drawRect(6, 6, renderer.getScreenWidth() - 12, renderer.getScreenHeight() - 12, 2, true);
    renderer.displayBuffer();
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}

// Ported from crosspoint-reader PR #4457 (Uri Tauber, @Uri-Tauber): the BMP used to be the portrait rotation whatever
// the orientation, so a landscape screenshot came out sideways.
bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const GfxRenderer& renderer,
                                          const GfxRenderer::Orientation orientation) {
  const uint8_t* framebuffer = renderer.getFrameBuffer();
  if (!framebuffer) {
    return false;
  }

  const int width = renderer.getScreenWidth(orientation);
  const int height = renderer.getScreenHeight(orientation);
  const uint32_t rowSizePadded = (width + 31) / 32 * 4;
  // A fixed buffer, not a VLA: the widest row is the T5S3's 960 pixels in landscape, 120 bytes.
  constexpr size_t kMaxRowSize = 128;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", static_cast<unsigned>(rowSizePadded));
    return false;
  }

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!dir.empty() && !Storage.exists(dir.c_str())) {
      if (!Storage.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  FsFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  BmpHeader header;

  createBmpHeader(&header, width, height, BmpRowOrder::BottomUp);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    file.close();
    Storage.remove(filename);
    return false;
  }

  // One row at a time, read back through the renderer's own logical-to-panel mapping. copyLogicalRow() fills the row's
  // (width + 7) / 8 bytes; the padding after them stays zero from here.
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);
  for (int outY = 0; outY < height; outY++) {
    // BMP rows run bottom to top.
    frameregion::copyLogicalRow(framebuffer, renderer.getDisplayWidthBytes(), orientation, renderer.getDisplayWidth(),
                                renderer.getDisplayHeight(), height - 1 - outY, width, rowBuffer);
    if (file.write(rowBuffer, rowSizePadded) != rowSizePadded) {
      write_error = true;
      break;
    }
  }

  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}
