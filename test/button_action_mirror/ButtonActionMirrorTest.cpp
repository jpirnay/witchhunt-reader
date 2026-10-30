#include <gtest/gtest.h>

#include "CrossPointSettings.h"

namespace {

using S = CrossPointSettings;

TEST(ButtonActionMirror, EveryDirectionalPairSwaps) {
  EXPECT_EQ(S::BTN_PAGE_BACK, S::mirroredDirection(S::BTN_PAGE_FORWARD));
  EXPECT_EQ(S::BTN_PAGE_FORWARD, S::mirroredDirection(S::BTN_PAGE_BACK));
  EXPECT_EQ(S::BTN_PAGE_BACK_10, S::mirroredDirection(S::BTN_PAGE_FORWARD_10));
  EXPECT_EQ(S::BTN_PAGE_FORWARD_10, S::mirroredDirection(S::BTN_PAGE_BACK_10));
  EXPECT_EQ(S::BTN_PREV_SECTION, S::mirroredDirection(S::BTN_NEXT_SECTION));
  EXPECT_EQ(S::BTN_NEXT_SECTION, S::mirroredDirection(S::BTN_PREV_SECTION));
}

TEST(ButtonActionMirror, AMirrorOfAMirrorIsTheAction) {
  for (uint8_t a = 0; a < S::BUTTON_ACTION_COUNT; ++a) {
    EXPECT_EQ(a, S::mirroredDirection(S::mirroredDirection(a))) << int(a);
  }
}

TEST(ButtonActionMirror, OtherActionsAreUnchanged) {
  for (const uint8_t a : {S::BTN_DEFAULT, S::BTN_GO_HOME, S::BTN_OPEN_TOC, S::BTN_READER_MENU, S::BTN_IGNORE,
                          S::BTN_FONT_SIZE_SMALLER, S::BTN_FONT_SIZE_LARGER, S::BTN_CYCLE_ORIENTATION,
                          S::BTN_CYCLE_ORIENTATION_BACK}) {
    EXPECT_EQ(a, S::mirroredDirection(a)) << int(a);
  }
}

}  // namespace
