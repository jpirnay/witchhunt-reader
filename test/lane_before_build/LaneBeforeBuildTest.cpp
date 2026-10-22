// LaneBeforeBuild: who has the secondary framebuffer when an image decode and Background-B's build both want it.
#include <gtest/gtest.h>

#include "activities/reader/LaneBeforeBuild.h"

namespace {

using LaneBeforeBuild::buildMayBorrow;
using LaneBeforeBuild::Decode;
using LaneBeforeBuild::decodeWhileBuilding;
using LaneBeforeBuild::kMaxYields;
using LaneBeforeBuild::Window;

TEST(LaneBeforeBuild, TheBuildTakesTheBufferOnlyOnceTheLaneIsDoneWithItsWindow) {
  EXPECT_TRUE(buildMayBorrow(Window::Clean));     // looked at, nothing to decode
  EXPECT_TRUE(buildMayBorrow(Window::Closed));    // the lane cannot run: B decides by its own gates
  EXPECT_FALSE(buildMayBorrow(Window::Unknown));  // not looked at yet: the pictures ahead may need the buffer
  EXPECT_FALSE(buildMayBorrow(Window::Dirty));    // the lane is decoding them
}

TEST(LaneBeforeBuild, ADecodeTakesTheBufferFromTheBuildWhileItHasYieldsLeft) {
  for (uint8_t yields = 0; yields < kMaxYields; ++yields) {
    EXPECT_EQ(Decode::TakeFromBuild, decodeWhileBuilding(yields)) << static_cast<int>(yields);
  }
}

TEST(LaneBeforeBuild, PastItsYieldsTheBuildKeepsTheBuffer) {
  EXPECT_EQ(Decode::AfterBuild, decodeWhileBuilding(kMaxYields));
  EXPECT_EQ(Decode::AfterBuild, decodeWhileBuilding(255));
}

}  // namespace
