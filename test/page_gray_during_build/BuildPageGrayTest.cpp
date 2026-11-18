// BuildPageGray: when the page a chapter build drew gets its anti-aliasing.
#include <gtest/gtest.h>

#include "activities/reader/BuildPageGray.h"

namespace {

using BuildPageGray::asksPassNow;
using BuildPageGray::Gray;
using BuildPageGray::owedAtBuildEnd;
using BuildPageGray::Pass;
using BuildPageGray::passFor;
using BuildPageGray::whileBuilding;

// --- Which pass ---------------------------------------------------------------------------------------------------

TEST(BuildPageGray, OnTheS3sTheGrayIsRestoredFromTheSecondaryBuffer) {
  // The build lays out in PSRAM: the frame on screen stays in the secondary buffer (#808).
  EXPECT_EQ(passFor(/*buildsInPsram=*/true, /*keepsPreviousFrame=*/true, /*buildHoldsSecondary=*/false), Pass::Restore);
}

TEST(BuildPageGray, OnAnS3WhoseBufferItselfWasLentTheGrayWaitsForTheBuildsEnd) {
  // The PSRAM arena could not be had: as before #808's pass, whatever the build holds.
  EXPECT_EQ(passFor(true, /*keepsPreviousFrame=*/false, /*buildHoldsSecondary=*/false), Pass::AtBuildEnd);
  EXPECT_EQ(passFor(true, false, /*buildHoldsSecondary=*/true), Pass::AtBuildEnd);
}

TEST(BuildPageGray, OnTheC3sABuildThatHoldsTheSecondaryBufferRedrawsThePage) {
  // The X3's build, and the X4's released one: the secondary buffer is the build's arena.
  EXPECT_EQ(passFor(/*buildsInPsram=*/false, /*keepsPreviousFrame=*/false, /*buildHoldsSecondary=*/true), Pass::Redraw);
}

TEST(BuildPageGray, TheX4sResidentBuildLeavesTheGrayToTheBuildsEnd) {
  // It keeps the secondary buffer but runs on the heap its low-heap guard watches.
  EXPECT_EQ(passFor(false, /*keepsPreviousFrame=*/true, /*buildHoldsSecondary=*/false), Pass::AtBuildEnd);
}

// --- R2: the gray starts when the panel is free, not at the build's end --------------------------------------------

TEST(BuildPageGray, WhereTheBuildLeavesTheFramesAloneAPageOfTextGetsItsGrayWhileTheBuildGoesOn) {
  EXPECT_TRUE(whileBuilding(Pass::Restore, /*antiAliasing=*/true, /*pageHasImages=*/false,
                            /*somethingOverPage=*/false));
}

TEST(BuildPageGray, OnTheC3sWhereTheBuildHoldsTheSecondaryBufferAPageOfTextGetsItsGrayWhileTheBuildGoesOn) {
  const Pass pass = passFor(/*buildsInPsram=*/false, /*keepsPreviousFrame=*/false, /*buildHoldsSecondary=*/true);
  EXPECT_EQ(pass, Pass::Redraw);
  EXPECT_TRUE(whileBuilding(pass, /*antiAliasing=*/true, /*pageHasImages=*/false, /*somethingOverPage=*/false));
}

TEST(BuildPageGray, WhereNeitherPassCanRunTheGrayWaitsForTheBuildsEnd) {
  EXPECT_FALSE(whileBuilding(Pass::AtBuildEnd, true, false, false));
}

TEST(BuildPageGray, WithoutAntiAliasingThereIsNoGray) {
  EXPECT_FALSE(whileBuilding(Pass::Restore, /*antiAliasing=*/false, false, false));
  EXPECT_FALSE(whileBuilding(Pass::Redraw, /*antiAliasing=*/false, false, false));
}

TEST(BuildPageGray, APageWithPicturesIsDrawnAgainOnceTheBuildIsDone) {
  EXPECT_FALSE(whileBuilding(Pass::Restore, true, /*pageHasImages=*/true, false));
  EXPECT_FALSE(whileBuilding(Pass::Redraw, true, /*pageHasImages=*/true, false));
}

TEST(BuildPageGray, UnderTheLiveBarsOrASearchStripThePageStaysBlackAndWhite) {
  EXPECT_FALSE(whileBuilding(Pass::Restore, true, false, /*somethingOverPage=*/true));
  EXPECT_FALSE(whileBuilding(Pass::Redraw, true, false, /*somethingOverPage=*/true));
}

// --- When, and once -----------------------------------------------------------------------------------------------

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
  EXPECT_TRUE(owedAtBuildEnd(Gray::None));    // the X4's resident build, or no pass yet
  EXPECT_TRUE(owedAtBuildEnd(Gray::Asked));   // the pass was preempted, or found no room
  EXPECT_FALSE(owedAtBuildEnd(Gray::Shown));  // nothing drawn twice
}

}  // namespace
