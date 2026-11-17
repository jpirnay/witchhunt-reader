// PageComplete: when the page the reader asked for is complete on the panel, which is what CMD:IDLE:SCREEN waits for.
#include <gtest/gtest.h>

#include "activities/reader/PageComplete.h"

namespace {

using PageComplete::BuildPage;
using PageComplete::Reader;
using PageComplete::screenWorkPending;

// A page being read, nothing owed.
Reader settled() {
  Reader r;
  r.sectionLoaded = true;
  r.reading = true;
  return r;
}

// A chapter being laid out, its page of text on the panel and no gray owed (anti-aliasing off, or its gray shown).
Reader buildWithItsPageUp() {
  Reader r;
  r.sectionLoaded = true;
  r.buildLive = true;
  r.buildPage.shown = true;
  return r;
}

TEST(PageComplete, APageBeingReadIsComplete) { EXPECT_FALSE(screenWorkPending(settled())); }

TEST(PageComplete, NoSectionYetIsScreenWork) {
  Reader r = settled();
  r.sectionLoaded = false;
  EXPECT_TRUE(screenWorkPending(r));
}

TEST(PageComplete, ATurnOnItsWayToThePanelIsScreenWork) {
  Reader r = settled();
  r.turnHandedToRender = true;
  EXPECT_TRUE(screenWorkPending(r));
}

TEST(PageComplete, AnOwedGrayPassIsScreenWork) {
  Reader r = settled();
  r.grayPassOwed = true;
  EXPECT_TRUE(screenWorkPending(r));
}

TEST(PageComplete, ABlockingBuildIsScreenWork) {
  Reader r = settled();
  r.reading = false;
  EXPECT_TRUE(screenWorkPending(r));
}

// The case the gate exists for: the build goes on, the page is complete.
TEST(PageComplete, ALiveBuildIsNotScreenWorkOnceItsPageIsUpAndNoGrayIsOwed) {
  EXPECT_FALSE(screenWorkPending(buildWithItsPageUp()));
}

TEST(PageComplete, ALiveBuildIsScreenWorkUntilItsPageIsUp) {
  Reader r = buildWithItsPageUp();
  r.buildPage.shown = false;  // the Indexing popup, or a turn to a page not laid out yet
  EXPECT_TRUE(screenWorkPending(r));
}

TEST(PageComplete, ABuildPageWaitsForItsGray) {
  Reader r = buildWithItsPageUp();
  r.buildPage.grayOwed = true;  // the C3s: it comes at the build's end
  EXPECT_TRUE(screenWorkPending(r));
}

TEST(PageComplete, ABuildPageWithPicturesIsDrawnAgain) {
  Reader r = buildWithItsPageUp();
  r.buildPage.hasImages = true;
  EXPECT_TRUE(screenWorkPending(r));
}

TEST(PageComplete, AGrayPassAskedForDuringTheBuildIsScreenWork) {
  Reader r = buildWithItsPageUp();
  r.grayPassOwed = true;  // the S3s: armed between two slices
  EXPECT_TRUE(screenWorkPending(r));
}

// While the build is live the reader is not in its reading phase: that alone must not read as screen work.
TEST(PageComplete, TheBuildsPhaseDoesNotCountWhileItsPageIsComplete) {
  Reader r = buildWithItsPageUp();
  r.reading = false;
  EXPECT_FALSE(screenWorkPending(r));
  EXPECT_TRUE(PageComplete::buildPageComplete(r.buildPage));
}

}  // namespace
