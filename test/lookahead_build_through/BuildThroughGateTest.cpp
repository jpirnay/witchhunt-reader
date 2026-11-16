// BuildThroughGate: whether Background-B may build the next chapter through page turns on a panel.
#include <gtest/gtest.h>

#include <cstdint>

#include "activities/reader/BuildThroughGate.h"

namespace {

using BuildThroughGate::keepsFastRefresh;
using BuildThroughGate::mayBegin;
using BuildThroughGate::Panel;
using BuildThroughGate::seedsRed;

Panel panel(const bool needsHostFrame, const bool psramArena) {
  Panel p;
  p.fastRefreshNeedsHostFrame = needsHostFrame;
  p.lendsPsramArena = psramArena;
  return p;
}

const Panel kX3 = panel(false, false);        // its controller keeps the frame on screen (DTM1)
const Panel kX4 = panel(true, false);         // FAST diffs against RED, B lends the framebuffer
const Panel kPsramBoard = panel(true, true);  // B lends a PSRAM arena, the framebuffer stays

TEST(BuildThroughGate, TheX3BuildsThroughWithoutASeed) {
  EXPECT_TRUE(keepsFastRefresh(kX3));
  EXPECT_FALSE(seedsRed(kX3, true));  // no RED to seed
  EXPECT_TRUE(mayBegin(kX3, false, 0, 7));
}

// The X4, 2026-11-16: no build-through at all, so at a page every 2.5 s the next chapter was laid out in the
// foreground at every crossing. Its pages keep FAST on the RED that B's borrow seeds.
TEST(BuildThroughGate, TheX4BuildsThroughOnTheRedItsBorrowSeeded) {
  EXPECT_TRUE(keepsFastRefresh(kX4));
  EXPECT_TRUE(seedsRed(kX4, true));
  EXPECT_TRUE(mayBegin(kX4, /*redSeeded=*/true, /*seedFrame=*/41, /*framesShipped=*/41));
}

TEST(BuildThroughGate, TheX4WaitsWhenTheSecondaryNoLongerHoldsTheFrameOnScreen) {
  // An image decode lent the buffer since the page went out: it holds the write buffer's contents.
  EXPECT_FALSE(seedsRed(kX4, false));
  EXPECT_FALSE(mayBegin(kX4, /*redSeeded=*/false, 41, 41));
}

TEST(BuildThroughGate, TheX4WaitsWhenAFrameShippedSinceTheSeed) {
  EXPECT_FALSE(mayBegin(kX4, /*redSeeded=*/true, /*seedFrame=*/41, /*framesShipped=*/42));
}

TEST(BuildThroughGate, ABoardThatLendsAPsramArenaIsLeftOut) {
  EXPECT_FALSE(keepsFastRefresh(kPsramBoard));
  EXPECT_FALSE(seedsRed(kPsramBoard, true));
  EXPECT_FALSE(mayBegin(kPsramBoard, true, 41, 41));
}

}  // namespace
