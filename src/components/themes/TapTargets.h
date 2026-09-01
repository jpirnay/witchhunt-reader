#pragma once

#include <atomic>
#include <cstdint>

// Rectangles a screen painted that a finger can mean something by, published by the draw for
// the input side to match against.
//
// The third recorder in this family, and it exists because the other two do not fit everything:
//
//   ButtonHintStrip  four equal boxes in a fixed strip, resolved to a raw button.
//   ListTouchBand    a vertical run of contiguous rows, resolved to an item index. Rows may
//                    differ in height but they always span the band's full width.
//   TapTargets       arbitrary rects, each carrying a value. For the elements that are neither
//                    a list nor a hint strip: the home cover tile(s), and the home menu -- which
//                    is a vertical row list in two themes and a HORIZONTAL icon strip in the
//                    carousel theme, so no y-band model can describe all three.
//
// Unlike the other two this is a type with named instances rather than one global, because the
// home screen has two of these live at the same time (its covers and its menu) and they must not
// overwrite each other.
//
// Coordinates are LIVE-orientation logical pixels, as for ListTouchBand: the home draws happen
// in whatever orientation the renderer is in, not in a forced Portrait like the hint strip.
namespace TapTargets {

// Ten is the home menu at its longest (browse, recents, bookmarks, OPDS, transfer, weather,
// settings, plus continue-reading) with room to spare; the cover recorders use three. One
// capacity for both keeps this a plain class instead of a capacity template, which in this
// firmware mints a fresh copy of the code in flash per instantiation.
inline constexpr int kMaxTargets = 10;

struct Target {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  // What the hit means to the screen: a menu entry index, a book index. Interpretation is the
  // recorder's and the consumer's shared business; this does not care.
  int16_t value = 0;
};

struct Set {
  Target items[kMaxTargets]{};
  int16_t count = 0;
};

// Pure geometry, split out so the arithmetic is testable without the shared state. Returns the
// VALUE of the first rect containing the point, or -1 for a miss. First rather than best: the
// recorders paint non-overlapping rects, and where the carousel's side covers do overlap the
// centre one, the one recorded first is the one drawn on top.
inline int hitTestIn(const Set& s, const int px, const int py) {
  for (int i = 0; i < s.count && i < kMaxTargets; ++i) {
    const Target& t = s.items[i];
    if (t.w <= 0 || t.h <= 0) continue;
    if (px >= t.x && px < t.x + t.w && py >= t.y && py < t.y + t.h) return t.value;
  }
  return -1;
}

// One published set. Seqlock for the same reason ButtonHintStrip has one: the draw runs on the
// render task and the hit test on the loop task, and a half-written Set would be matched against
// a mix of two screens' geometry. A torn read answers "nothing here", and the tap falls through
// to whoever else wants it -- the safe way to be wrong.
class Recorder {
 public:
  // Built up during a draw, then published in one call.
  class Builder {
   public:
    void add(const int x, const int y, const int w, const int h, const int value) {
      if (set_.count >= kMaxTargets) return;
      Target& t = set_.items[set_.count];
      t.x = static_cast<int16_t>(x);
      t.y = static_cast<int16_t>(y);
      t.w = static_cast<int16_t>(w);
      t.h = static_cast<int16_t>(h);
      t.value = static_cast<int16_t>(value);
      set_.count = static_cast<int16_t>(set_.count + 1);
    }
    const Set& set() const { return set_; }

   private:
    friend class Recorder;
    Set set_{};
  };

  void record(const Set& in) {
    seq_.fetch_add(1, std::memory_order_acq_rel);  // odd: write in progress
    storage_ = in;
    seq_.fetch_add(1, std::memory_order_release);  // even: stable
  }
  void record(const Builder& b) { record(b.set()); }

  // Forget the targets. Called on every activity transition, for the reason the other two
  // recorders document: a screen that paints none would otherwise inherit the previous
  // screen's rects and turn a tap into a phantom action.
  void invalidate() { record(Set{}); }

  bool snapshot(Set& out) const {
    const uint32_t before = seq_.load(std::memory_order_acquire);
    if (before == 0 || (before & 1u) != 0) return false;
    out = storage_;
    return seq_.load(std::memory_order_acquire) == before;
  }

  // True when something tappable is recorded, so a screen with none can leave the tap alone.
  bool hasTargets() const {
    Set s;
    return snapshot(s) && s.count > 0;
  }

  int hitTest(const int px, const int py) const {
    Set s;
    if (!snapshot(s)) return -1;
    return hitTestIn(s, px, py);
  }

 private:
  Set storage_{};
  mutable std::atomic<uint32_t> seq_{0};
};

// The home screen's two live sets. Function-local statics rather than namespace-scope objects so
// there is no static-init order to reason about between the themes that write them and the
// activity that reads them.
//
// The cover values are BOOK indices and the menu values are MENU-ENTRY indices, both zero-based.
// HomeActivity's own selector runs covers-then-menu across one range, so it is the one that adds
// the offset -- a theme drawing the menu has no idea how many covers sit above it.
inline Recorder& homeCovers() {
  static Recorder r;
  return r;
}
inline Recorder& homeMenu() {
  static Recorder r;
  return r;
}

// The settings tab bar. A horizontal strip of variable-width tabs, so no y-band model describes
// it and it cannot be a ListTouchBand -- the same reason the carousel theme's menu is here.
//
// Values are CATEGORY indices. Recorded by each theme's drawTabBar() from the same x it painted
// the label at, because the three themes space and pad their tabs differently and re-deriving
// that in the activity is a second copy of the layout rule.
inline Recorder& tabBar() {
  static Recorder r;
  return r;
}

}  // namespace TapTargets
