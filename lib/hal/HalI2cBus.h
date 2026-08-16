#pragma once

#include <BoardConfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Serializes access to the I2C bus shared by the touch controller, the RTC and
// the fuel gauge.
//
// Why this is needed (docs/touch-input-migration-2026-08-14.md §5, "P1"):
//
// On the X4 Pro all three peripherals sit on ONE bus — GT911 touch at 0x5D,
// BM8563 RTC at 0x51 and the CW2017 gauge at 0x63, all on bus 0 (SDA39/SCL38).
// They are driven from DIFFERENT tasks:
//
//   btnsample task   HalGPIO::sampleOnce() -> inputMgr.update() -> serviceTouch()
//   loop task        HalClock RTC reads/writes, HalPowerManager gauge polls
//
// InputManager::update() is the only public entry point and always services
// touch, so touch I2C necessarily runs wherever update() runs — there is no way
// to move it onto the loop task without an SDK change. Concurrent transactions
// from two tasks on one bus corrupt each other, so the bus needs a lock.
//
// Upstream has no equivalent: they poll input synchronously from the loop task,
// so they never have two tasks on the bus. Their multi-bus support
// (BoardProfile::batteryGauge.i2cBus, moving the gauge to Wire1 on Sticky) does
// not help here, because the X4 Pro wires all three devices to the same pins.
//
// This mirrors HalSpiBus exactly, including the recursive mutex and the
// timeout-and-proceed policy: a wedged bus should show up as a diagnosable log
// line, not a frozen device. There is no lock-ordering relationship with
// HalSpiBus — no path holds one and takes the other.
//
// On a NON-touch board the whole thing compiles away to nothing: the sampler
// reads the ADC only, and the RTC and gauge are both driven from the loop task,
// so there is no cross-task I2C to serialize. Making that a no-op Lock rather
// than an #if at every call site keeps the callers identical on both targets
// (the same approach phase 1 took for the touch passthrough) and keeps the C3
// byte-identical.
class HalI2cBus {
 public:
#if FREEINK_CAP_TOUCH
  class Lock {
   public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    bool acquired = false;
  };

  static HalI2cBus& getInstance();

  // Create the mutex up front. Call once from setup() before the input sampler
  // starts, so mutex allocation never lands in the first touch poll.
  static void begin();

 private:
  HalI2cBus();

  SemaphoreHandle_t mutex = nullptr;

  friend class Lock;
#else
  // No touch controller: nothing shares the bus across tasks.
  class Lock {
   public:
    Lock() = default;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
  };

  static void begin() {}
#endif
};
