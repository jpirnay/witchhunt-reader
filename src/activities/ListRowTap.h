#pragma once

#include <atomic>
#include <cstdint>

// What a tap on a list row means, given where the selection already is.
//
// apply() implements the safer point-then-confirm rule: the first tap on a row MOVES the
// selection there and stops; only a tap on the row that is already selected runs the action.
// ActivationState applies the user's tap preference without confusing keyboard focus with a
// preceding tap. In two-step mode the first tap always arms the row; only the next tap on that
// same, still-selected row activates it.
//
// This replaces an earlier single-tap-activates design. The argument against two-step then was
// that it cost two panel refreshes per action, but that measured the wrong thing: it was one
// *contact* producing both a held-move and a release-activate, i.e. two refreshes for one
// gesture. Here each refresh belongs to its own deliberate tap, and the first one is the
// feedback — which on e-paper is the only feedback available until a localised tap-flash
// exists (docs/future_work/boards-and-touch.md, "Localized tap flash").
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

class ActivationState {
 public:
  void reset() { armedIndex = -1; }

  Result applyPreference(const int index, const Result result, const bool activateImmediately) {
    if (result == Result::Rejected) {
      reset();
      return result;
    }
    if (activateImmediately) {
      reset();
      return Result::Activate;
    }
    if (result == Result::Selected || armedIndex != index) {
      armedIndex = index;
      return Result::Selected;
    }
    reset();
    return Result::Activate;
  }

 private:
  int armedIndex = -1;
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

// Same, for a selection that lives in freeink::ui::ListNav, whose `selected` the SDK made a
// std::atomic<int> (upstream d78c3b2) because input and render reach it from different tasks.
// An atomic converts to int but will not bind to the int& above, and every screen that keeps its
// selection in the nav would otherwise repeat the same load/apply/store.
inline Result apply(const int index, const int count, std::atomic<int>& selection) {
  int current = selection.load();
  const Result result = apply(index, count, current);
  if (result == Result::Selected) selection.store(current);
  return result;
}

}  // namespace ListRowTap
