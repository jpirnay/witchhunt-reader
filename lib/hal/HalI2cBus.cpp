#include "HalI2cBus.h"

#if FREEINK_CAP_TOUCH

#include <Logging.h>

namespace {
// A single I2C transaction here is small — a GT911 contact read, an RTC
// register block, a two-byte gauge read — and the bus runs at 400 kHz, so
// anything approaching this is a wedged transfer or a lock-order mistake, not
// contention. Time out, log, and proceed unlocked: a corrupted read is
// recoverable and visible, a silent hang is neither. Shorter than the SPI
// timeout because no I2C path here is bulk-transfer sized.
constexpr TickType_t I2C_LOCK_TIMEOUT_TICKS = pdMS_TO_TICKS(1000);
}  // namespace

HalI2cBus::HalI2cBus() {
  mutex = xSemaphoreCreateRecursiveMutex();
  if (mutex == nullptr) {
    LOG_ERR("I2C", "Failed to create I2C bus mutex - touch/RTC/gauge access is unserialized");
  }
}

HalI2cBus& HalI2cBus::getInstance() {
  static HalI2cBus i2cBus;
  return i2cBus;
}

void HalI2cBus::begin() { (void)getInstance(); }

HalI2cBus::Lock::Lock() {
  auto& bus = HalI2cBus::getInstance();
  if (bus.mutex == nullptr) {
    LOG_ERR("I2C", "I2C bus mutex not initialized, proceeding unlocked");
    return;
  }
  if (xSemaphoreTakeRecursive(bus.mutex, I2C_LOCK_TIMEOUT_TICKS) != pdTRUE) {
    LOG_ERR("I2C", "Timed out acquiring I2C bus mutex - proceeding unlocked (possible lock-order bug)");
    return;
  }
  acquired = true;
}

HalI2cBus::Lock::~Lock() {
  if (!acquired) return;
  xSemaphoreGiveRecursive(HalI2cBus::getInstance().mutex);
}

#endif  // FREEINK_CAP_TOUCH
