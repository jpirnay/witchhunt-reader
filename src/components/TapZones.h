#pragma once

#include <cstdint>

// Which of five zones a tap on the reading surface landed in.
//
// Pure geometry with no dependencies, so the arithmetic is testable on the host
// — the same split as SliderGeometry.h and ListTouchBand.h. What a zone MEANS
// is TouchGestures.h's business.
//
// The bands reproduce the reader's long-standing zones exactly, which is why the
// horizontal split is tested first: the outer thirds have been the page-turn
// zones over their whole height (ReaderUtils::detectTouchPageTurn), and the
// reader menu has been the centre third of the centre column
// (ReaderUtils::isTouchMenuTap). Top and Bottom are the two remaining cells of
// the centre column, which nothing has ever used. So every tap that meant
// something before still means it, and the two new zones are carved out of empty
// space rather than out of a page turn.
//
//     +--------+--------+--------+
//     |        |  Top   |        |
//     |  Left  +--------+ Right  |
//     |        | Centre |        |
//     |        +--------+        |
//     |        | Bottom |        |
//     +--------+--------+--------+
namespace TapZones {

enum class Zone : uint8_t { Left, Right, Centre, Top, Bottom };

// Coordinates are LIVE-orientation logical pixels — the frame the reader is
// looking at — so the zones rotate with the page, as a reader expects.
inline Zone zoneFor(const int x, const int y, const int width, const int height) {
  const int columnWidth = width / 3;
  if (x < columnWidth) return Zone::Left;
  if (x >= width - columnWidth) return Zone::Right;
  const int rowHeight = height / 3;
  if (y < rowHeight) return Zone::Top;
  if (y >= height - rowHeight) return Zone::Bottom;
  return Zone::Centre;
}

// The four corners, for LONG presses only.
//
// Kept separate from Zone rather than added to it, and that separation is the whole
// safety argument: a corner sits INSIDE Left or Right, so folding it into zoneFor()
// would carve two squares out of the page-turn thirds -- behaviour that is already
// device-validated. Only the long-press path asks for a corner; a tap never does, so
// every tap still means exactly what it meant before these existed.
// (test/tap_zones asserts that pixel by pixel.)
//
//     +---+----------------+---+
//     | TL|                |TR |
//     +---+                +---+
//     |      zoneFor() unchanged |
//     +---+                +---+
//     | BL|                |BR |
//     +---+----------------+---+
enum class Corner : uint8_t { None, TopLeft, TopRight, BottomLeft, BottomRight };

// Square, and sized off the SHORTER screen edge -- a deliberate divergence from
// KOReader, whose corner zones are w/8 x h/8 independently. On the T5S3's 540x960
// frame that formula gives a 67x120 rectangle, which reads as a stripe down the side
// rather than as a corner; taking the shorter edge keeps the target square and the
// same physical size whichever way the device is held.
//
// One eighth matches KOReader's DTAP_ZONE_*_CORNER, which is the smallest target that
// is still comfortably hittable without aiming.
inline constexpr int kCornerDivisor = 8;

// Coordinates are LIVE-orientation logical pixels, as for zoneFor().
inline Corner cornerFor(const int x, const int y, const int width, const int height) {
  const int shorterEdge = width < height ? width : height;
  const int side = shorterEdge / kCornerDivisor;
  // A nonsense or tiny frame has no corners, and a long press there falls through to
  // the five zones -- the same "a dull answer rather than an undefined one" contract
  // zoneFor() keeps for the same reason.
  if (side <= 0) return Corner::None;
  const bool left = x < side;
  const bool right = x >= width - side;
  const bool top = y < side;
  const bool bottom = y >= height - side;
  if (top && left) return Corner::TopLeft;
  if (top && right) return Corner::TopRight;
  if (bottom && left) return Corner::BottomLeft;
  if (bottom && right) return Corner::BottomRight;
  return Corner::None;
}

// Which half of a BOUNDED band a tap fell in: the page-turn split for the paged text views that
// are not the reading surface -- a book's description, a dictionary entry.
//
// Bounded, and that is the whole point. The zones above run the full height of the panel, which
// is right for the reader (it draws no chrome) and wrong for these, which draw a button-hint
// strip along the bottom: full-height zones would claim the taps meant for it and Back would
// stop working. Restricting the split to the rectangle the text occupies leaves every pixel
// outside it to whoever else wants it.
enum class Half : uint8_t { None, Previous, Next };

inline Half halfOfBand(const int px, const int py, const int x, const int y, const int width, const int height) {
  if (width <= 0 || height <= 0) return Half::None;
  if (px < x || px >= x + width) return Half::None;
  if (py < y || py >= y + height) return Half::None;
  return px < x + width / 2 ? Half::Previous : Half::Next;
}

}  // namespace TapZones
