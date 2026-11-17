// PopupProbe: when CMD:STATE reports a popup as on screen.
#include <gtest/gtest.h>

#include "util/PopupProbe.h"

namespace {

using PopupProbe::Mark;
using PopupProbe::onScreen;

Mark mark(const uint32_t activity, const uint32_t pass, const uint32_t frames) {
  Mark m;
  m.activity = activity;
  m.renderPass = pass;
  m.frames = frames;
  return m;
}

// Drawn and shipped (blocking or async): its own frame is on the panel.
TEST(PopupProbe, APopupIsOnScreenWithItsOwnFrame) {
  EXPECT_TRUE(onScreen(mark(3, 10, 40), mark(3, 10, 40)));  // drawn, not shipped yet (PopupShip::Caller)
  EXPECT_TRUE(onScreen(mark(3, 10, 40), mark(3, 10, 41)));  // its own ship
}

// A chapter build that ended while its popup painted puts its page up in the same render pass: one more frame.
TEST(PopupProbe, APageShippedInTheSamePassEndsIt) { EXPECT_FALSE(onScreen(mark(3, 10, 40), mark(3, 10, 42))); }

TEST(PopupProbe, AnotherScreenOrALaterPassEndsIt) {
  EXPECT_FALSE(onScreen(mark(3, 10, 40), mark(4, 10, 41)));
  EXPECT_FALSE(onScreen(mark(3, 10, 40), mark(3, 11, 41)));
}

// The first open's progress bar ships the popup again (notePopupContinued moves the mark to that frame).
TEST(PopupProbe, AProgressBarKeepsItReported) {
  Mark noted = mark(3, 10, 40);
  EXPECT_FALSE(onScreen(noted, mark(3, 10, 42)));  // the bar's frame, unmarked
  noted.frames = 41;                               // marked: the frame just shipped is the popup's own
  EXPECT_TRUE(onScreen(noted, mark(3, 10, 42)));
}

}  // namespace
