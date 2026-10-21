// OnOffOverride: a per-book override (-1 / 0 / 1) shown as an Off / On row, the reader menu's Bionic reading.
#include <gtest/gtest.h>

#include "activities/reader/OnOffOverride.h"

namespace {

using OnOffOverride::forChoice;
using OnOffOverride::isOn;

constexpr int8_t kOverrides[] = {-1, 0, 1};

TEST(OnOffOverride, TheRowShowsWhatThePageDoes) {
  EXPECT_FALSE(isOn(-1, false));  // no override: the global setting
  EXPECT_TRUE(isOn(-1, true));
  EXPECT_FALSE(isOn(0, true));  // an override wins either way
  EXPECT_TRUE(isOn(1, false));
}

// X3, 2026-10-21: a book without an override came back pinned to the global value on a plain Back.
TEST(OnOffOverride, ARowLeftAloneHandsBackTheOverrideTheBookOpenedWith) {
  for (const int8_t opened : kOverrides) {
    for (const bool globalOn : {false, true}) {
      EXPECT_EQ(opened, forChoice(isOn(opened, globalOn), opened, globalOn))
          << "opened " << int{opened} << " global " << globalOn;
    }
  }
}

TEST(OnOffOverride, AFlippedRowSetsTheBooksOwnValue) {
  EXPECT_EQ(1, forChoice(true, -1, false));
  EXPECT_EQ(0, forChoice(false, -1, true));
  EXPECT_EQ(0, forChoice(false, 1, false));
  EXPECT_EQ(1, forChoice(true, 0, true));
}

// Confirm cycles the two values, so a row flipped twice is back where it started and changes nothing.
TEST(OnOffOverride, ARowFlippedThereAndBackChangesNothing) {
  for (const int8_t opened : kOverrides) {
    for (const bool globalOn : {false, true}) {
      int8_t pending = opened;
      pending = forChoice(!isOn(pending, globalOn), opened, globalOn);
      EXPECT_NE(isOn(opened, globalOn), isOn(pending, globalOn));
      pending = forChoice(!isOn(pending, globalOn), opened, globalOn);
      EXPECT_EQ(opened, pending) << "opened " << int{opened} << " global " << globalOn;
    }
  }
}

}  // namespace
