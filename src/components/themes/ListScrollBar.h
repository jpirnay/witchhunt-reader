#pragma once

#include <atomic>
#include <cstdint>

#include "TouchUi.h"

// Where the list scroll bar was painted, so a tap beside the thumb can page the list.
//
// The fourth recorder in the family, and it exists for a reason the other three do not
// cover: ListTouchBand records the ROWS, which is every tap that means "this item". A tap
// on the scroll bar means "somewhere else in the list", which is not an item at all.
//
// Why it is worth having when a vertical swipe already pages a list
// (ActivityManager::dispatchListSwipe): the swipe is INVISIBLE. Nothing on screen suggests
// it, so it is only reachable by someone who already knows. The bar is painted, so a tap
// on it is the discoverable version of the same action -- and on the T5S3 and X4 Pro, which
// have a Down key and no Up key, paging back has no physical button at all.
//
// Coordinates are LIVE-orientation logical pixels, as for ListTouchBand: the list draws
// happen in whatever orientation the renderer is in, not in a forced Portrait like the
// button-hint strip.
namespace ListScrollBar {

// The painted bar is four pixels wide on every shipped theme (BaseTheme's
// scrollBarWidth = 4). Four pixels is not a touch target -- so the TAPPABLE strip is
// widened to this, reaching left into the list, while the painted bar stays where it is.
// The eye aims at the bar and the finger is forgiven for missing it, which is how every
// scroll bar on a touch screen has to work.
inline constexpr int kMinHitWidth = 40;

struct Bar {
  int16_t hitX = 0;      // left edge of the tappable strip, NOT of the painted bar
  int16_t hitWidth = 0;  // 0 = nothing recorded
  int16_t top = 0;       // top of the track
  int16_t height = 0;    // track height
  int16_t thumbTop = 0;
  int16_t thumbHeight = 0;
};

// Above the thumb means back, below it means forward -- the direction the CONTENT moves,
// which is how a scroll bar has always read. A tap on the thumb itself is deliberately
// nothing: it is neither direction, and on e-paper there is no drag to start.
enum class Hit : uint8_t { None, PageBack, PageForward };

// Pure geometry, split out so the arithmetic is testable without the shared state -- the
// same split as the other three recorders.
inline Hit hitTestIn(const Bar& b, const int px, const int py) {
  if (b.hitWidth <= 0 || b.height <= 0) return Hit::None;
  if (px < b.hitX || px >= b.hitX + b.hitWidth) return Hit::None;
  if (py < b.top || py >= b.top + b.height) return Hit::None;
  if (b.thumbHeight > 0 && py >= b.thumbTop && py < b.thumbTop + b.thumbHeight) return Hit::None;
  return py < b.thumbTop ? Hit::PageBack : Hit::PageForward;
}

// Turn what the draw painted into what a finger can hit. Stated once, here, so the two
// themes that draw a bar cannot widen it differently.
//
// `paintedX` is the left edge of the painted bar and `listLeft` the left edge of the list,
// which is the clamp: the strip may reach inwards for a bigger target but must not escape
// the list and start claiming taps on a neighbouring column.
inline Bar fromPaintedBar(const int paintedX, const int paintedWidth, const int listLeft, const int top,
                          const int height, const int thumbTop, const int thumbHeight) {
  Bar bar{};
  const int paintedRight = paintedX + paintedWidth;
  int hitX = paintedRight - kMinHitWidth;
  if (hitX < listLeft) hitX = listLeft;
  if (hitX > paintedX) hitX = paintedX;  // never narrower than the bar itself
  bar.hitX = static_cast<int16_t>(hitX);
  bar.hitWidth = static_cast<int16_t>(paintedRight - hitX);
  bar.top = static_cast<int16_t>(top);
  bar.height = static_cast<int16_t>(height);
  bar.thumbTop = static_cast<int16_t>(thumbTop);
  bar.thumbHeight = static_cast<int16_t>(thumbHeight);
  return bar;
}

#if CP_TOUCH_UI
// Published across tasks, seqlock for the reason the other three document: the draw runs on
// the render task and the hit test on the loop task, and a half-written Bar would be
// hit-tested against a mix of two screens' geometry. A torn read answers "nothing here".
namespace detail {
inline Bar& storage() {
  static Bar b;
  return b;
}
inline std::atomic<uint32_t>& seq() {
  static std::atomic<uint32_t> s{0};
  return s;
}
}  // namespace detail

inline void record(const Bar& in) {
  auto& seq = detail::seq();
  seq.fetch_add(1, std::memory_order_acq_rel);  // odd: write in progress
  detail::storage() = in;
  seq.fetch_add(1, std::memory_order_release);  // even: stable
}

// Forget the bar. A screen that paints no scroll bar would otherwise inherit the previous
// screen's, and turn a tap near the right edge into a phantom page turn.
inline void invalidate() { record(Bar{}); }

inline bool snapshot(Bar& out) {
  auto& seq = detail::seq();
  const uint32_t before = seq.load(std::memory_order_acquire);
  if (before == 0 || (before & 1u) != 0) return false;
  out = detail::storage();
  return seq.load(std::memory_order_acquire) == before;
}

inline bool hasBar() {
  Bar b;
  return snapshot(b) && b.hitWidth > 0 && b.height > 0;
}

inline Hit hitTest(const int px, const int py) {
  Bar b;
  if (!snapshot(b)) return Hit::None;
  return hitTestIn(b, px, py);
}

#else  // !CP_TOUCH_UI

// No digitiser: nothing hit-tests the bar, so the storage is gone and the themes' record()
// calls compile away. hitTestIn() and fromPaintedBar() above stay -- they are pure
// arithmetic the host tests exercise either way.
inline void record(const Bar&) {}
inline void invalidate() {}
inline bool snapshot(Bar&) { return false; }
inline bool hasBar() { return false; }
inline Hit hitTest(int, int) { return Hit::None; }

#endif  // CP_TOUCH_UI

}  // namespace ListScrollBar
