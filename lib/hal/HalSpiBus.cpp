#include "HalSpiBus.h"

#include <Logging.h>

HalSpiBus::HalSpiBus() {
  mutex = xSemaphoreCreateRecursiveMutex();
  if (mutex == nullptr) {
    LOG_ERR("SPI", "Failed to create SPI bus mutex - display/SD access is unserialized");
  }
}

HalSpiBus& HalSpiBus::getInstance() {
  static HalSpiBus spiBus;
  return spiBus;
}

void HalSpiBus::begin() { (void)getInstance(); }

HalSpiBus::Lock::Lock() {
  auto& bus = HalSpiBus::getInstance();
  if (bus.mutex == nullptr) {
    LOG_ERR("SPI", "SPI bus mutex not initialized");
    assert(false && "SPI bus mutex not initialized");
    return;
  }
  xSemaphoreTakeRecursive(bus.mutex, portMAX_DELAY);
  acquired = true;
}

HalSpiBus::Lock::~Lock() {
  if (!acquired) return;
  xSemaphoreGiveRecursive(HalSpiBus::getInstance().mutex);
}
