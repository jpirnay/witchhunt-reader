#pragma once

#include <Arduino.h>
#include <Imu.h>

#include "OrientationTracker.h"
#include "TiltRate.h"

namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;

// Tilt page turns from the board's IMU, read through the SDK's freeink::Imu. The SDK takes the
// address, the chip and the mount correction from the board profile (BoardConfig sensors), so the
// gyro reports one frame on every board: X = the portrait left/right tilt axis, Y = the landscape
// one. It replaced a QMI8658 driver of our own that assumed the X3: its address first (0x6B, so the
// Picco's IMU at 0x6A answered only after a NACK that logged an I2C error at every boot) and its
// mounting.
//
// SDK backend ported from crosspoint-reader (Justin Mitchell, @itsthisjustin): #2481 and 8fc2b6618.
class HalTiltSensor {
  bool _present = false;    // the IMU answered begin()
  bool _available = false;  // ... and has a gyro, which tilt page turns need
  Imu _imu;

  bool _tiltForwardEvent = false;
  bool _tiltBackEvent = false;
  bool _hadActivity = false;
  bool _inTilt = false;
  bool _isAwake = false;
  bool _rotating = false;  // auto-rotate was on at the last update()
  OrientationTracker _tracker;
  uint8_t _orientationEvent = OrientationTracker::kNone;
  unsigned long _lastTiltMs = 0;
  unsigned long _wakeMs = 0;
  unsigned long _lastPollMs = 0;

  static constexpr float RATE_THRESHOLD_DPS = 270.0f;
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;
  static constexpr unsigned long COOLDOWN_MS = 600;
  static constexpr unsigned long POLL_INTERVAL_MS = 50;
  // Auto-rotate alone: a pose takes OrientationTracker::kDwellMs to settle, so 5 Hz is enough.
  static constexpr unsigned long ROTATE_POLL_INTERVAL_MS = 200;
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;

  // Every IMU transaction holds HalI2cBus::Lock: on a touch board the bus is shared with the touch
  // controller, which the input sampler polls from another task (the Picco's TMA525C, light and
  // charger all sit on SDA7/SCL8). The SDK's own freeink::I2cBusLock is inert on our boards.
  bool imuRead(Imu::Sample& out);
  bool imuSleep();
  bool imuWake();

 public:
  void begin();
  bool wake();
  bool deepSleep();

  bool isAvailable() const { return _available; }

  // One sample, mount-corrected (serial test API, CMD:IMU). A sleeping IMU is woken for it, given
  // WAKE_STABILIZE_MS to settle and put back to sleep. Loop task only, like update(). False when no
  // IMU answered at boot or the read failed.
  bool readSample(Imu::Sample& out);

  // Once per loop pass. The IMU is awake only in a reader, and only while tilt page turns (`mode`, along `axis`) or
  // auto-rotate want it; otherwise it is in the power-down mode Imu::sleep() sets.
  void update(CrossPointTiltPageTurn::Value mode, TiltRate::Axis axis, CrossPointOrientation::Value orientation,
              bool autoRotate, bool inReader);

  // Auto-rotate: true once for each orientation the device has settled in, which is `orientation`. Consumed on read.
  bool takeOrientation(uint8_t& orientation);

  bool wasTiltedForward();
  bool wasTiltedBack();
  bool hadActivity();
  void clearPendingEvents();
};
