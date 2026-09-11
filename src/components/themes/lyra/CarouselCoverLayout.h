#pragma once

// Where the carousel's three cover tiles sit, and which book each one shows.
//
// This exists because the draw and the tap targets each derived it separately and disagreed.
// The draw WRAPS -- the slot left of the first book shows the last one, the way the Left/Right
// buttons wrap -- and gates the side tiles on the book count (a two-book carousel has a right
// neighbour but no left one, or the same cover would appear twice). The recorder did neither,
// so at centre index 0, which is where the home screen opens, the left cover was painted and
// tapping it did nothing; with exactly two books a target was recorded for a tile that is not
// drawn at all.
//
// Pure arithmetic over its inputs, with no theme or renderer dependency, so the round trip
// (compute a slot, hit-test the pixel it was drawn at) is exercised on the host -- the gate that
// caught two bugs in SliderGeometry and would have caught this one.
namespace CarouselCoverLayout {

// The tile sizes the theme paints with. Passed in rather than baked in so this header stays free
// of ThemeMetrics; LyraCarouselTheme states them once.
struct Dimensions {
  int centreW;
  int centreH;
  int sideW;
  int sideH;
  // How far each side tile slides BEHIND the centre one. The centre is drawn last and so covers
  // this much of each neighbour.
  int overlap;
  // Gap between the top of the cover rect and the top of the centre tile.
  int topPad;
};

struct Slot {
  // The book this slot shows, or -1 when the slot is not drawn. Callers must check: an absent
  // slot still carries its geometry, and acting on it would be the phantom-target bug again.
  int bookIndex = -1;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct Slots {
  Slot centre;
  Slot left;
  Slot right;
};

inline Slots compute(const Dimensions& d, const int screenW, const int rectY, const int centerIdx,
                     const int bookCount) {
  Slots slots;
  if (bookCount <= 0) return slots;

  const int centreTileY = rectY + d.topPad;
  const int sideTileY = centreTileY + (d.centreH - d.sideH) / 2;
  const int centreX = (screenW - d.centreW) / 2;

  slots.centre = {centerIdx, centreX, centreTileY, d.centreW, d.centreH};

  // Side tiles wrap, so their geometry is always valid; only bookIndex says whether they exist.
  slots.left = {-1, centreX - d.sideW + d.overlap, sideTileY, d.sideW, d.sideH};
  slots.right = {-1, centreX + d.centreW - d.overlap, sideTileY, d.sideW, d.sideH};

  // A left neighbour needs three books: with two, the book on the left and the book on the right
  // would be the same one. A right neighbour needs only two.
  if (bookCount >= 3) slots.left.bookIndex = (centerIdx + bookCount - 1) % bookCount;
  if (bookCount >= 2) slots.right.bookIndex = (centerIdx + 1) % bookCount;

  return slots;
}

// Clip a slot to the screen, so the part of a side tile that hangs off the edge stops being
// tappable.
//
// The side tiles are positioned to slide behind the centre one, which puts the left tile's x at
// `centreX - sideW + overlap`. On a narrow panel that is NEGATIVE -- 540 px wide gives -40 --
// so the tile is drawn clipped, and the recorded rect used to keep claiming the empty margin
// beside it. A press there opened a book that was not visible under the finger.
//
// Clipping on the RECORDING side rather than in the hit test, so the geometry a caller reads is
// the geometry a reader can see; a slot clipped away entirely reports w/h 0 and hitTest misses
// it like any other empty rect.
inline Slot clampToScreen(const Slot& slot, const int screenW, const int screenH) {
  Slot out = slot;
  const int right = slot.x + slot.w;
  const int bottom = slot.y + slot.h;
  out.x = slot.x < 0 ? 0 : slot.x;
  out.y = slot.y < 0 ? 0 : slot.y;
  const int clippedRight = right > screenW ? screenW : right;
  const int clippedBottom = bottom > screenH ? screenH : bottom;
  out.w = clippedRight - out.x;
  out.h = clippedBottom - out.y;
  if (out.w < 0) out.w = 0;
  if (out.h < 0) out.h = 0;
  return out;
}

// The book under a point, or -1 for a miss. Centre first: the side tiles slide behind it and the
// draw puts the centre on top, so the overlapping strip belongs to whatever the reader can see.
inline int hitTest(const Slots& slots, const int px, const int py) {
  for (const Slot* slot : {&slots.centre, &slots.left, &slots.right}) {
    if (slot->bookIndex < 0) continue;
    if (slot->w <= 0 || slot->h <= 0) continue;
    if (px >= slot->x && px < slot->x + slot->w && py >= slot->y && py < slot->y + slot->h) return slot->bookIndex;
  }
  return -1;
}

}  // namespace CarouselCoverLayout
