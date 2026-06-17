#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;

  uint8_t score() const {
    return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658);
  }
};

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  *outValue = Wire.read();
  return true;
}

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

bool probeBQ27220Signature() {
  uint16_t soc = 0;
  uint16_t voltageMv = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc)) {
    return false;
  }
  if (soc > 100) {
    return false;
  }
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_VOLT_REG, &voltageMv)) {
    return false;
  }
  return voltageMv >= 2500 && voltageMv <= 5000;
}

bool probeDS3231Signature() {
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) {
    return false;
  }
  const uint8_t tensDigit = (sec >> 4) & 0x07;
  const uint8_t onesDigit = sec & 0x0F;

  return tensDigit <= 5 && onesDigit <= 9;
}

bool probeQMI8658Signature() {
  uint8_t whoami = 0;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);

  result.bq27220 = probeBQ27220Signature();
  result.ds3231 = probeDS3231Signature();
  result.qmi8658 = probeQMI8658Signature();

  Wire.end();
  pinMode(20, INPUT);
  pinMode(0, INPUT);
  return result;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: run active X3 fingerprint probe and persist result.
  const X3GPIO::X3ProbeResult pass1 = X3GPIO::runX3ProbePass();
  delay(2);
  const X3GPIO::X3ProbeResult pass2 = X3GPIO::runX3ProbePass();

  const uint8_t score1 = pass1.score();
  const uint8_t score2 = pass2.score();
  LOG_INF("HW", "X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)", score1, pass1.bq27220,
          pass1.ds3231, pass1.qmi8658, score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);
  const bool x3Confirmed = (score1 >= 2) && (score2 >= 2);
  const bool x4Confirmed = (score1 == 0) && (score2 == 0);

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  _deviceType = detectDeviceTypeWithFingerprint();

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
}

// Push one debounced edge into the loop-drained FIFO. Caller holds inputMux_.
// Drops the newest edge if the ring is full — a full ring means the loop task
// has been blocked through 32 distinct transitions, far more than any real burst.
void HalGPIO::pushEdgeLocked(uint8_t button, bool pressed, uint32_t timeMs) {
  const int next = (edgeTail_ + 1) % EDGE_BUF;
  if (next == edgeHead_) {
    return;
  }
  edgeBuf_[edgeTail_] = {button, pressed, timeMs};
  edgeTail_ = next;
}

// One sampling pass: read + debounce the buttons, then latch any edges. Runs on
// the sampler task once started; also called synchronously from update() before
// the sampler is up. inputMgr.update() does the ADC read and must run OUTSIDE the
// critical section (analogRead may take the ADC driver mutex). Only the latching
// of the results into the shared accumulators/queue is done under inputMux_.
void HalGPIO::sampleOnce() {
  inputMgr.update();

  uint8_t live = 0;
  uint8_t pressed = 0;
  uint8_t released = 0;
  for (uint8_t i = 0; i <= BTN_POWER; i++) {
    if (inputMgr.isPressed(i)) live |= (1u << i);
    if (inputMgr.wasPressed(i)) pressed |= (1u << i);
    if (inputMgr.wasReleased(i)) released |= (1u << i);
  }
  const uint32_t now = millis();
  const unsigned long held = inputMgr.getHeldTime();

  portENTER_CRITICAL(&inputMux_);
  liveState_ = live;
  accumPressed_ |= pressed;
  accumReleased_ |= released;
  heldTimeSnapshot_ = held;
  for (uint8_t i = 0; i <= BTN_POWER; i++) {
    // A debounced button makes at most one transition per pass, so a button can
    // appear in pressed or released here, never both.
    if (pressed & (1u << i)) pushEdgeLocked(i, true, now);
    if (released & (1u << i)) pushEdgeLocked(i, false, now);
  }
  portEXIT_CRITICAL(&inputMux_);
}

void HalGPIO::samplerTask(void* arg) {
  HalGPIO* self = static_cast<HalGPIO*>(arg);
  TickType_t last = xTaskGetTickCount();
  while (self->samplerRunning_) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    self->sampleOnce();
  }
  vTaskDelete(nullptr);  // self-terminate once stopInputSampler() clears the flag
}

void HalGPIO::startInputSampler() {
  if (samplerRunning_) {
    return;
  }
  samplerRunning_ = true;
  sampleOnce();  // prime so the first loop iteration sees current state
  // Priority above the Arduino loop task (1) so the 10ms cadence holds even while
  // the loop task is busy in a long build slice. 2 KB stack: the sampler's deepest
  // path (inputMgr.update → analogRead) was measured at ~380 bytes peak on device,
  // so this leaves >4x headroom while staying small (this codebase is sensitive to
  // stack-into-heap spills). Watch the btnSampler high-water [MEM] line if changed.
  xTaskCreate(&HalGPIO::samplerTask, "btnsample", 2048, this, 2, &samplerTaskHandle_);
}

