#pragma once

// The slider's value <-> pixel mapping, in one place and both directions.
//
// Split out of SliderPickerActivity so the draw and the touch hit test cannot disagree about
// where a value sits on the bar. They are inverses of each other, and the failure they prevent
// is the one a reader notices immediately: tapping the knob and watching it jump somewhere else.
//
// Pure arithmetic — no renderer, no theme — so it is exercised on the host.
namespace SliderGeometry {

// The bar is drawn as a 1 px outlined rect with the fill inset 2 px on every side, so the run a
// value actually travels is 4 px narrower than the bar. Both directions work in that inner
// track, not in the bar.
inline constexpr int kTrackInset = 2;

inline int trackX(const int barX) { return barX + kTrackInset; }
inline int trackWidth(const int barWidth) { return barWidth - 2 * kTrackInset; }

// Pixels of fill for `value` — what render() paints.
inline int fillWidthFor(const int value, const int barWidth, const int minValue, const int maxValue) {
  const int range = maxValue - minValue;
  if (range <= 0) return 0;
  // A bar narrower than its own two insets yields a negative track, and a negative fill width
  // reaches fillRect() as a huge unsigned span. Guarded here as well as in valueForX(), which
  // has the same trap: the two are inverses and must be defensible on the same inputs.
  const int track = trackWidth(barWidth);
  if (track <= 0) return 0;
  const int clamped = value < minValue ? minValue : (value > maxValue ? maxValue : value);
  return track * (clamped - minValue) / range;
}

// The value a finger at `px` means — the inverse.
//
// Rounds to the nearest step rather than truncating, so the value under the fingertip is the
// one nearest it rather than always the one below: with truncation the top of a 0..100 range is
// unreachable by tap, because only x at the very last pixel yields 100.
//
// Clamps rather than rejecting out-of-range x on purpose. The caller's touch band is
// deliberately taller AND wider than the drawn bar (a 16 px bar is far below a finger-sized
// target), so a tap just off either end is a genuine "go to the end" and not a miss.
inline int valueForX(const int px, const int barX, const int barWidth, const int minValue, const int maxValue) {
  const int range = maxValue - minValue;
  if (range <= 0) return minValue;
  const int track = trackWidth(barWidth);
  if (track <= 0) return minValue;
  const int dx = px - trackX(barX);
  if (dx <= 0) return minValue;
  if (dx >= track) return maxValue;
  return minValue + (dx * range + track / 2) / track;
}

}  // namespace SliderGeometry
