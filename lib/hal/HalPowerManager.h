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
  // Locks nest. They used to be a single slot: a second concurrent Lock logged an error and was
  // handed back holding NOTHING, and the first one's release then cleared the flag while the
  // second holder was still running. Two real holders overlap constantly — the render task takes
  // one per render, the loop task takes one per build slice — so the effect was that a build
  // slice ran unprotected, and enterWaveformWait() saw no foreign lock and downclocked the chip
  // underneath it. Counted instead: the flag is set while any lock is held and cleared by the
  // last release.
  uint8_t lockCount_ = 0;
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
    uint32_t attempts = 0;       // calls to lightSleep()
    uint32_t slept = 0;          // calls that actually halted the chip
    uint32_t sleptMs = 0;        // total time halted
    uint32_t awakeMs = 0;        // total time between slices
    uint32_t wakeTimer = 0;      // woke on the slice timer (the normal case)
    uint32_t wakeGpio = 0;       // woke early on the power button
    uint32_t rejLock = 0;        // declined: a render Lock was held
    uint32_t rejWifi = 0;        // declined: WiFi up
    uint32_t rejUsb = 0;         // declined: host attached
    uint32_t rejFrontlight = 0;  // declined: board has a PWM frontlight
    uint32_t rejDebounce = 0;    // declined: button change mid-debounce
    uint32_t rejIdf = 0;         // esp_light_sleep_start() returned non-OK
  };

  // The last steps of startDeepSleep(), reported to an optional observer. That
  // function does not return, so the only way anything downstream can learn how far it
  // got is to be told on the way past.
  enum class SleepStep : uint8_t {
    RailsConfigured,  // GPIO13 / rail teardown done, wake pin pulled up
    AwaitingRelease,  // about to block on the power-button release
    ReleaseDone,      // release seen, or the wait gave up (see `timedOut`)
    WakeArmed,        // wake source armed; esp_deep_sleep_start() is the next statement
  };
  // Plain function pointer, not std::function: the latter costs ~2-4 KB of binary per
  // signature and heap-allocates its closure, and this is called on the sleep path where
  // the heap may already be why we are sleeping. Owned by the app layer so the HAL takes
  // no dependency on the diagnostics module.
  //
  // `storageLive` says whether the SD rail is still powered at this point in the teardown,
  // which only the HAL knows: the battery-latch branch leaves the card alone, while the
  // rail-teardown branch has already cut it via powerDownRailsForSleep(). An observer that
  // wants to persist something from inside the sleep path has to be told, or it writes
  // into a card that is no longer there.
  using SleepStepHook = void (*)(SleepStep step, unsigned long waitedMs, bool timedOut, bool storageLive);
  void setSleepStepHook(SleepStepHook hook) { sleepStepHook_ = hook; }

 private:
  LightSleepStats lightSleepStats_;
  SleepStepHook sleepStepHook_ = nullptr;
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

  // Force the CPU back to its normal frequency before bringing the radio up.
  //
  // CALL THIS IMMEDIATELY BEFORE WiFi.mode()/WiFi.begin()/softAP(). WiFi does not work below
  // 80 MHz on this SoC — the same fact enterWaveformWait() relies on when it refuses to
  // downclock while WiFi is up — and LOW_POWER_FREQ is 10 MHz. Bringing the radio up from
  // there wedges PHY init.
  //
  // setPowerSaving(false) is NOT a substitute. It only acts when its own isLowPower flag is
  // set, and it forces enabled=false only once WiFi.getMode() is already non-null — i.e. it
  // protects a radio that is already up, and nothing raised the clock on the way in. It is also
  // blind to a clock dropped by enterWaveformWait(), which tracks its own waveformLowPower_
  // flag; that combination leaves the CPU at 10 MHz with isLowPower false, so every later
  // setPowerSaving(false) is a silent no-op. This clears both flags and restores the frequency
  // unconditionally.
  void ensureFullSpeedForRadio();

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
  // active, USB is connected (light sleep kills the CDC link), the board has a PWM
  // frontlight (light sleep stops LEDC and the light would flicker), or a button
  // change is mid-debounce. The caller must fall back to delay() in that case. Call from
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
