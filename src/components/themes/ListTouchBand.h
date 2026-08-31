#pragma once

#include <atomic>
#include <cstdint>

// Where the rows of the list on screen actually landed, so a tap can be matched against the
// list that is really painted.
//
// Same shape and the same reasoning as ButtonHintStrip next door: the draw records what it
// painted, and the input side matches against that instead of re-deriving the geometry. The
// alternative -- every screen computing its own row band for a hit test -- is a second copy
// of the layout rule that drifts from the first the moment a theme changes a row height, and
// it cannot express a wrapped list at all, where rows are not a uniform step.
//
// Recorded by the `drawList` implementations (BaseTheme, LyraTheme) and by the shared
// `drawWrappedList`, so every caller of GUI.drawList gets a tappable list without being
// touched. That is the whole point: there are 23 direct callers plus MenuListActivity's 8
// subclasses, and none of them should have to know the arithmetic.
//
// COORDINATES ARE LIVE-ORIENTATION LOGICAL PIXELS, and this differs from ButtonHintStrip on
// purpose. drawButtonHints() forces Portrait for the duration of its draw, so the strip is
// recorded in the Portrait frame and hit-tested there. drawList() does no such thing -- it
// paints in whatever orientation the renderer is currently in -- so the rows are recorded in
// that frame and must be hit-tested with the plain live-orientation tap. Rotating the screen
// forces a repaint, which re-records; the window between the two carries no strip and no
// band, which reads as "no list here yet" rather than as a wrong hit.
//
// Rows are contiguous in item order: row r is item `firstIndex + r`. Both the paged and the
// scrolled path draw a contiguous run, so nothing needs a per-row index.
namespace ListTouchBand {

// Deliberately equal to ListLayout::kMaxRows, which owns the argument for the number: the
// shortest theme row is 30 px and the tallest content area is well under 600 px, so a
// screenful cannot exceed this. Stated here rather than included so this header stays free of
// everything (it is hit-tested on the host); the static_assert in ListTouchBand.cpp's test
// keeps the two honest.
inline constexpr int kMaxRows = 24;

struct Band {
  int16_t x = 0;      // left edge of the tappable band
  int16_t width = 0;  // 0 = nothing recorded
  int16_t firstIndex = 0;
  int16_t count = 0;
  int16_t top[kMaxRows]{};     // absolute y of each row
  int16_t height[kMaxRows]{};  // each row's height in px
  // Bit r set = row r is a real, activatable item. Separator rows are drawn but are not
  // selectable -- the draw loop skips them and the navigation skips over them -- so a finger
  // landing on one must do nothing rather than select the heading.
  uint32_t selectable = 0;
};

static_assert(kMaxRows <= 32, "selectable is a uint32_t bitmask");

// Published across tasks: drawList() runs on the render task, the hit test on the loop task.
// A seqlock rather than a bare valid flag, because a half-written Band would be hit-tested
// against a mix of two screens' geometry. Writes are frequent and reads rare, so losing a read
// to a concurrent write costs nothing -- a failed snapshot reads as "no list this tick" and
// the tap falls through to whoever else wants it, which is the safe way to be wrong.
namespace detail {
inline Band& storage() {
  static Band b;
  return b;
}
inline std::atomic<uint32_t>& seq() {
  static std::atomic<uint32_t> s{0};
  return s;
}
}  // namespace detail

// Called by the list draw with the geometry it just painted.
inline void record(const Band& in) {
  auto& seq = detail::seq();
  seq.fetch_add(1, std::memory_order_acq_rel);  // odd: write in progress
  detail::storage() = in;
  seq.fetch_add(1, std::memory_order_release);  // even: stable
}

// Forget the list. Must be called on every activity transition, for the reason ButtonHintStrip
// documents: a screen that draws no list would otherwise inherit the previous screen's rows and
// turn a tap anywhere in the content area into a phantom selection.
inline void invalidate() { record(Band{}); }

// Reads the band if one is stable. False on a torn read or when none was ever recorded.
inline bool snapshot(Band& out) {
  auto& seq = detail::seq();
  const uint32_t before = seq.load(std::memory_order_acquire);
  if (before == 0 || (before & 1u) != 0) return false;
  out = detail::storage();
  return seq.load(std::memory_order_acquire) == before;
}

// True when the current screen painted at least one selectable row. Lets a caller skip the tap
// queue entirely on a screen with no list, leaving the tap for whatever else wants it.
inline bool hasBand() {
  Band b;
  if (!snapshot(b)) return false;
  return b.width > 0 && b.count > 0 && b.selectable != 0;
}

// Pure geometry, split out so the arithmetic can be tested without the shared state.
// Returns the ITEM INDEX under the point, or -1 for a miss.
//
// Rows are tested individually rather than by dividing by a step: a wrapped list's rows differ
// in height, and even a uniform list can leave a remainder strip at the bottom of the rect that
// belongs to no row. A tap there must miss, not round into the last row.
inline int hitTestIn(const Band& b, const int px, const int py) {
  if (b.width <= 0 || b.count <= 0) return -1;
  if (px < b.x || px >= b.x + b.width) return -1;
  for (int r = 0; r < b.count && r < kMaxRows; ++r) {
    if ((b.selectable & (1u << r)) == 0) continue;
    if (py >= b.top[r] && py < b.top[r] + b.height[r]) return b.firstIndex + r;
  }
  return -1;
}

// Returns the item index under the point, or -1 for a miss, a torn read, or a list that was
// never recorded.
inline int hitTest(const int px, const int py) {
  Band b;
  if (!snapshot(b)) return -1;
  return hitTestIn(b, px, py);
}

// --- Recording helpers -------------------------------------------------------------------
//
// Both live here rather than in the themes so the two fixed-height `drawList` implementations
// cannot drift apart, and so the rule "row r is item firstIndex + r" is stated once.

// Builder for a draw loop: rows are appended in the order they are painted, and each says
// whether it is a real item. Kept as a plain struct the caller stack-allocates and hands to
// record() once at the end -- so `rowTitle(i)` is never called a second time just to decide
// selectability, which matters because an SD-backed list reads that from the card.
struct Builder {
  Band band{};

  void begin(const int rectX, const int rectWidth, const int firstIndex) {
    band = Band{};
    band.x = static_cast<int16_t>(rectX);
    band.width = static_cast<int16_t>(rectWidth);
    band.firstIndex = static_cast<int16_t>(firstIndex);
  }

  void addRow(const int top, const int height, const bool selectable) {
    if (band.count >= kMaxRows) return;  // see kMaxRows: unreachable in practice
    const auto r = static_cast<int>(band.count);
    band.top[r] = static_cast<int16_t>(top);
    band.height[r] = static_cast<int16_t>(height);
    if (selectable) band.selectable |= (1u << r);
    band.count = static_cast<int16_t>(r + 1);
  }

  void commit() const { record(band); }
};

}  // namespace ListTouchBand
