#pragma once

#include <cstdint>

// Which of the reader's four orientations the device is held in, from the accelerometer.
//
// Input is the IMU's mount-corrected sample (freeink::Imu, BoardConfig SensorsConfig) in the panel's native frame: X
// along the long side toward native +x, Y along the short side toward native +y, Z into the screen. A board whose IMU
// sits otherwise sets imuSwapXY / imuFlipX / imuFlipY in its profile. Held still, an accelerometer reads 1 g pointing
// UP, so the reading points at the top edge of the text that reads upright in that pose, and GfxRenderer puts that
// edge at native -x for Portrait, +y for LandscapeClockwise, +x for PortraitInverted, -y for
// LandscapeCounterClockwise. Measured on the Picco (CMD:IMU, 2026-11-16): held normally -x, turned a quarter clockwise
// +y, upside down +x, a quarter counter-clockwise -y, flat face up -z.
//
// update() reports a change once the device has been held in a new orientation for kDwellMs: a flick that turns a page
// comes back before it settles, and a device lying flat or held near a diagonal changes nothing. Pure, so the host
// tests drive it with samples and timestamps.
class OrientationTracker {
 public:
  static constexpr uint8_t kNone = 0xFF;

  // The orientation a sample points at, a CrossPointOrientation value, or kNone: near flat (less than kMinTiltG of
  // gravity in the panel's plane, about 30 degrees from flat) or near a diagonal (the leading axis under
  // kLeadRatio times the other, about 34 degrees from the axis).
  static uint8_t classify(float ax, float ay);

  // One sample at `nowMs`. True when a new orientation has settled: read it from settled(). The first one to settle
  // after reset() counts as new, so a reader opened in landscape turns at once.
  bool update(float ax, float ay, uint32_t nowMs);
  uint8_t settled() const { return settled_; }
  // Forget the history: the IMU was asleep, or auto-rotate was off.
  void reset();

  static constexpr float kMinTiltG = 0.5f;
  static constexpr float kLeadRatio = 1.5f;
  static constexpr uint32_t kDwellMs = 600;

 private:
  uint8_t candidate_ = kNone;
  uint32_t candidateSinceMs_ = 0;
  uint8_t settled_ = kNone;
};