void HalGPIO::stopInputSampler() {
  if (!samplerRunning_) {
    return;
  }
  // Signal the task to exit on its next wake and self-delete. Avoids vTaskDelete()
  // tearing it down mid-analogRead (which would leak the ADC driver mutex).
  samplerRunning_ = false;
  samplerTaskHandle_ = nullptr;
}

bool HalGPIO::hasPendingInput() const {
  bool pending = false;
  portENTER_CRITICAL_SAFE(const_cast<portMUX_TYPE*>(&inputMux_));
  pending = accumPressed_ != 0;
  portEXIT_CRITICAL_SAFE(const_cast<portMUX_TYPE*>(&inputMux_));
  return pending;
}

bool HalGPIO::popButtonEdge(ButtonEdge& out) {
  bool got = false;
  portENTER_CRITICAL(&inputMux_);
  if (edgeHead_ != edgeTail_) {
    out = edgeBuf_[edgeHead_];
    edgeHead_ = (edgeHead_ + 1) % EDGE_BUF;
    got = true;
  }
  portEXIT_CRITICAL(&inputMux_);
  return got;
}

void HalGPIO::flushButtonEdges() {
  portENTER_CRITICAL(&inputMux_);
  edgeHead_ = 0;
  edgeTail_ = 0;
  accumPressed_ = 0;
  accumReleased_ = 0;
  portEXIT_CRITICAL(&inputMux_);
  snapPressed_ = 0;
  snapReleased_ = 0;
}

void HalGPIO::update() {
  if (samplerRunning_) {
    // Drain the sampler's accumulated edges + latest state into the loop-side
    // snapshot. No edge seen since the last drain is ever lost, regardless of how
    // long this loop iteration took.
    portENTER_CRITICAL(&inputMux_);
    snapState_ = liveState_;
    snapPressed_ = accumPressed_;
    snapReleased_ = accumReleased_;
    accumPressed_ = 0;
    accumReleased_ = 0;
    portEXIT_CRITICAL(&inputMux_);
  } else {
    // Pre-sampler (early boot): sample synchronously on the calling task.
    sampleOnce();
    portENTER_CRITICAL(&inputMux_);
    snapState_ = liveState_;
    snapPressed_ = accumPressed_;
    snapReleased_ = accumReleased_;
    accumPressed_ = 0;
    accumReleased_ = 0;
    portEXIT_CRITICAL(&inputMux_);
  }
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return (snapState_ & (1u << buttonIndex)) != 0; }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return (snapPressed_ & (1u << buttonIndex)) != 0; }

bool HalGPIO::wasAnyPressed() const { return snapPressed_ != 0; }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return (snapReleased_ & (1u << buttonIndex)) != 0; }

bool HalGPIO::wasAnyReleased() const { return snapReleased_ != 0; }

unsigned long HalGPIO::getHeldTime() const { return samplerRunning_ ? heldTimeSnapshot_ : inputMgr.getHeldTime(); }

void HalGPIO::waitForStablePowerRelease() {
  // Wait until the raw power-button pin reads HIGH (released) for RELEASE_STABLE_MS
  // consecutive milliseconds.  The InputManager debounce (5 ms) is too short for
  // mechanical switch bounce which can last 10-50 ms, so we bypass it entirely here.
  constexpr unsigned long RELEASE_STABLE_MS = 200;
  const unsigned long waitStart = millis();
  unsigned long stableStart = 0;
  while (true) {
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == HIGH) {
      if (stableStart == 0) stableStart = millis();
      if (millis() - stableStart >= RELEASE_STABLE_MS) break;
    } else {
      stableStart = 0;
    }
    delay(10);
  }
  LOG_DBG("GPIO", "Power button stable-released after %lu ms", millis() - waitStart);
}

