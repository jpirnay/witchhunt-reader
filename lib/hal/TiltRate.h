#pragma once

#include <cstdint>

// The rate of the chosen tilt gesture, from the IMU's mount-corrected gyro (the panel's native frame, X along the long
// side, Y along the short side, Z into the screen: OrientationTracker.h) and the reader's orientation, in thousandths
// of a degree per second. Negative while the right edge of the text as read (Sideways) or its top edge (ForwardBack)
// moves away from the reader; HalTiltSensor reports a negative crossing as tilted back (the "Right / forward" action),
// a positive one as tilted forward ("Left / back").
//
// Each case is the right-hand rule on the edge GfxRenderer puts there. Sideways in Portrait: the text's right edge is
// native -y, and -y turning into +z is a negative turn about x. ForwardBack in Portrait: the top edge is native -x, and
// -x turning into +z is a positive turn about y, so the rate is -gy.
namespace TiltRate {

enum class Axis : uint8_t { Sideways = 0, ForwardBack = 1 };

// `orientation`: a CrossPointOrientation value (0 Portrait, 1 LandscapeCw, 2 Inverted, 3 LandscapeCcw).
inline int32_t rate(const int32_t gx, const int32_t gy, const uint8_t orientation, const Axis axis) {
  const bool forwardBack = axis == Axis::ForwardBack;
  switch (orientation) {
    case 0:  // Portrait
      return forwardBack ? -gy : gx;
    case 1:  // LandscapeCw
      return forwardBack ? -gx : -gy;
    case 2:  // Inverted
      return forwardBack ? gy : -gx;
    case 3:  // LandscapeCcw
      return forwardBack ? gx : gy;
    default:
      return forwardBack ? -gy : gx;
  }
}

}  // namespace TiltRate
