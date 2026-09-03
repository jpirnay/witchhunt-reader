#pragma once

#include <cstdint>

// What a tap on a list row means, given where the selection already is.
//
// Point-then-confirm, not point-and-go: the first tap on a row MOVES the selection there and
// stops; only a tap on the row that is already selected runs the action. The reader therefore
// always sees the highlight land on their choice before anything happens, and a mis-tap costs
// a second tap rather than an action to undo.
//
// This replaces an earlier single-tap-activates design. The argument against two-step then was
// that it cost two panel refreshes per action, but that measured the wrong thing: it was one
// *contact* producing both a held-move and a release-activate, i.e. two refreshes for one
// gesture. Here each refresh belongs to its own deliberate tap, and the first one is the
// feedback — which on e-paper is the only feedback available until a localised tap-flash
// exists (P2 in docs/touch-input-migration-2026-08-14.md).
//
// Kept free of Activity so the rule is exercised on the host: this is a state machine about
// selection, and every screen in the firmware routes its taps through it.
namespace ListRowTap {

enum class Result : uint8_t {
  // Not a row this screen can act on right now — out of range, a separator, a screen whose
  // state has moved on since the render that recorded the band. The tap is consumed but inert.
  Rejected,
  // The selection moved here. Repaint; do NOT activate.
  Selected,
  // The finger landed on the row that was already selected. Activate it.
  Activate,
};

// The standard body, so nineteen screens do not each re-derive the comparison. `selection` is
// updated in place when the row is a new one.
//
// `count` is checked against the screen's CURRENT data rather than the data the render saw:
// the row band is recorded on the render task and a list can be rebuilt between that render and
// this tap.
inline Result apply(const int index, const int count, int& selection) {
  if (index < 0 || index >= count) return Result::Rejected;
  if (index == selection) return Result::Activate;
  selection = index;
  return Result::Selected;
}

}  // namespace ListRowTap
