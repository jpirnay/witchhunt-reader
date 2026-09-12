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
// This mirrors HalSpiBus's recursive, fail-closed mutex. There is no
// lock-ordering relationship with HalSpiBus — no path holds one and takes the
// other.
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

  // Use `external` as THE bus mutex instead of the one begin() created.
  //
  // For a board whose support layer also drives peripherals on these pins from
  // a task of its own. The T5S3 is one: its PCA9535 and TPS65185 helpers take a
  // mutex private to the board layer, and LovyanGFX's panel task runs them on
  // every refresh to raise the EPD rails. Two mutexes over one Wire serialise
  // nothing, and the consequence there is not a garbled register read -- a
  // failed power-up makes the panel task clock a whole frame out with the
  // high-voltage rails down, so the frame lands weakly or not at all.
  //
  // Adopting the board's handle rather than handing it ours keeps the direction
  // of the dependency right: the SDK board layer cannot see this header, and the
  // board-specific choice stays in the wiring layer (main.cpp) next to the
  // BoardT5S3::begin() call that already lives there.
  //
  // Call before anything can take a Lock -- i.e. before the input sampler and
  // the display come up. Idempotent; a null handle is ignored.
  static void adoptMutex(SemaphoreHandle_t external);

 private:
  HalI2cBus();

  SemaphoreHandle_t mutex = nullptr;
  // False once adoptMutex() has taken someone else's handle, so we never delete
  // a mutex the board layer is still using.
  bool ownsMutex = true;

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
  // No cross-task I2C to serialize, so there is no mutex to replace. Declared
  // anyway so the wiring layer needs no #if of its own.
  static void adoptMutex(SemaphoreHandle_t) {}
#endif

 public:
  // Bring the shared I2C bus up, once, from the active board profile.
  //
  // This layer owns bus INITIALISATION as well as serialization, because those
  // are the same question: who owns the bus. Previously HalPowerManager called
  // Wire.begin() as a side effect of setting up the fuel gauge, which is the
  // wrong layer to decide it -- the gauge is one of several peripherals on a
  // shared bus, not its owner.
  //
  // The subtle part is that we are NOT always the one who should start it. When
  // the board has a touch controller on these pins, the SDK's InputManager has
  // already called Wire.begin() for it during inputMgr.begin(). Starting it a
  // second time hands the ESP-IDF i2c_master driver a port it already owns: the
  // call fails and every later transaction returns ESP_ERR_INVALID_STATE. Both
  // touch boards hit this -- X4 Pro shares SDA39/SCL38 between GT911, gauge and
  // RTC; T5S3 shares SDA39/SCL40 between GT911 and its gauge.
  //
  // Must be called AFTER gpio.begin() (which runs inputMgr.begin()), so the
  // touch driver has had its chance first. Idempotent.
  static void ensureBusStarted();
};
