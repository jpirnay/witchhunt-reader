#pragma once

#include "TouchUi.h"
#include "activities/Activity.h"

#if CP_TOUCH_UI

// Shows WHERE the touch zones are and what each one currently does, on the device.
//
// The counterpart of ButtonActionsOverviewActivity, and the same trick: every action string
// comes from the shared settings list via SettingInfo::getDisplayValue(), so this cannot
// drift from what is actually bound. That includes "Built-in" rows, whose label is computed
// by TouchGestures::builtinLabelFor() and therefore names what the reader will really do --
// a picture maintained by hand could disagree with the firmware, and this one cannot.
//
// The zones are the part a settings list cannot convey: "Long tap top-left corner" is a
// position on the glass, and a list of nineteen rows does not tell anyone where it is. So
// each page draws the frame and puts the bound action inside the region it belongs to.
class GestureActionsOverviewActivity final : public Activity {
 public:
  explicit GestureActionsOverviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GestureActionsOverview", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int page = 0;

  // Two pages always, plus a third for pinch/rotate where the controller reports more than
  // one contact. Asked of the HAL rather than assumed, exactly as the settings rows are.
  int pageCount() const;
};

#endif  // CP_TOUCH_UI
