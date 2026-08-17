#pragma once

#include <cstddef>

// Where the four bottom button hints sit.
//
// This used to be `gpio.deviceIsX3() ? x3Positions : x4Positions`, which put every
// S3 board on the X4's layout — geometry for a 480px-wide panel, on a 540px one.
//
// The tuned sets are not arbitrary spacing: they mirror the X3/X4 four-button
// strip along the bottom bezel, which is why Lyra's carry a wide middle gap
// (x3: gaps 12, 54, 12) splitting the hints into the same two pairs as the keys.
// That is real board geometry, but it is geometry the SDK does not model — there
// is no hint-position field in BoardProfile, because it describes where buttons
// sit in a *case*, not what the hardware can do.
//
// So the tuned layouts are kept verbatim for the panel widths they were measured
// on, and anything else is spread evenly. A board without that button strip has
// nothing to line its hints up with: on the T5S3 the four hints are gestures on
// two inputs in different places (tap/hold of a side key and of a capacitive key
// below the panel), so pairing them like a key strip would imply a physical
// arrangement that does not exist.
//
// Keyed on screen width rather than board identity because width is what the
// layout actually depends on — and it keeps both validated C3 boards
// pixel-identical.
namespace ButtonHintLayout {

// Panel widths whose layouts were hand-tuned on hardware, in the reader's
// portrait frame: the X4 (and X3-in-X4-mode) at 480, the X3 at 528.
inline constexpr int TUNED_WIDTH_X4 = 480;
inline constexpr int TUNED_WIDTH_X3 = 528;

// Fills `out` with the x position of each of the four hint boxes.
// `tunedX4` / `tunedX3` are the theme's measured layouts for those widths.
inline void positions(int screenWidth, int buttonWidth, const int (&tunedX4)[4], const int (&tunedX3)[4],
                      int (&out)[4]) {
  const int* tuned = nullptr;
  if (screenWidth == TUNED_WIDTH_X4) {
    tuned = tunedX4;
  } else if (screenWidth == TUNED_WIDTH_X3) {
    tuned = tunedX3;
  }
  if (tuned != nullptr) {
    for (int i = 0; i < 4; ++i) out[i] = tuned[i];
    return;
  }

  // Even spread: five equal spaces — two outer margins and three inner gaps —
  // around four boxes. Degrades gracefully if the boxes do not fit (negative
  // slack clamps the gap to zero and they simply abut).
  const int slack = screenWidth - 4 * buttonWidth;
  const int gap = slack > 0 ? slack / 5 : 0;
  for (int i = 0; i < 4; ++i) {
    out[i] = gap + i * (buttonWidth + gap);
  }
}

}  // namespace ButtonHintLayout
