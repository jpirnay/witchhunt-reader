#pragma once

// Pure geometry for mapping touch coordinates, kept free of Arduino/ESP-IDF so
// it can be host-tested. GfxRenderer::tapToLogical is a thin wrapper over
// tapToLogical() below; the math lives here only so the four-orientation
// behaviour is testable without hardware (see test/touch_transform).
//
// Coordinate contract, as the SDK defines it: InputManager reports taps as
// normalized 0..1 floats in the PANEL's native frame, already corrected for
// swapXY/flipX/flipY by the board profile. This maps that into the LOGICAL
// frame the renderer is currently drawing in.

namespace touchtransform {

// Mirrors GfxRenderer::Orientation's declared order. Kept as plain ints so this
// header needs no GfxRenderer.h (and therefore no Arduino). GfxRenderer.cpp
// static_asserts that the two orders still agree, so drift is a build error
// rather than a silently rotated touch panel.
enum Orientation : int {
  Portrait = 0,
  LandscapeClockwise = 1,
  PortraitInverted = 2,
  LandscapeCounterClockwise = 3,
};

// Panel-native normalized touch -> logical screen pixels. The inverse of
// GfxRenderer's rotateCoordinates().
//
// Clamping happens on the PHYSICAL point, before rotation: a controller that
// reports slightly outside 0..1 (or exactly 1.0, which would land one pixel
// past the edge) is pulled onto the panel first, so every rotation branch is
// guaranteed an in-range input and the logical result is always on screen.
inline void tapToLogical(const int orientation, const int panelWidth, const int panelHeight, const float nx,
                         const float ny, int& outX, int& outY) {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;

  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

// Band hit-test shared by MappedInputManager's rowTouch() and colTouch().
//
// Both ask the same question on perpendicular axes: "which equal-sized band
// along one axis does this point fall in, given it is within bounds on the
// other?" Rows pass along=y/across=x, columns pass along=x/across=y. Factoring
// it here removes the duplicated arithmetic and, more usefully, makes the
// geometry testable without HalGPIO or a renderer.
//
//   along       coordinate on the banded axis (y for rows, x for columns)
//   across      coordinate on the perpendicular axis
//   start       where band 0 begins on the banded axis (top / left)
//   step        band pitch, including any gap between bands
//   count       number of bands
//   acrossStart/acrossEnd  half-open bounds on the perpendicular axis
//   extent      hit depth within each step; 0 = the whole step (no gap band).
//               Non-zero excludes the gap between rows, so a tap that lands
//               between two rows selects neither rather than the one above it.
//
// Returns false and leaves `index` untouched when the point misses.
inline bool bandHit(const int along, const int across, const int start, const int step, const int count,
                    const int acrossStart, const int acrossEnd, const int extent, int& index) {
  if (step <= 0 || count <= 0) return false;
  if (across < acrossStart || across >= acrossEnd) return false;
  if (along < start) return false;
  const int band = (along - start) / step;
  if (band >= count) return false;
  if (extent > 0 && (along - start) % step >= extent) return false;
  index = band;
  return true;
}

}  // namespace touchtransform
