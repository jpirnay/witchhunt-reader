// LightSleepGate: what on the panel's side keeps the idle loop from halting the chip.
#include <gtest/gtest.h>

#include "LightSleepGate.h"

namespace {

using LightSleepGate::PanelHold;
using LightSleepGate::panelHold;

TEST(LightSleepGate, AnIdlePanelLetsTheChipSleep) {
  EXPECT_EQ(panelHold(/*lockHeld=*/false, /*refreshPending=*/false), PanelHold::None);
}

TEST(LightSleepGate, ARefreshNothingHasFinishedKeepsTheChipAwake) {
  // The busy indicator or a popup shipped without waiting, Calibre's status screen: no pass holds a Lock for them.
  EXPECT_EQ(panelHold(/*lockHeld=*/false, /*refreshPending=*/true), PanelHold::RefreshPending);
}

TEST(LightSleepGate, ARenderPassKeepsTheChipAwakeAsBefore) {
  EXPECT_EQ(panelHold(/*lockHeld=*/true, /*refreshPending=*/false), PanelHold::RenderLock);
}

TEST(LightSleepGate, APassThatFinishesItsOwnRefreshIsCountedAsALock) {
  // renderContents() triggers, then collects with the Lock still held: counted as before the guard.
  EXPECT_EQ(panelHold(/*lockHeld=*/true, /*refreshPending=*/true), PanelHold::RenderLock);
}

}  // namespace
