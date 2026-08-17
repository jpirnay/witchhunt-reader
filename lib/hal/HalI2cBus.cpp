#include "HalI2cBus.h"

#include <HalCapabilities.h>
#include <Logging.h>
#include <Wire.h>

#if FREEINK_CAP_TOUCH

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

// --- Bus start-up ------------------------------------------------------------
// Deliberately OUTSIDE the FREEINK_CAP_TOUCH guard: a board with no touch
// controller still needs its bus brought up (the X3's BQ27220 gauge and DS3231
// RTC), and there it is nobody else's job.

void HalI2cBus::ensureBusStarted() {
  static bool started = false;
  if (started) return;
  started = true;

  // Which pins do our own (non-touch) peripherals need? The gauge and the
  // sensor block each carry their own copy; take whichever is populated. They
  // agree on every board we ship -- one physical bus -- and a board that ever
  // splits them across two controllers would need the i2cBus field honoured
  // here too.
  const BoardConfig::BatteryGaugeConfig& gauge = BoardConfig::ACTIVE.batteryGauge;
  const BoardConfig::SensorsConfig& sensors = BoardConfig::ACTIVE.sensors;
  int8_t sda = BoardConfig::PIN_UNASSIGNED;
  int8_t scl = BoardConfig::PIN_UNASSIGNED;
  uint32_t hz = 400000;
  if (gauge.gaugeAddr != 0 && gauge.i2cSda != BoardConfig::PIN_UNASSIGNED) {
    sda = gauge.i2cSda;
    scl = gauge.i2cScl;
    hz = gauge.i2cHz;
  } else if (sensors.rtcAddr != 0 && sensors.i2cSda != BoardConfig::PIN_UNASSIGNED) {
    sda = sensors.i2cSda;
    scl = sensors.i2cScl;
    hz = sensors.i2cHz;
  }
  if (sda == BoardConfig::PIN_UNASSIGNED) {
    // Nothing of ours lives on I2C (X4). Leave the bus alone entirely.
    return;
  }

#if BOARD_SUPPORT_OWNS_BUSES
  LOG_INF("I2C", "Bus owned by the board-support layer; not re-initialising");
  return;
#endif

  // The SDK's InputManager already started this bus for the touch controller.
  // Starting it again would break it -- see the header.
  const BoardConfig::TouchConfig& touch = BoardConfig::ACTIVE.touch;
  if (BoardConfig::hasTouch() && touch.sda == sda && touch.scl == scl) {
    LOG_INF("I2C", "Bus already started by the touch driver (SDA%d/SCL%d); not re-initialising", sda, scl);
    return;
  }

  Wire.begin(sda, scl, hz);
  // Short timeout: a wedged peripheral must not stall the loop task. Matches
  // what the gauge path has always used.
  Wire.setTimeOut(4);
  LOG_INF("I2C", "Bus started SDA%d/SCL%d @%luHz", sda, scl, static_cast<unsigned long>(hz));
}
