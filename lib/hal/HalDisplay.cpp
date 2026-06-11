#include <BootHeapProbe.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

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

  einkDisplay.begin();

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
  lastRefreshMode = mode;
  lastDisplayModeByte = refreshModeToByte(mode);

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(pendingX3SettlePasses > 1 ? pendingX3SettlePasses : 1);
  } else if (pendingX3SettlePasses > 0) {
    einkDisplay.requestResync(pendingX3SettlePasses);
  }
  pendingX3SettlePasses = 0;

  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
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

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
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

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::releaseBuffers() { einkDisplay.releaseBuffers(); }

bool HalDisplay::releaseSecondaryBuffer() { return einkDisplay.releaseSecondaryBuffer(); }

bool HalDisplay::reallocSecondaryBuffer() { return einkDisplay.reallocSecondaryBuffer(); }

bool HalDisplay::hasSecondaryBuffer() const { return einkDisplay.hasSecondaryBuffer(); }

void HalDisplay::triggerDisplay(RefreshMode mode, bool turnOffScreen) {
  einkDisplay.triggerDisplay(static_cast<EInkDisplay::RefreshMode>(mode), turnOffScreen);
}

void HalDisplay::completeDisplay() { einkDisplay.completeDisplay(); }

bool HalDisplay::isRefreshPending() const { return einkDisplay.isRefreshPending(); }

HalDisplay::RefreshMode HalDisplay::getLastRefreshMode() const { return lastRefreshMode; }

uint8_t HalDisplay::getLastDisplayModeByte() const { return lastDisplayModeByte; }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::syncRedRamFromFrameBuffer() { einkDisplay.syncRedRamFromFrameBuffer(); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::cleanupGrayscaleWithPreviousBuffer() { einkDisplay.cleanupGrayscaleWithPreviousBuffer(); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

void HalDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  einkDisplay.displayWindow(x, y, w, h, turnOffScreen);
}

bool HalDisplay::deviceIsX3() const { return einkDisplay.isX3Mode(); }

void HalDisplay::setFastGrayscaleLut(bool fast) { einkDisplay.setFastGrayscaleLut(fast); }

bool HalDisplay::getFastGrayscaleLut() const { return einkDisplay.getFastGrayscaleLut(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