void HalGPIO::startDeepSleep() {
  LOG_DBG("GPIO", "startDeepSleep: waiting for power button release (isPressed=%d, rawPin=%d)",
          inputMgr.isPressed(BTN_POWER), digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);
  waitForStablePowerRelease();
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  LOG_DBG("GPIO", "startDeepSleep: entering deep sleep now");
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  // The wakeup reason was already confirmed as a power button press before this is called,
  // so we know a real press occurred. When short presses are allowed, nothing more to verify.
  if (shortPressAllowed) {
    LOG_DBG("GPIO", "verifyPowerButtonWakeup: shortPressAllowed, skipping hold verification");
    return;
  }

  // Calibrate: subtract boot time already elapsed, assuming button held since boot.
  // Never collapse to less than BOUNCE_TOLERANCE_MS so the hold loop always has time to
  // sample the button and detect a release (early release = unintentional tap).
  constexpr unsigned long BOUNCE_TOLERANCE_MS = 100;
  const uint16_t calibration = millis();
  const uint16_t calibratedDuration =
      (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : BOUNCE_TOLERANCE_MS;
  LOG_DBG("GPIO", "verifyPowerButtonWakeup: requiredMs=%u, calibration=%u, calibratedMs=%u", requiredDurationMs,
          calibration, calibratedDuration);

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state after boot
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  LOG_DBG("GPIO", "verifyPowerButtonWakeup: initial detect took %lu ms, isPressed=%d, rawPin=%d", millis() - start,
          inputMgr.isPressed(BTN_POWER), digitalRead(InputManager::POWER_BUTTON_PIN) == LOW);

  if (inputMgr.isPressed(BTN_POWER)) {
    // Monitor the hold for calibratedDuration, tolerating brief bounces up to BOUNCE_TOLERANCE_MS.
    // Early release beyond the bounce window means an unintentional tap — go back to sleep.
    unsigned long lastSeenPressed = millis();
    const auto holdStart = millis();
    unsigned long bounceCount = 0;

    while (millis() - holdStart < calibratedDuration) {
      delay(10);
      inputMgr.update();
      if (inputMgr.isPressed(BTN_POWER)) {
        if (millis() - lastSeenPressed > 20) {
          bounceCount++;
        }
        lastSeenPressed = millis();
      } else if (millis() - lastSeenPressed >= BOUNCE_TOLERANCE_MS) {
        LOG_DBG("GPIO", "verifyPowerButtonWakeup: released early after %lu ms (bounces=%lu), going to sleep",
                millis() - holdStart, bounceCount);
        startDeepSleep();
      }
    }
    LOG_DBG("GPIO", "verifyPowerButtonWakeup: hold verified after %lu ms (bounces=%lu), proceeding with boot",
            millis() - holdStart, bounceCount);
  } else {
    LOG_DBG("GPIO", "verifyPowerButtonWakeup: button not pressed after 1s wait, going to sleep");
    startDeepSleep();
  }
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: GPIO20 is repurposed as I2C SDA, so the X4 pin-level USB detect is
    // unusable here — the I2C pull-ups would always report HIGH. Probe the
    // BQ27220 fuel gauge instead. Using just Current() mis-reports "not
    // connected" when the battery is full (current ~= 0 mA); combine it with
    // the Flags() DSG bit so we report true whenever the charger is present
    // (DSG=0 means charging or fully charged, not discharging).
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      uint16_t flags = 0;
      if (X3GPIO::readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_FLAGS_REG, &flags)) {
        if ((flags & BQ27220_FLAG_DSG) == 0) {
          return true;
        }
        int16_t currentMa = 0;
        if (X3GPIO::readBQ27220CurrentMA(&currentMa) && currentMa > 0) {
          return true;
        }
        return false;
      }
      delay(2);
    }
    return false;
  }
  // X4: U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  // X3: a POWERON reset can come from either a power-button press (battery-latch
  // MOSFET closes) OR from USB VBUS supplying power to the MCU through a path
  // that bypasses the latch — notably after a Quick Resume sleep, where GPIO13
  // is driven LOW so the MCU is otherwise unpowered. Disambiguate by reading
  // the pulled-up power-button pin directly: GPIO3 reads LOW only while the
  // button is held. If it's not held, treat a USB-connected POWERON as
  // AfterUSBPower so the early handler in setup() puts the device straight
  // back to sleep, matching X4. inputMgr.begin() in gpio.begin() has already
  // configured INPUT_PULLUP on POWER_BUTTON_PIN by the time we're called.
  if (deviceIsX3() && wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON) {
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
      return WakeupReason::PowerButton;
    }
    return isUsbConnected() ? WakeupReason::AfterUSBPower : WakeupReason::PowerButton;
  }

  const bool usbConnected = isUsbConnected();
  LOG_DBG("GPIO", "getWakeupReason: wakeupCause=%d, resetReason=%d, usbConnected=%d", static_cast<int>(wakeupCause),
          static_cast<int>(resetReason), usbConnected);

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
