#include "OrientationTracker.h"

namespace {
// CrossPointOrientation values (HalTiltSensor.h), kept apart so this file builds on the host.
constexpr uint8_t PORTRAIT = 0;
constexpr uint8_t LANDSCAPE_CW = 1;
constexpr uint8_t INVERTED = 2;
constexpr uint8_t LANDSCAPE_CCW = 3;

// Squares in int64: a sample is a few thousand mg, but nothing here assumes the sensor's range.
int64_t absMg(const int32_t v) { return v < 0 ? -static_cast<int64_t>(v) : v; }
}  // namespace

uint8_t OrientationTracker::classify(const int32_t axMg, const int32_t ayMg) {
  const int64_t x = absMg(axMg);
  const int64_t y = absMg(ayMg);
  if (x * x + y * y < static_cast<int64_t>(kMinTiltMg) * kMinTiltMg) return kNone;
  if (x * kLeadDen >= kLeadNum * y) return axMg < 0 ? PORTRAIT : INVERTED;
  if (y * kLeadDen >= kLeadNum * x) return ayMg > 0 ? LANDSCAPE_CW : LANDSCAPE_CCW;
  return kNone;
}

bool OrientationTracker::update(const int32_t axMg, const int32_t ayMg, const uint32_t nowMs) {
  const uint8_t now = classify(axMg, ayMg);
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
