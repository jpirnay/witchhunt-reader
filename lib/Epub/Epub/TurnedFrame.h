#pragma once

#include <cstdint>

// A vertical-rl page is laid out as a horizontal page in a frame one 90-degree step from the reader's (spec 5.2):
// inline runs down, block runs leftward, so a content point (x, y) is the reader point (readerWidth - 1 - y, x).
// Orientations in GfxRenderer::Orientation's order; PageFrameScope.h static_asserts that they agree.
namespace turnedframe {

enum Orientation : int { Portrait = 0, LandscapeClockwise = 1, PortraitInverted = 2, LandscapeCounterClockwise = 3 };

// The turned frame for the reader's.
constexpr int contentOf(const int reader) { return (reader + 3) % 4; }
// Its inverse.
constexpr int readerOf(const int content) { return (content + 1) % 4; }

struct Rect {
  int x;
  int y;
  int width;
  int height;
};

// A rectangle of the turned frame in the reader's.
constexpr Rect toReader(const int readerWidth, const Rect content) {
  return {readerWidth - content.y - content.height, content.x, content.height, content.width};
}

struct Point {
  int x;
  int y;
};

// The inverse of toReader for a point: the content point under the reader's (readerX, readerY).
constexpr Point toContentPoint(const int readerWidth, const int readerX, const int readerY) {
  return {readerY, readerWidth - 1 - readerX};
}

struct Offsets {
  int x;
  int y;
};

// Where a page drawn at reader offsets (readerX, readerY) puts its origin in the turned frame: block 0 is the right
// edge of a viewport blockExtent wide.
constexpr Offsets contentOffsets(const int readerWidth, const int readerX, const int readerY, const int blockExtent) {
  return {readerY, readerWidth - readerX - blockExtent};
}

}  // namespace turnedframe
