#include "HalSpiBus.h"

#include <Logging.h>

namespace {
// A single SPI transaction is short (a panel refresh is driven in chunks, an SD
// transfer is block-sized), so anything past this means a lock-order mistake or
// a wedged transfer. Waiting forever would turn that into a silent hang; time
// out, log, and proceed unlocked so the failure is diagnosable in a log rather
// than presenting as a frozen device.
constexpr TickType_t SPI_LOCK_TIMEOUT_TICKS = pdMS_TO_TICKS(5000);
}  // namespace

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
    LOG_ERR("SPI", "SPI bus mutex not initialized, proceeding unlocked");
    return;
  }
  if (xSemaphoreTakeRecursive(bus.mutex, SPI_LOCK_TIMEOUT_TICKS) != pdTRUE) {
    LOG_ERR("SPI", "Timed out acquiring SPI bus mutex - proceeding unlocked (possible lock-order bug)");
    return;
  }
  acquired = true;
}

HalSpiBus::Lock::~Lock() {
  if (!acquired) return;
  xSemaphoreGiveRecursive(HalSpiBus::getInstance().mutex);
}
