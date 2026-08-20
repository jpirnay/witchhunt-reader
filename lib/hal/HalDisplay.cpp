#include <BootHeapProbe.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "HalSpiBus.h"

// Global HalDisplay instance, bracketed by static-init heap probes (slots 0/1).
static BootHeapProbe s_probePreDisplay(0);
HalDisplay display;
static BootHeapProbe s_probePostDisplay(1);

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  // Drop the CPU clock while the render task sleeps out a waveform (any BUSY
  // wait that proves long — the SDK's bus fires the hooks around the ISR sleep
  // or the poll loop) and restore it before the post-waveform SPI work. Policy
  // (WiFi / lock / idle low-power guards) lives in HalPowerManager; the driver
  // just brackets the wait.
  einkDisplay.setBusyWaitHooks([] { powerManager.enterWaveformWait(); }, [] { powerManager.exitWaveformWait(); });

  {
    HalSpiBus::Lock spiLock;
    einkDisplay.begin();
  }

  // Request resync after specific wakeup events to ensure clean display state.
  // Skip when seamless=true so the current panel content is preserved (Quick Resume).
  if (!seamless) {
    const auto wakeupReason = gpio.getWakeupReason();
    if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
        wakeupReason == HalGPIO::WakeupReason::Other) {
      einkDisplay.requestResync();
    }
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

static uint8_t refreshModeToByte(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::RefreshMode::FAST_REFRESH:
      return 0x0C;
    case HalDisplay::RefreshMode::HALF_REFRESH:
      return 0xD4;
    case HalDisplay::RefreshMode::FULL_REFRESH:
    default:
      return 0x34;
  }
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::requestResync(uint8_t settlePasses) {
  if (gpio.deviceIsX3() && settlePasses > pendingX3SettlePasses) {
    pendingX3SettlePasses = settlePasses;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  lastRefreshMode = mode;
  lastDisplayModeByte = refreshModeToByte(mode);

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(pendingX3SettlePasses > 1 ? pendingX3SettlePasses : 1);
  } else if (pendingX3SettlePasses > 0) {
    einkDisplay.requestResync(pendingX3SettlePasses);
  }
  pendingX3SettlePasses = 0;

  // Panel time, measured at the one place every paint funnels through. The boot trace
  // (main.cpp) shows the splash costing seconds; without this the host-side compose and
  // the waveform itself are indistinguishable inside that number. FAST/HALF/FULL are
  // reported separately because promotions happen inside the driver (a FAST can be
  // upgraded to HALF on a cold panel), so the mode asked for is not always what ran.
  const unsigned long refreshStart = millis();
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  const unsigned long refreshMs = millis() - refreshStart;
  const char* const modeName =
      mode == RefreshMode::FAST_REFRESH ? "FAST" : (mode == RefreshMode::HALF_REFRESH ? "HALF" : "FULL");

  // The X4 differential keeps its previous-frame baseline in the host-managed secondary buffer,
  // so a FAST requested without one is promoted to HALF inside the driver
  // (FreeInkDisplay::resolveRefreshMode) — roughly 500 ms becoming 1700 ms. That used to be
  // invisible: the line read "mode=FAST took 1755 ms", which reads as a stuck panel rather than
  // a promotion, and cost a whole debugging session when Background-B's borrow of the secondary
  // buffer degraded every refresh behind the reader menu.
  //
  // The two flags are reported rather than the resolved mode on purpose: resolveRefreshMode() is
  // internal to the driver, and re-deriving its verdict here would be a second copy of the rule
  // that can drift from the first. These are the inputs it decides on, which is enough to explain
  // the duration without asserting what the driver did.
  if (mode == RefreshMode::FAST_REFRESH && gpio.deviceIsX4() && !einkDisplay.hasSecondaryBuffer() &&
      !einkDisplay.singleBufferFastDiff()) {
    LOG_DBG("DISP", "displayBuffer mode=%s took %lu ms (no diff baseline: secondary=0 fastDiff=0 -> driver runs HALF)",
            modeName, refreshMs);
  } else {
    LOG_DBG("DISP", "displayBuffer mode=%s took %lu ms", modeName, refreshMs);
  }
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  lastRefreshMode = mode;
  lastDisplayModeByte = refreshModeToByte(mode);

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(pendingX3SettlePasses > 1 ? pendingX3SettlePasses : 1);
  } else if (pendingX3SettlePasses > 0) {
    einkDisplay.requestResync(pendingX3SettlePasses);
  }
  pendingX3SettlePasses = 0;

  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() {
  HalSpiBus::Lock spiLock;
  einkDisplay.deepSleep();
}

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::syncWriteBufferFromActive() const { einkDisplay.syncWriteBufferFromActive(); }

void HalDisplay::releaseBuffers() { einkDisplay.releaseBuffers(); }

// FBUF: centralized framebuffer-state trace. Every secondary-buffer / RED-RAM / single-buffer
// transition logs here so a ghosting regression can be tracked to the exact op that left the panel
// diffing against the wrong baseline. free=largest contiguous 8-bit block (what a realloc needs).
static uint32_t fbufContig() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT); }

bool HalDisplay::releaseSecondaryBuffer() {
  HalSpiBus::Lock spiLock;  // see the note above borrowSecondaryBuffer()
  // Double-release guard. releaseSecondaryBuffer() returning false means the
  // secondary buffer was already gone — i.e. a caller released it without
  // tracking that it had, then released again. The release windows here are
  // NOT lexically scoped (Background-C releases in buildSection and restores in
  // recoverSecondaryBufferIfNeeded(), across page turns), so a lost release-flag
  // is a real, plausible bug rather than a theoretical one. It is not fatal —
  // the buffer is already freed — but it signals confused ownership that can
  // strand the reader in degraded (single-buffer) mode, so log it loudly.
  if (!einkDisplay.hasSecondaryBuffer()) {
    LOG_INF("FBUF", "releaseSecondary called but secondary already released (double-release; caller lost track)");
    return false;
  }
  const bool ok = einkDisplay.releaseSecondaryBuffer();
  LOG_INF("FBUF", "releaseSecondary -> %d (hasSecondary=%d redSynced=%d contig=%lu)", ok ? 1 : 0,
          einkDisplay.hasSecondaryBuffer() ? 1 : 0, einkDisplay.isRedRamSynced() ? 1 : 0,
          static_cast<unsigned long>(fbufContig()));
  return ok;
}

