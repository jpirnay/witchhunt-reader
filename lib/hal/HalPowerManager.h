#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  // I2C fuel gauge configuration for X3 battery monitoring
  bool _batteryUseI2C = false;                   // True if using I2C fuel gauge (X3), false for ADC (X4)
  mutable int _batteryCachedPercent = 0;         // Last read battery percentage — X3: 0-100, X4: 0-1000 (scaled)
  mutable bool _batterySeeded = false;           // True once the smoothing filter has a first real sample (X4)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  std::atomic<LockMode> currentLockMode{None};
  SemaphoreHandle_t modeMutex = nullptr;  // Protect Lock acquire/release ordering
  // Task that holds the current NormalSpeed lock (nullptr when none). Guarded by
  // modeMutex. Needed by enterWaveformWait(): the render task holds a Lock for
  // the whole render() pass, and the waveform wait happens inside it — the
  // downclock is safe when the lock holder IS the waiting task (its code only
  // resumes after exitWaveformWait() restores the clock), but not when another
  // task holds the lock and keeps running.
  TaskHandle_t lockOwnerTask_ = nullptr;
  // True while the CPU clock is dropped for an e-ink waveform wait (see
  // enterWaveformWait / exitWaveformWait). Guarded by modeMutex.
  bool waveformLowPower_ = false;

 public:
  // Idle light-sleep instrumentation, all written from the loop task only. Plain
  // members rather than RTC memory: light sleep retains RAM, so they survive every
  // slice, and a deep sleep ends the session they describe anyway. `awakeMs` is
  // wall time between consecutive lightSleep() calls, so sleptMs/(sleptMs+awakeMs)
  // is the idle duty cycle — the closest proxy for average current without a meter.
  struct LightSleepStats {
    uint32_t attempts = 0;     // calls to lightSleep()
    uint32_t slept = 0;        // calls that actually halted the chip
    uint32_t sleptMs = 0;      // total time halted
    uint32_t awakeMs = 0;      // total time between slices
    uint32_t wakeTimer = 0;    // woke on the slice timer (the normal case)
    uint32_t wakeGpio = 0;     // woke early on the power button
    uint32_t rejLock = 0;      // declined: a render Lock was held
    uint32_t rejWifi = 0;      // declined: WiFi up
    uint32_t rejUsb = 0;       // declined: host attached
    uint32_t rejDebounce = 0;  // declined: button change mid-debounce
    uint32_t rejIdf = 0;       // esp_light_sleep_start() returned non-OK
  };

 private:
  LightSleepStats lightSleepStats_;
  unsigned long lastSliceEndMs_ = 0;

 public:
  static constexpr int LOW_POWER_FREQ = 10;  // MHz

  // Two-stage idle backoff. Renders re-raise the clock via Lock regardless, so the
  // full-speed window only needs to cover rapid consecutive input (avoids clock
  // thrash); polling stays at 100 Hz until light sleep takes over the cadence.
  static constexpr unsigned long IDLE_DOWNCLOCK_MS = 500;     // full speed -> LOW_POWER_FREQ
  static constexpr unsigned long IDLE_LIGHT_SLEEP_MS = 1000;  // 100 Hz polling -> light sleep

  static constexpr unsigned long BATTERY_POLL_MS = 1500;     // ms
  static constexpr unsigned long LIGHT_SLEEP_SLICE_MS = 50;  // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Waveform-wait power hooks, installed on the display driver by HalDisplay:
  // drop the CPU clock while the render task sleeps on the e-ink BUSY-ISR
  // semaphore (nothing can run during the waveform — background work gates on
  // isRefreshPending()/the render lock), restore it before the post-waveform
  // SPI work. enterWaveformWait() is a no-op when WiFi is active, a
  // NormalSpeed lock is held by ANOTHER task, or the CPU is already in idle
  // low-power mode. The render task's own per-render Lock does not block it:
  // that holder is the waiting task itself and only resumes after the clock
  // is restored. Runs on the render task; tolerant of the loop task's
  // concurrent setPowerSaving() calls (same relaxed model as setPowerSaving).
  void enterWaveformWait();
  void exitWaveformWait();

  // Light-sleep the CPU for LIGHT_SLEEP_SLICE_MS (timer wake; buttons are polled on
  // wake at the same cadence as the delay() this replaces). Returns false WITHOUT
  // sleeping when unsafe: a performance Lock is held (render in flight), WiFi is
  // active, USB is connected (light sleep kills the CDC link), or a button change
  // is mid-debounce. The caller must fall back to delay() in that case. Call from
  // the main loop only — light sleep halts the whole chip, including the button
  // sampler task.
  bool lightSleep(const HalGPIO& gpio);

  // Idle light-sleep counters since boot, for the System Information screen. That
  // screen is the only practical way to read them: the CDC guard means light sleep
  // is off for as long as a serial monitor is attached, so a serial log of these
  // would only ever print zeroes.
  const LightSleepStats& lightSleepStats() const { return lightSleepStats_; }

  // Setup wake up GPIO and enter deep sleep.
  // When keepClockAlive is true, GPIO13 stays HIGH so the LP timer keeps
  // running during sleep (~3-4 mA extra).  This allows HalClock to compute
  // elapsed sleep time and restore the wall clock accurately on wake.
  void startDeepSleep(HalGPIO& gpio, bool keepClockAlive = false) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
