#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

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

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

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

bool HalGPIO::isHeldNow(uint8_t buttonIndex, uint8_t confirmSamples) {
  constexpr unsigned long SAMPLE_GAP_MS = 10;
  // Raw values from the deciding sample, logged below. A negative verdict on the ADC
  // ladder has two very different causes — the button genuinely is not held, or its
  // divider reads outside the band the firmware expects — and only the raw value tells
  // them apart. Worth a line: the boot-time callers run before any UI exists to report a
  // misdetected combo, so the log is the sole evidence.
  int raw1 = -1;
  int raw2 = -1;
  int classified1 = -1;
  int classified2 = -1;
  bool held = true;

  for (uint8_t i = 0; i < confirmSamples; i++) {
    if (i > 0) delay(SAMPLE_GAP_MS);
    if (buttonIndex == BTN_POWER) {
      if (digitalRead(InputManager::POWER_BUTTON_PIN) != LOW) {
        held = false;
        break;
      }
      continue;
    }
    InputManager::ButtonAdcSample group1{};
    InputManager::ButtonAdcSample group2{};
    inputMgr.readButtonAdc(group1, group2);
    raw1 = group1.raw;
    raw2 = group2.raw;
    classified1 = group1.button;
    classified2 = group2.button;
    if (group1.button != buttonIndex && group2.button != buttonIndex) {
      held = false;
      break;
    }
  }

  if (buttonIndex != BTN_POWER) {
    LOG_DBG("GPIO", "isHeldNow(btn=%u) -> %d (adc1 raw=%d btn=%d, adc2 raw=%d btn=%d)", buttonIndex, held ? 1 : 0, raw1,
            classified1, raw2, classified2);
  }
  return held;
}

const char* HalGPIO::wakeVerdictName(WakeVerdict verdict) {
  switch (verdict) {
    case WakeVerdict::NotPressed:
      return "not-pressed";
    case WakeVerdict::ShortPress:
      return "short";
    case WakeVerdict::LongHold:
      return "long-hold";
    case WakeVerdict::DoubleClick:
      return "double-click";
    case WakeVerdict::ReleasedEarly:
      return "released-early";
    case WakeVerdict::NoSecondPress:
      return "no-second-press";
  }
  return "?";
}

HalGPIO::WakeCheck HalGPIO::verifyPowerButtonWakeup(WakeGestures gestures, uint16_t requiredDurationMs) {
  constexpr unsigned long BOUNCE_TOLERANCE_MS = 100;
  constexpr unsigned long POLL_INTERVAL_MS = 10;
  // Mirrors ButtonEventManager::DOUBLE_WINDOW_MS so a double-click-to-sleep gesture
  // wakes on the same cadence it was configured with.
  constexpr unsigned long DOUBLE_WINDOW_MS = 300;

  // Must be called before any long-running init (it is the first statement of setup()) so a short
  // press cannot be hidden by boot work: a released button is detected below and returns
  // immediately, which is also why running this on every boot costs nothing on non-button resets.
  const unsigned long gateStart = millis();
  const auto stamp = [](unsigned long ms) { return static_cast<uint16_t>(ms > UINT16_MAX ? UINT16_MAX : ms); };

  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(InputManager::POWER_BUTTON_PIN) != LOW) {
    return {WakeVerdict::NotPressed, stamp(millis()), 0};
  }

  if (gestures.shortAllowed) {
    return {WakeVerdict::ShortPress, stamp(millis()), stamp(millis() - gateStart)};
  }

  // requiredDurationMs (longHold) is compared against millis() directly — time since app
  // start, NOT time since this call. That is deliberate: the press that caused the wake
  // began before setup() ran, so boot time counts toward the hold rather than being
  // charged to the user twice. The caveat is that millis() starts at app init and so
  // excludes the ~200-300 ms bootloader, making the real-world hold needed that much
  // longer than the configured value. Erring long is the safe direction — the failure
  // mode being guarded against is a stray tap waking the device.
  //
  // Watch the first press through to release, classifying it exactly like the awake FSM
  // (ButtonEventManager): held past requiredDurationMs is a long press; released earlier
  // followed by a second press within DOUBLE_WINDOW_MS is a double click; otherwise it's
  // a short tap. Whichever it turns out to be, accept it only if that gesture is enabled.
  unsigned long lastSeenPressed = millis();
  while (true) {
    const unsigned long now = millis();
    if (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
      lastSeenPressed = now;
      if (gestures.longHold && now >= requiredDurationMs) {
        return {WakeVerdict::LongHold, stamp(now), stamp(now - gateStart)};
      }
    } else if (now - lastSeenPressed >= BOUNCE_TOLERANCE_MS) {
      // Released before the long-hold threshold. A double-click gesture gets one more
      // chance: a second press within the window after this release.
      const uint16_t heldMs = stamp(lastSeenPressed - gateStart);
      if (!gestures.doubleClick) {
        return {WakeVerdict::ReleasedEarly, stamp(now), heldMs};
      }
      const unsigned long releaseTime = now;
      while (millis() - releaseTime < DOUBLE_WINDOW_MS) {
        if (digitalRead(InputManager::POWER_BUTTON_PIN) == LOW) {
          return {WakeVerdict::DoubleClick, stamp(millis()), heldMs};
        }
        delay(POLL_INTERVAL_MS);
      }
      return {WakeVerdict::NoSecondPress, stamp(millis()), heldMs};
    }
    delay(POLL_INTERVAL_MS);
  }
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: GPIO20 is repurposed as I2C SDA, so the X4 pin-level USB detect is
    // unusable here — the I2C pull-ups would always report HIGH. Probe the
    // BQ27220 fuel gauge instead.
    //
    // Current() is the primary signal: it is signed, and current flowing INTO
    // the battery (positive, above a noise floor) only happens on a charger.
    // DSG alone must NOT be trusted as "charger present": the gauge clears DSG
    // whenever the pack isn't actively discharging above its detection
    // threshold, which includes plain idle/relaxation on battery. Treating
    // DSG=0 as connected made the UI latch to the charging bolt shortly after
    // unplugging and never recover (issue #86). FC (fully charged) is the one
    // resting state that does imply a charger, since it only latches while
    // topped off on the charger, so it stays as a secondary signal.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      uint16_t flags = 0;
      if (X3GPIO::readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_FLAGS_REG, &flags)) {
        const bool discharging = (flags & BQ27220_FLAG_DSG) != 0;
        if (!discharging && (flags & BQ27220_FLAG_FC) != 0) {
          return true;  // topped off on the charger
        }
        int16_t currentMa = 0;
        if (X3GPIO::readBQ27220CurrentMA(&currentMa) && currentMa > USB_CHARGE_CURRENT_MIN_MA) {
          return true;  // measurable current into the battery
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

  // A GPIO deep-sleep wake means POWER_BUTTON_PIN was pulled LOW, which is a button press by
  // definition — whatever the power source. This clause used to also require usbConnected, on the
  // premise that on battery the MCU is fully powered down so every wake arrives as POWERON.
  // HalPowerManager::startDeepSleep(keepClockAlive=true) broke that premise: with the clock
  // enabled on X4 (enterDeepSleep's keepLpAlive) GPIO13 is held HIGH, the MCU stays powered
  // through sleep, and a battery wake arrives as GPIO+DEEPSLEEP with no USB. That combination
  // matched nothing and fell through to Other, so setup() skipped the hold verification entirely
  // and any tap woke the device. Plugging USB does not pull this pin low, so the AfterUSBPower
  // case below (POWERON + USB) is unaffected.
  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP)) {
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
