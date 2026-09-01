#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

// The C3 flash SPIWP pad, unused in these boards' DIO flash mode and rewired as a
// power control on the Xteink C3 units (see lightSleep()).
static constexpr gpio_num_t GPIO_BATTERY_LATCH = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Relaxed atomic read: a slightly stale value is acceptable (the lock holder
  // that just won the race will re-call setPowerSaving anyway), but we want
  // defined semantics rather than relying on compiler behavior for a plain int.
  const LockMode mode = currentLockMode.load(std::memory_order_relaxed);

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::ensureFullSpeedForRadio() {
  if (normalFreq <= 0) {
    return;  // begin() not called yet — nothing to restore to
  }
  // Clear BOTH downclock owners before touching the frequency. The idle governor
  // (isLowPower) and the waveform hook (waveformLowPower_) track their drops separately, and
  // whichever one is live would otherwise restore or re-drop the clock behind the radio's back.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const bool wasIdleLow = isLowPower;
  const bool wasWaveformLow = waveformLowPower_;
  isLowPower = false;
  waveformLowPower_ = false;
  xSemaphoreGive(modeMutex);

  const int current = getCpuFrequencyMhz();
  if (current >= normalFreq) {
    return;  // already at full speed; nothing to do beyond the flag reconciliation above
  }
  if (setCpuFrequencyMhz(normalFreq)) {
    LOG_INF("PWR", "CPU %d -> %d MHz for radio bring-up (idleLow=%d waveformLow=%d)", current, normalFreq,
            wasIdleLow ? 1 : 0, wasWaveformLow ? 1 : 0);
  } else {
    // Worth an error rather than a debug line: WiFi association from a 10 MHz clock does not
    // fail cleanly, it hangs, and this is the only place that would have seen it coming.
    LOG_ERR("PWR", "Failed to restore %d MHz before radio bring-up (still %d MHz) — WiFi may hang", normalFreq,
            current);
  }
}

void HalPowerManager::enterWaveformWait() {
  if (normalFreq <= 0) {
    return;  // begin() not called yet — nothing to restore to
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    return;  // WiFi requires the 80 MHz APB clock
  }
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  // A NormalSpeed lock held by ANOTHER task means full-speed code is running
  // concurrently — don't downclock. Our own lock (the render task's per-render
  // Lock) is fine: this task is about to sleep and only resumes after
  // exitWaveformWait() restores the clock.
  //
  // More than one holder means at least one of them is not us, whoever lockOwnerTask_ names —
  // that is the build-slice case, where the loop task is laying out pages while this task waits
  // on a waveform. Treat it as foreign: never downclock on a maybe.
  const bool foreignLock = currentLockMode.load(std::memory_order_relaxed) != None &&
                           (lockCount_ > 1 || lockOwnerTask_ != xTaskGetCurrentTaskHandle());
  // Already at the low clock from idle power saving — leave ownership of the
  // restore with setPowerSaving(); don't double-track it here.
  if (!foreignLock && !isLowPower && setCpuFrequencyMhz(LOW_POWER_FREQ)) {
    waveformLowPower_ = true;
    LOG_TRC("PWR", "Waveform wait: CPU down to %d MHz", LOW_POWER_FREQ);
  }
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::exitWaveformWait() {
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  if (waveformLowPower_) {
    waveformLowPower_ = false;
    // If idle power saving engaged meanwhile (loop task), the low clock is now
    // intentional — keep it and let setPowerSaving(false) restore it later.
    if (!isLowPower) {
      setCpuFrequencyMhz(normalFreq);
      LOG_TRC("PWR", "Waveform wait: CPU restored to %d MHz", normalFreq);
    }
  }
  xSemaphoreGive(modeMutex);
}

// Ported from crosspoint-reader PR #2525 (Brian Pugh / @BrianPugh); the Xteink C3
// guard on the GPIO13 hold follows PR #2998 (Justin Mitchell / @itsthisjustin).
// The xTaskCatchUpTicks() correction at the end is this fork's own addition — see
// the comment there for why our background button sampler needs it.
bool HalPowerManager::lightSleep(const HalGPIO& gpio) {
  const unsigned long entryMs = millis();
  if (lastSliceEndMs_ != 0) {
    lightSleepStats_.awakeMs += entryMs - lastSliceEndMs_;
  }
  lightSleepStats_.attempts++;
  lastSliceEndMs_ = entryMs;  // overwritten with the post-sleep stamp if we do sleep

  // Light sleep stops the LEDC peripheral's clock, so an ESP-driven PWM
  // frontlight visibly flickers as the idle loop enters slice after slice.
  // Justin Mitchell (@itsthisjustin) reported this against crosspoint PR #2983
  // ("feat: Add support for x4pro & papermono devices"), where the guard reads
  // `Frontlight.present() && Frontlight.isOn()`. This fork has no frontlight
  // driver, so it cannot ask whether the light is currently lit — decline on any
  // board that HAS one. That costs nothing today (no board we build has a
  // frontlight, so this never fires and the counter stays 0) and keeps the X4
  // Pro correct the moment its profile is selectable. Tighten to
  // present-and-on once a frontlight HAL exists, so those boards can still
  // idle-sleep with the light off.
  if (BoardConfig::hasPwmFrontlight()) {
    lightSleepStats_.rejFrontlight++;
    return false;
  }

  // A performance Lock means a render (or similar) task is mid-flight; light sleep
  // freezes the whole chip, so it would stall that task. Read without the mutex,
  // like setPowerSaving(): stale in either direction is acceptable — a stale lock
  // delays sleeping by one slice, and a Lock acquired after this check freezes that
  // task for at most one slice (timer wake; RAM and peripheral state are retained).
  if (currentLockMode.load(std::memory_order_relaxed) != None) {
    lightSleepStats_.rejLock++;
    return false;
  }
  // Light sleep drops a WiFi association and kills an enumerated USB-CDC link.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    lightSleepStats_.rejWifi++;
    return false;
  }
  if (gpio.isUsbConnectedCached()) {
    lightSleepStats_.rejUsb++;
    return false;
  }
  // A raw button-state change is still inside the debounce window: committing it
  // needs a second matching sample, so the caller should poll again quickly rather
  // than halt the chip — a tap shorter than the slice would otherwise land in a
  // single sample and be dropped, and every press would commit a slice late.
  if (gpio.isDebouncePending()) {
    lightSleepStats_.rejDebounce++;
    return false;
  }

  // Timer wake keeps the poll cadence bounded: the front/side buttons are ADC
  // resistor-ladder inputs whose pressed levels sit mid-rail, invisible to a
  // digital GPIO wake, so they can only be sampled awake. The power button (a
  // true GPIO) is ALSO armed, as a level wake at its pressed level, so a tap
  // shorter than one slice still gets the chip awake in time to sample it. That
  // wake is only an early poll — the sampler's debounce still decides whether a
  // real press happened, so a misread around the sleep transition costs one
  // extra wake, never a phantom press. Idle cost is zero: the pin only holds its
  // pressed level while a finger is on it.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(LIGHT_SLEEP_SLICE_MS) * 1000ULL);
  const int8_t powerPin = BoardConfig::ACTIVE.input.power;
  if (powerPin >= 0) {
    gpio_wakeup_enable(static_cast<gpio_num_t>(powerPin),
                       BoardConfig::ACTIVE.input.powerActiveHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
  }

  // GPIO13 is rewired as a power control on the Xteink C3 boards: the X4
  // battery-latch MOSFET gate, and the X3 SD-rail enable (BoardConfig's
  // XTEINK_X3 sd.powerEnable). Both are active-high and both must stay HIGH
  // while the device runs. The IDF flash-leakage workaround
  // (CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND) pulls the DIO-unused SPIWP pad
  // low on light-sleep entry, which would power an X4 off outright and cut the
  // X3's card out from under an open file. Drive the pad HIGH and pad-hold it:
  // the hold latches the level and overrides both the sleep-time pull and any
  // pad re-muxing on the wake path. It is left enabled while running — the wake
  // path may restore flash-pad muxing, so even a brief release between slices
  // can drop it. Nothing downstream is blocked by that: both deep-sleep paths
  // release the hold first (startDeepSleep()'s own gpio_hold_dis on X4,
  // holdRailOff()'s inside powerDownRailsForSleep() on X3).
  // Level is set BEFORE direction so the pad never glitches low on the way to
  // output mode. Non-Xteink boards are left alone — GPIO13 is an ordinary signal
  // there (SPI MOSI on the LilyGo T5S3, display CS on the X4 Pro).
  if (gpio.isXteinkDevice()) {
    // gpio_hold_dis() first: a hold left over from an earlier slice (or from a previous
    // sleep cycle) makes gpio_set_level() a silent no-op, so without this the pin would
    // keep whatever level it was latched at — the exact failure mode documented at
    // startDeepSleep()'s own gpio_hold_dis below. Re-holding immediately after is free.
    gpio_hold_dis(GPIO_BATTERY_LATCH);
    gpio_set_level(GPIO_BATTERY_LATCH, 1);
    gpio_set_direction(GPIO_BATTERY_LATCH, GPIO_MODE_OUTPUT);
    gpio_hold_en(GPIO_BATTERY_LATCH);
  }

  const unsigned long sleepStart = millis();
  const esp_err_t err = esp_light_sleep_start();
  const unsigned long sleptMs = millis() - sleepStart;

  // Disarm immediately: an armed timer wake persists across sleep calls and would
  // carry over into startDeepSleep(), waking the device on USB power after one
  // slice. gpio_wakeup_disable() clears only the wake-enable bit — the pin's LEVEL
  // interrupt type survives it, and a leftover level type is live ammunition for
  // any later-installed GPIO ISR service (an asserted level with no per-pin
  // handler re-enters the shared ISR forever). Clear the type explicitly.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (powerPin >= 0) {
    gpio_wakeup_disable(static_cast<gpio_num_t>(powerPin));
    gpio_set_intr_type(static_cast<gpio_num_t>(powerPin), GPIO_INTR_DISABLE);
  }

  if (err != ESP_OK) {
    // e.g. ESP_ERR_SLEEP_REJECT — we did not actually sleep
    lightSleepStats_.rejIdf++;
    LOG_DBG("PWR", "Light sleep rejected: %d", static_cast<int>(err));
    return false;
  }

  lightSleepStats_.slept++;
  lightSleepStats_.sleptMs += sleptMs;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    lightSleepStats_.wakeGpio++;  // power button pressed mid-slice
  } else {
    lightSleepStats_.wakeTimer++;
  }
  // Restart the awake stopwatch from here, so awakeMs counts only time the chip
  // was actually running between slices, not the slices themselves.
  lastSliceEndMs_ = millis();

  // esp_light_sleep_start() corrects esp_timer (so millis() stays wall-clock
  // honest) but NOT the FreeRTOS tick, which simply stops for the duration
  // (sleep_modes.c only calls esp_timer_private_set/esp_set_time_from_rtc).
  // Uncorrected, every tick-paced waiter stretches by the slice: the 10 ms
  // button sampler task would only get one pass per ~10 slices, turning idle
  // input latency into half a second, and any delay() elsewhere would run long.
  // Step the tick forward by the measured sleep so blocked tasks come due on
  // wall-clock time. Tasks whose deadline passed are released here, which is why
  // this is xTaskCatchUpTicks() and not vTaskStepTick().
  xTaskCatchUpTicks(pdMS_TO_TICKS(sleptMs));
  return true;
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio, bool keepClockAlive) const {
  LOG_DBG("PWR", "startDeepSleep: isPressed=%d, rawPin=%d, keepClock=%d", gpio.isPressed(HalGPIO::BTN_POWER),
          digitalRead(InputManager::POWER_BUTTON_PIN) == LOW, keepClockAlive);

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif
  // Perform all hardware preparation immediately (while the button may still be held)
  // so the user gets instant visual feedback (display already off). Only block for
  // button release at the very end, right before entering sleep.
  // GPIO13 on X4 is the battery latch MOSFET.
  // When keepClockAlive is false (default): GPIO13 goes LOW, the MCU is
  // completely powered off during sleep (including the LP timer / RTC memory).
  // When keepClockAlive is true: GPIO13 stays HIGH, the MCU remains powered
  // at ~3-4 mA so the LP timer keeps running and RTC memory is preserved.
  // This allows HalClock to accurately compute elapsed sleep time on wake.
  //
  // X3 does NOT share this meaning: there GPIO13 is the SD rail's power enable
  // (active-high), declared as sd.powerEnable in the XTEINK_X3 board profiles.
  // X3 keeps time on a battery-backed DS3231 (I2C 0x68), so it never needs the
  // LP timer held alive across sleep — main.cpp forces keepLpAlive=false on X3
  // for exactly that reason. So on X3 we hand the pin to the SDK's rail teardown
  // (which cuts the card's power and latches it off) instead of treating it as a
  // latch; SDCardManager::begin() releases that hold and re-powers on the next
  // boot. Doing both would make us a second, independent writer of the pin.
  //
  // Non-Xteink boards share neither meaning and take the rail-teardown path too:
  // GPIO13 is an ordinary signal there — SPI MOSI on the LilyGo T5S3
  // (BoardT5S3Pins.h), display chip select on the X4 Pro — so driving it as an
  // output and pad-holding it would clobber a live bus.
  // Those boards also ignore keepClockAlive: nothing cuts MCU power there, so
  // the LP timer and RTC memory survive deep sleep either way.
  // Ported from crosspoint-reader PR #2998 ("fix: Guard GPIO13 power control for
  // Xteink C3 boards only", Justin Mitchell / @itsthisjustin). His fix guards
  // lightSleep()/onEinkBusyWaitSlice(), which this fork does not have; here the
  // same predicate guards the one place we touch GPIO13.
  const bool gpio13IsBatteryLatch = gpio.isXteinkDevice() && !gpio.deviceIsX3();
  if (gpio13IsBatteryLatch) {
    constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
    // Release any GPIO hold from a previous sleep cycle (keepClockAlive=true leaves GPIO13 held after wake).
    // Without this, gpio_set_level() below silently fails and GPIO13 is stuck in its prior state,
    // causing the device to enter a sleep/wake loop that requires a hardware reset to escape.
    gpio_hold_dis(GPIO_SPIWP);
    gpio_deep_sleep_hold_dis();
    gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SPIWP, keepClockAlive ? 1 : 0);
    esp_sleep_config_gpio_isolate();
    gpio_deep_sleep_hold_en();
    gpio_hold_en(GPIO_SPIWP);
  } else {
    // Same release, for the boards that reach GPIO13 through the SDK's rail teardown
    // instead of the latch branch above. It matters on X3: lightSleep() re-holds GPIO13
    // HIGH on every idle slice for ALL Xteink C3 boards (isXteinkDevice()), but only the
    // X4 branch above released it — so powerDownRailsForSleep() could not drive the X3's
    // SD rail enable LOW and the card stayed powered through deep sleep, draining the
    // battery. Guarded to Xteink C3 for the same reason the branch above is: elsewhere
    // GPIO13 is an ordinary bus signal (SPI MOSI on the T5S3, display CS on the X4 Pro)
    // that nothing has held and that we must not touch.
    if (gpio.isXteinkDevice()) {
      gpio_hold_dis(GPIO_NUM_13);
    }
    gpio_deep_sleep_hold_dis();
    freeink::PowerManager::powerDownRailsForSleep();
    esp_sleep_config_gpio_isolate();
    gpio_deep_sleep_hold_en();
  }
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  if (sleepStepHook_) sleepStepHook_(SleepStep::RailsConfigured, 0, false, gpio13IsBatteryLatch);

  // Now wait for the power button to be fully released before arming the wakeup
  // trigger and entering sleep — prevents immediate re-wake from a held button.
  // Bounded (see waitForStablePowerRelease): a pin stuck LOW used to freeze the device
  // here with the sleep screen displayed and no watchdog covering this task.
  if (sleepStepHook_) sleepStepHook_(SleepStep::AwaitingRelease, 0, false, gpio13IsBatteryLatch);
  const unsigned long releaseWaitMs = gpio.waitForStablePowerRelease();
  const bool releaseTimedOut = releaseWaitMs >= HalGPIO::POWER_RELEASE_TIMEOUT_MS;
  if (sleepStepHook_) sleepStepHook_(SleepStep::ReleaseDone, releaseWaitMs, releaseTimedOut, gpio13IsBatteryLatch);

  // Arm the wakeup trigger *after* the button is released
  // Note: when keepClockAlive is false, this is only useful for waking up on USB power. On battery, the MCU will be
  // completely powered off, so the power button is hard-wired to briefly provide power to the MCU, waking it up
  // regardless of the wakeup source configuration.
  // When keepClockAlive is true, this is the actual wakeup mechanism since the MCU stays powered.
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (sleepStepHook_) sleepStepHook_(SleepStep::WakeArmed, releaseWaitMs, releaseTimedOut, gpio13IsBatteryLatch);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  // Guard against an X3 board mistakenly taking the ADC path: BAT_GPIO0 is
  // reused as X3_I2C_SCL on X3, so reading it as ADC would collide with the
  // fuel-gauge bus. _batteryUseI2C must match the detected device type.
  assert(_batteryUseI2C == gpio.deviceIsX3());
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC from the I2C fuel gauge via the shared helper so the transaction
    // shape stays consistent with other BQ27220/DS3231/QMI8658 reads.
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    uint16_t soc = 0;
    if (X3GPIO::readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc)) {
      _batteryCachedPercent = soc > 100 ? 100 : soc;
    }
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // Smooth the battery % with a 1/10-weight IIR. The cache stores the value
  // scaled ×10 so integer math keeps enough precision. Seed explicitly on the
  // first real sample; using 0 as a sentinel caused a second seed whenever a
  // later reading momentarily returned 0, producing visible jumps.
  const uint16_t sample = battery.readPercentage();
  if (!_batterySeeded) {
    _batteryCachedPercent = 10 * sample;
    _batterySeeded = true;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + sample * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Counted, not exclusive: every Lock holds. See lockCount_ for what the old single-slot version
  // silently did to the second holder.
  if (powerManager.lockCount_ < UINT8_MAX) {
    powerManager.lockCount_++;
    valid = true;
  } else {
    LOG_ERR("PWR", "Lock count overflow; this lock holds nothing");  // cannot happen; not silent if it does
    valid = false;
  }
  if (valid && powerManager.lockCount_ == 1) {
    // First holder owns the flag and is the task enterWaveformWait() compares against.
    powerManager.currentLockMode.store(NormalSpeed, std::memory_order_relaxed);
    powerManager.lockOwnerTask_ = xTaskGetCurrentTaskHandle();
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode. Unconditional, not
    // just for the first holder: a nested lock wants full speed as much as the outer one does.
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid && powerManager.lockCount_ > 0 && --powerManager.lockCount_ == 0) {
    powerManager.currentLockMode.store(None, std::memory_order_relaxed);
    powerManager.lockOwnerTask_ = nullptr;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
