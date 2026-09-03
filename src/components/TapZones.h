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

}  // namespace TapZones
