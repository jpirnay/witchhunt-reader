#pragma once

#include <memory>

#include "ReaderBarsController.h"
#include "activities/Activity.h"

// The reader bars (ReaderBarsController) as a screen of their own, pushed over the reader where it does not keep
// running under them (the X3 and X4): they paint over the page the reader left on the panel, and the reader lays the
// book out with the Layout tab's values once they close.
class ReaderBarsActivity final : public Activity {
 public:
  using Info = ReaderBarsController::Info;

  ReaderBarsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Info info);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The bars paint only their bands over the frame the reader prepared before pushing them; a busy indicator synced
  // in between would put another frame under them.
  bool suppressesBusyIndicator() const override { return true; }
  void appendDebugState(std::string& out) const override { bars.appendDebugState(out); }

 private:
  ReaderBarsController bars;
  // The opening pass composites over the frame the reader prepared; every later one syncs first (render()).
  bool firstRender = true;
};