bool HalDisplay::reallocSecondaryBuffer() {
  HalSpiBus::Lock spiLock;  // see the note above borrowSecondaryBuffer()
  const bool ok = einkDisplay.reallocSecondaryBuffer();
  LOG_INF("FBUF", "reallocSecondary -> %d (hasSecondary=%d redSynced=%d contig=%lu)", ok ? 1 : 0,
          einkDisplay.hasSecondaryBuffer() ? 1 : 0, einkDisplay.isRedRamSynced() ? 1 : 0,
          static_cast<unsigned long>(fbufContig()));
  return ok;
}

bool HalDisplay::hasSecondaryBuffer() const { return einkDisplay.hasSecondaryBuffer(); }

// The four buffer-ownership transitions below all take the SPI bus lock, for two reasons that
// both trace back to them running on the LOOP task (reader background builds) while the render
// task drives the panel:
//
//  - borrowSecondaryBuffer() is not a pointer swap. It calls syncPendingAsync(), which runs the
//    driver's displayFinish() — real SPI traffic plus a BUSY wait. Unlocked, that collides with
//    the render task's own refresh and with SD transfers, which is precisely the unsynchronised
//    SdSpiCard failure this lock exists to prevent (see HalSpiBus.h).
//  - all four reassign frameBufferActive, which a deferred refresh still reads (X3's
//    post-waveform DTM1 sync). Serialising them behind the same lock the display paths take
//    means the swap cannot land mid-refresh.
//
// The mutex is recursive, so a caller that already holds it is unaffected, and the lock is
// uncontended on the normal path — this costs nothing when nothing else is on the bus.
uint8_t* HalDisplay::borrowSecondaryBuffer(size_t* size) {
  HalSpiBus::Lock spiLock;
  uint8_t* buf = einkDisplay.borrowSecondaryBuffer(size);
  LOG_INF("FBUF", "borrowSecondary -> %d (size=%lu hasSecondary=%d)", buf ? 1 : 0,
          static_cast<unsigned long>(buf && size ? *size : 0), einkDisplay.hasSecondaryBuffer() ? 1 : 0);
  return buf;
}

bool HalDisplay::returnSecondaryBuffer() {
  HalSpiBus::Lock spiLock;
  const bool ok = einkDisplay.returnSecondaryBuffer();
  LOG_INF("FBUF", "returnSecondary -> %d (hasSecondary=%d)", ok ? 1 : 0, einkDisplay.hasSecondaryBuffer() ? 1 : 0);
  return ok;
}

void HalDisplay::setSingleBufferFastDiff(bool enabled) {
  LOG_INF("FBUF", "singleBufferFastDiff=%d (hasSecondary=%d redSynced=%d)", enabled ? 1 : 0,
          einkDisplay.hasSecondaryBuffer() ? 1 : 0, einkDisplay.isRedRamSynced() ? 1 : 0);
  einkDisplay.setSingleBufferFastDiff(enabled);
}

void HalDisplay::triggerDisplay(RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  einkDisplay.triggerDisplay(static_cast<EInkDisplay::RefreshMode>(mode), turnOffScreen);
}

void HalDisplay::completeDisplay() {
  HalSpiBus::Lock spiLock;
  einkDisplay.completeDisplay();
}

// The async split locks each half separately rather than spanning the pair.
// triggerDisplayAsync() returns while the waveform runs, and that gap is
// deliberately used for CPU/RAM work (see GfxRenderer.h and the inline-AA path
// in EpubReaderActivity) - holding the bus across it would serialize away the
// overlap the split exists to create. It is safe to drop the lock in between:
// during the waveform the controller scans its own RAM and the panel is not
// driving SPI.
void HalDisplay::triggerDisplayAsync(RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  einkDisplay.triggerDisplayAsync(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::finishDisplayAsync() {
  HalSpiBus::Lock spiLock;
  einkDisplay.finishDisplayAsync();
}

bool HalDisplay::isRefreshPending() const { return einkDisplay.isRefreshPending(); }
bool HalDisplay::isRedRamSynced() const { return einkDisplay.isRedRamSynced(); }

HalDisplay::RefreshMode HalDisplay::getLastRefreshMode() const { return lastRefreshMode; }

uint8_t HalDisplay::getLastDisplayModeByte() const { return lastDisplayModeByte; }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::syncRedRamFromFrameBuffer() {
  einkDisplay.syncRedRamFromFrameBuffer();
  LOG_INF("FBUF", "syncRedRamFromFrameBuffer (hasSecondary=%d redSynced=%d)", einkDisplay.hasSecondaryBuffer() ? 1 : 0,
          einkDisplay.isRedRamSynced() ? 1 : 0);
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::cleanupGrayscaleWithPreviousBuffer() { einkDisplay.cleanupGrayscaleWithPreviousBuffer(); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
}

bool HalDisplay::deviceIsX3() const { return einkDisplay.isX3Mode(); }

void HalDisplay::setFastGrayscaleLut(bool fast) { einkDisplay.setFastGrayscaleLut(fast); }

bool HalDisplay::getFastGrayscaleLut() const { return einkDisplay.getFastGrayscaleLut(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
