#pragma once

#include "activities/Activity.h"

// Drives the panel through a conditioning cycle to clear accumulated ghosting.
//
// Why this exists as a user action rather than as part of the refresh cadence: on the LGFX
// panels every page turn goes out on a differential bank that deliberately skips the eraser, so
// the physical state of the ink drifts from the model the driver diffs against. The periodic
// scrub recovers some of it and not all, and the board has no deeper waveform to escalate to --
// its lutQuality is a deliberate one-word stub, because a real third LUT bank once pushed the
// fast bank past the block-offset limit and killed every refresh below ~27 C.
//
// So the drift is cleared the way e-ink displays have always cleared it: by driving every pixel
// hard between the rails several times over. That is far too slow to do between pages, which is
// why it is a maintenance action and not a refresh mode.
//
// Modelled on the `repair()` routine in azw413/lilygo-t5s3paperpro-rs, an independent Rust
// driver for this panel family, which clears, drives the panel to black repeatedly, clears,
// drives it to white rather more, and clears again. The asymmetry is theirs and is kept: white
// is the rest state, so ending there leaves the least charge behind.
class ScreenRepairActivity final : public Activity {
 public:
  explicit ScreenRepairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ScreenRepair", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The cycle is a long run of blocking panel pushes; don't let the loop drop into power saving
  // underneath it. Same reason ClearCacheActivity does this.
  bool skipLoopDelay() override { return true; }

 private:
  enum State { WARNING, REPAIRING, DONE };
  State state = WARNING;

  // Each push is a clean-bank refresh -- around a second and a half on this panel -- so these
  // counts are what set the run time, roughly 20 seconds. Kept low enough that a reader who
  // starts it by accident is not stranded, high enough to be worth running.
  static constexpr int kAlternations = 5;  // black/white pairs
  static constexpr int kTrailingWhite = 3;

  void runRepairCycle();
};
