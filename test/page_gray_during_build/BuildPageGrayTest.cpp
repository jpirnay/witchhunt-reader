// BuildPageGray: when the page a chapter build drew gets its anti-aliasing.
#include <gtest/gtest.h>

#include "activities/reader/BuildPageGray.h"

namespace {

using BuildPageGray::asksPassNow;
using BuildPageGray::Gray;
using BuildPageGray::owedAtBuildEnd;
using BuildPageGray::whileBuilding;

TEST(BuildPageGray, WhereTheBuildLeavesTheFramesAloneAPageOfTextGetsItsGrayWhileTheBuildGoesOn) {
  EXPECT_TRUE(whileBuilding(/*buildLeavesFramesAlone=*/true, /*antiAliasing=*/true, /*pageHasImages=*/false,
                            /*somethingOverPage=*/false));
}

TEST(BuildPageGray, OnTheC3sTheGrayWaitsForTheBuildsEnd) {
  // The X3's build holds the secondary buffer, the X4's resident build the heap: neither leaves the frames alone.
  EXPECT_FALSE(whileBuilding(/*buildLeavesFramesAlone=*/false, true, false, false));
}

TEST(BuildPageGray, WithoutAntiAliasingThereIsNoGray) {
  EXPECT_FALSE(whileBuilding(true, /*antiAliasing=*/false, false, false));
}

TEST(BuildPageGray, APageWithPicturesIsDrawnAgainOnceTheBuildIsDone) {
  EXPECT_FALSE(whileBuilding(true, true, /*pageHasImages=*/true, false));
}

TEST(BuildPageGray, UnderTheLiveBarsOrASearchStripThePageStaysBlackAndWhite) {
  EXPECT_FALSE(whileBuilding(true, true, false, /*somethingOverPage=*/true));
}

TEST(BuildPageGray, ThePassIsAskedForOnceThePagesRefreshIsOver) {
  EXPECT_TRUE(asksPassNow(Gray::None, /*refreshPending=*/false, /*passOwed=*/false));
  // While the refresh paints, the build's slices go on: an owed pass would hold them until it ran.
  EXPECT_FALSE(asksPassNow(Gray::None, /*refreshPending=*/true, false));
}

TEST(BuildPageGray, ThePassIsAskedForOnceAPage) {
  EXPECT_FALSE(asksPassNow(Gray::Asked, false, false));  // run, or preempted: the build's end asks again
  EXPECT_FALSE(asksPassNow(Gray::Shown, false, false));  // went up with the page in one push
}

TEST(BuildPageGray, NotWhileAnotherPassIsOwed) { EXPECT_FALSE(asksPassNow(Gray::None, false, /*passOwed=*/true)); }

TEST(BuildPageGray, TheBuildsEndAsksForTheGrayUnlessItIsOnThePanel) {
  EXPECT_TRUE(owedAtBuildEnd(Gray::None));    // a C3, or no pass yet
  EXPECT_TRUE(owedAtBuildEnd(Gray::Asked));   // the pass was preempted
  EXPECT_FALSE(owedAtBuildEnd(Gray::Shown));  // nothing drawn twice
}

}  // namespace
