#include "HalPowerManager.h"

#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

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
  const bool foreignLock =
      currentLockMode.load(std::memory_order_relaxed) != None && lockOwnerTask_ != xTaskGetCurrentTaskHandle();
  // Already at the low clock from idle power saving — leave ownership of the
  // restore with setPowerSaving(); don't double-track it here.
  if (!foreignLock && !isLowPower && setCpuFrequencyMhz(LOW_POWER_FREQ)) {
    waveformLowPower_ = true;
    LOG_DBG("PWR", "Waveform wait: CPU down to %d MHz", LOW_POWER_FREQ);
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
      LOG_DBG("PWR", "Waveform wait: CPU restored to %d MHz", normalFreq);
    }
  }
  xSemaphoreGive(modeMutex);
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
  if (gpio.deviceIsX3()) {
    freeink::PowerManager::powerDownRailsForSleep();
    esp_sleep_config_gpio_isolate();
    gpio_deep_sleep_hold_en();
  } else {
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
  }
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);

  // Now wait for the power button to be fully released before arming the wakeup
  // trigger and entering sleep — prevents immediate re-wake from a held button.
  gpio.waitForStablePowerRelease();

  // Arm the wakeup trigger *after* the button is released
  // Note: when keepClockAlive is false, this is only useful for waking up on USB power. On battery, the MCU will be
  // completely powered off, so the power button is hard-wired to briefly provide power to the MCU, waking it up
  // regardless of the wakeup source configuration.
  // When keepClockAlive is true, this is the actual wakeup mechanism since the MCU stays powered.
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
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
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode.load(std::memory_order_relaxed) != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode.store(NormalSpeed, std::memory_order_relaxed);
    powerManager.lockOwnerTask_ = xTaskGetCurrentTaskHandle();
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode.store(None, std::memory_order_relaxed);
    powerManager.lockOwnerTask_ = nullptr;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
