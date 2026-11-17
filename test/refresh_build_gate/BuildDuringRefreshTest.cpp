// BuildDuringRefresh: whether the chapter build runs through the refresh of the page it drew.
#include <gtest/gtest.h>

#include "activities/reader/BuildDuringRefresh.h"

namespace {

using BuildDuringRefresh::Slice;
using BuildDuringRefresh::sliceWaits;
using BuildDuringRefresh::waitsOffBus;

Slice slice(const bool pending, const bool cardOffPanelBus, const bool buildPageWaitOffBus) {
  Slice s;
  s.refreshPending = pending;
  s.cardOffPanelBus = cardOffPanelBus;
  s.buildPageWaitOffBus = buildPageWaitOffBus;
  return s;
}

// The X4: the card is on the panel's bus, and the build page's refresh is waited for without the bus lock.
TEST(BuildDuringRefresh, BuildRunsThroughItsOwnPagesRefreshWhereTheWaitLeavesTheBus) {
  EXPECT_TRUE(waitsOffBus(/*refreshPending=*/true, /*refreshLeavesBusToCard=*/true));
  EXPECT_FALSE(sliceWaits(slice(/*pending=*/true, /*cardOffPanelBus=*/false, /*buildPageWaitOffBus=*/true)));
}

// The X3 and the T5S3: the panel does not leave the bus to the card, so the wait keeps the lock and the slice waits.
TEST(BuildDuringRefresh, SliceWaitsWhereTheRefreshKeepsTheBus) {
  EXPECT_FALSE(waitsOffBus(/*refreshPending=*/true, /*refreshLeavesBusToCard=*/false));
  EXPECT_TRUE(sliceWaits(slice(/*pending=*/true, /*cardOffPanelBus=*/false, /*buildPageWaitOffBus=*/false)));
}

// Another refresh pending during a build on the X4 (the bars over the page): its wait holds the bus, the slice waits.
TEST(BuildDuringRefresh, OnlyTheBuildPagesRefreshIsBuiltThrough) {
  EXPECT_TRUE(sliceWaits(slice(/*pending=*/true, /*cardOffPanelBus=*/false, /*buildPageWaitOffBus=*/false)));
}

// The X4 Pro, Murphy and Picco: as before, the card has its own bus and every pending refresh is built through.
TEST(BuildDuringRefresh, CardOnItsOwnBusBuildsThroughEveryRefresh) {
  EXPECT_FALSE(sliceWaits(slice(/*pending=*/true, /*cardOffPanelBus=*/true, /*buildPageWaitOffBus=*/false)));
}

// A driver that finished the refresh in the call that started it, and Dark Mode's blocking path: nothing to wait for.
TEST(BuildDuringRefresh, NothingPendingNothingWaits) {
  EXPECT_FALSE(waitsOffBus(/*refreshPending=*/false, /*refreshLeavesBusToCard=*/true));
  EXPECT_FALSE(sliceWaits(slice(/*pending=*/false, /*cardOffPanelBus=*/false, /*buildPageWaitOffBus=*/false)));
  EXPECT_FALSE(sliceWaits(slice(/*pending=*/false, /*cardOffPanelBus=*/false, /*buildPageWaitOffBus=*/true)));
}

}  // namespace
