#include "OrientationTracker.h"

namespace {
// CrossPointOrientation values (HalTiltSensor.h), kept apart so this file builds on the host.
constexpr uint8_t PORTRAIT = 0;
constexpr uint8_t LANDSCAPE_CW = 1;
constexpr uint8_t INVERTED = 2;
constexpr uint8_t LANDSCAPE_CCW = 3;

float absf(const float v) { return v < 0 ? -v : v; }
}  // namespace

uint8_t OrientationTracker::classify(const float ax, const float ay) {
  if (ax * ax + ay * ay < kMinTiltG * kMinTiltG) return kNone;
  const float x = absf(ax);
  const float y = absf(ay);
  if (x >= kLeadRatio * y) return ax < 0 ? PORTRAIT : INVERTED;
  if (y >= kLeadRatio * x) return ay > 0 ? LANDSCAPE_CW : LANDSCAPE_CCW;
  return kNone;
}

bool OrientationTracker::update(const float ax, const float ay, const uint32_t nowMs) {
  const uint8_t now = classify(ax, ay);
  if (now != candidate_) {
    candidate_ = now;
    candidateSinceMs_ = nowMs;
    return false;
  }
  if (now == kNone || now == settled_ || nowMs - candidateSinceMs_ < kDwellMs) return false;
  settled_ = now;
  return true;
}

void OrientationTracker::reset() {
  candidate_ = kNone;
  candidateSinceMs_ = 0;
  settled_ = kNone;
}
