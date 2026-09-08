#pragma once

// A uniform grid of keys: equal cells, equal gaps, laid out from a top-left origin.
//
// The on-screen keyboard is two of these -- the character rows and the five-key bottom row --
// and it was the last screen in the firmware with a selection and no way to touch it. Every
// password, OPDS URL and sync login was typed by walking a cursor across forty-odd keys.
//
// The arithmetic is the inverse of what the draw already does, so it lives beside the draw's
// inputs rather than inside the activity: the same reason CoverGridLayout::hitTest() sits next
// to compute(). A grid whose hit test is derived independently of its layout is a grid that
// drifts, which is what happened to the home carousel's covers.
namespace KeyboardGrid {

struct Rows {
  int originX = 0;
  int originY = 0;
  int keyWidth = 0;
  int keyHeight = 0;
  // Gap between neighbouring keys, horizontally and vertically. Zero on some themes, so the
  // hit test must not assume a gutter it can reject taps into.
  int spacing = 0;
  int cols = 0;
  int rows = 0;
};

// The key under a point. Returns false for a miss -- outside the block, or in a gutter between
// keys. A gutter tap is a miss on purpose: the keys are small and adjacent, so charging a
// near-miss to whichever neighbour is closer would type the wrong letter rather than nothing.
inline bool hitTest(const Rows& g, const int px, const int py, int& row, int& col) {
  if (g.cols <= 0 || g.rows <= 0 || g.keyWidth <= 0 || g.keyHeight <= 0) return false;

  const int stepX = g.keyWidth + g.spacing;
  const int stepY = g.keyHeight + g.spacing;

  const int dx = px - g.originX;
  const int dy = py - g.originY;
  if (dx < 0 || dy < 0) return false;

  const int c = dx / stepX;
  const int r = dy / stepY;
  if (c >= g.cols || r >= g.rows) return false;

  // Inside the cell rather than in the gap after it.
  if (dx - c * stepX >= g.keyWidth) return false;
  if (dy - r * stepY >= g.keyHeight) return false;

  row = r;
  col = c;
  return true;
}

}  // namespace KeyboardGrid
