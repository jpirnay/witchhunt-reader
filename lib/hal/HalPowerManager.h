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
  // active, or USB is connected (light sleep kills the CDC link). The caller must
  // fall back to delay() in that case. Call from the main loop only — light sleep
  // halts the whole chip, including the button sampler task.
  bool lightSleep(const HalGPIO& gpio) const;

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
