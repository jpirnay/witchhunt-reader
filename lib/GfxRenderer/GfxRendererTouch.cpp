// GfxRenderer's touch mapping, apart from the rest of the renderer: the SDK reports a tap as normalized float
// coordinates, so this is where float enters and leaves it. GfxRenderer.cpp itself is float-free
// (scripts/check_no_float.py).
#include <TouchTransform.h>

#include "GfxRenderer.h"

// Inverse of rotateCoordinates: panel-native normalized touch -> logical px.
// Ported from upstream/develop so the layers above stay diff-comparable. Clamp
// first, then rotate, so an out-of-range report from the controller can never
// produce an off-screen logical point.
//
// One deliberate difference from upstream: our `orientation` member is a
// std::atomic<int>, not a plain enum, so this reads it once through
// getOrientation() instead of switching on the member directly. Reading it once
// also keeps the transform self-consistent if the reader rotates mid-call.
// The transform itself lives in TouchTransform.h so it can be host-tested
// without Arduino; keep the two orientation orders in lockstep.
static_assert(static_cast<int>(GfxRenderer::Portrait) == touchtransform::Portrait &&
                  static_cast<int>(GfxRenderer::LandscapeClockwise) == touchtransform::LandscapeClockwise &&
                  static_cast<int>(GfxRenderer::PortraitInverted) == touchtransform::PortraitInverted &&
                  static_cast<int>(GfxRenderer::LandscapeCounterClockwise) == touchtransform::LandscapeCounterClockwise,
              "touchtransform::Orientation must mirror GfxRenderer::Orientation");

void GfxRenderer::tapToLogical(const float nx, const float ny, int& outX, int& outY) const {
  // The held frame, read once: touch is read off the render task, and a vertical page holds the turned frame
  // as the live one for its whole draw.
  tapToLogical(getHeldOrientation(), nx, ny, outX, outY);
}

void GfxRenderer::tapToLogical(const Orientation orientation, const float nx, const float ny, int& outX,
                               int& outY) const {
  touchtransform::tapToLogical(static_cast<int>(orientation), panelWidth, panelHeight, nx, ny, outX, outY);
}

void GfxRenderer::logicalToTap(const int x, const int y, float& outNx, float& outNy) const {
  touchtransform::logicalToTap(static_cast<int>(getHeldOrientation()), panelWidth, panelHeight, x, y, outNx, outNy);
}
