// IndexingPopupGate: how an "Indexing" popup ships, so that the work behind it starts while it paints.
#include <gtest/gtest.h>

#include "activities/reader/IndexingPopupGate.h"

namespace {

using IndexingPopupGate::buildsThroughPopup;
using IndexingPopupGate::cardReadsDuringRefresh;
using IndexingPopupGate::chapterLend;
using IndexingPopupGate::firstOpenLend;
using IndexingPopupGate::indexesThroughPopup;
using IndexingPopupGate::Lend;
using IndexingPopupGate::Panel;

// What ReaderUtils::indexingPopupPanel() reads on each board, light mode, at rest (the secondary home, holding the
// frame on the glass).
Panel x3() {
  Panel p;
  p.asyncRefresh = true;
  p.refreshHoldsFrame = true;
  p.lendsDuringRefresh = true;
  p.hasSecondaryBuffer = true;
  return p;
}

Panel x4() {
  Panel p;
  p.asyncRefresh = true;
  p.refreshLeavesBusToCard = true;
  p.lendsDuringRefresh = true;
  p.hasSecondaryBuffer = true;
  return p;
}

// The X4 Pro, and the Picco and the Murphy since their drivers defer a refresh (#811): the card on SDMMC, PSRAM.
Panel sdmmcS3() {
  Panel p;
  p.asyncRefresh = true;
  p.cardOffPanelBus = true;
  p.decodesThroughRefresh = true;
  p.lendsPsramArena = true;
  p.hasSecondaryBuffer = true;
  return p;
}

// The LilyGo T5S3: PSRAM, the card on SPI, a parallel panel whose waveform a host task clocks out.
Panel t5s3() {
  Panel p;
  p.asyncRefresh = true;
  p.lendsPsramArena = true;
  p.hasSecondaryBuffer = true;
  return p;
}

Panel darkMode(Panel p) {
  p.asyncRefresh = false;  // FreeInkDisplay::supportsAsyncRefresh() while the output is inverted
  return p;
}

TEST(IndexingPopupGate, TheCardIsReadDuringARefreshWhereItsBusAllowsIt) {
  EXPECT_TRUE(cardReadsDuringRefresh(x3()));
  EXPECT_TRUE(cardReadsDuringRefresh(x4()));
  EXPECT_TRUE(cardReadsDuringRefresh(sdmmcS3()));
  EXPECT_FALSE(cardReadsDuringRefresh(t5s3()));
}

TEST(IndexingPopupGate, FirstOpenIndexesWhileThePopupPaintsOnEveryBoard) {
  EXPECT_EQ(firstOpenLend(x3()), Lend::DuringRefresh);
  EXPECT_EQ(firstOpenLend(x4()), Lend::DuringRefresh);
  EXPECT_EQ(firstOpenLend(sdmmcS3()), Lend::PsramArena);
  EXPECT_EQ(firstOpenLend(t5s3()), Lend::PsramArena);  // as since #792
}

TEST(IndexingPopupGate, FirstOpenBlocksInDarkModeAndWithoutASecondaryBuffer) {
  EXPECT_EQ(firstOpenLend(darkMode(x3())), Lend::Blocking);
  EXPECT_EQ(firstOpenLend(darkMode(x4())), Lend::Blocking);
  EXPECT_EQ(firstOpenLend(darkMode(sdmmcS3())), Lend::Blocking);
  Panel noSecondary = x3();
  noSecondary.hasSecondaryBuffer = false;
  EXPECT_EQ(firstOpenLend(noSecondary), Lend::Blocking);
  noSecondary = sdmmcS3();
  noSecondary.hasSecondaryBuffer = false;  // the arena is lent elsewhere
  EXPECT_EQ(firstOpenLend(noSecondary), Lend::Blocking);
}

// A C3 with its secondary away (released or lent): nothing to lend during the refresh.
TEST(IndexingPopupGate, NothingIsLentDuringTheRefreshWithoutASecondaryBuffer) {
  for (Panel p : {x3(), x4()}) {
    p.hasSecondaryBuffer = false;
    EXPECT_EQ(firstOpenLend(p), Lend::Blocking);
    EXPECT_EQ(chapterLend(p), Lend::Blocking);
  }
}

// A board whose card cannot be read during a refresh keeps the blocking popup, whatever its lend.
TEST(IndexingPopupGate, AFramebufferLendAloneDoesNotMakeTheCardReadable) {
  Panel p = x4();
  p.refreshLeavesBusToCard = false;
  EXPECT_FALSE(cardReadsDuringRefresh(p));
  EXPECT_EQ(chapterLend(p), Lend::Blocking);
  EXPECT_EQ(firstOpenLend(p), Lend::Blocking);
}

TEST(IndexingPopupGate, ChapterBuildStartsWhileThePopupPaintsWhereTheCardCanBeRead) {
  EXPECT_EQ(chapterLend(x3()), Lend::DuringRefresh);
  EXPECT_EQ(chapterLend(x4()), Lend::DuringRefresh);
  EXPECT_EQ(chapterLend(sdmmcS3()), Lend::PsramArena);
  EXPECT_EQ(chapterLend(t5s3()), Lend::Blocking);
}

TEST(IndexingPopupGate, ChapterBlocksInDarkModeAndWithTheArenaOut) {
  EXPECT_EQ(chapterLend(darkMode(x3())), Lend::Blocking);
  EXPECT_EQ(chapterLend(darkMode(x4())), Lend::Blocking);
  EXPECT_EQ(chapterLend(darkMode(sdmmcS3())), Lend::Blocking);
  Panel arenaOut = sdmmcS3();
  arenaOut.hasSecondaryBuffer = false;
  EXPECT_EQ(chapterLend(arenaOut), Lend::Blocking);
}

// The SectionBuilding pass's popup: async only where the loop task's slice runs through its refresh.
TEST(IndexingPopupGate, SectionBuildingPopupShipsAsyncWhereTheSliceRunsThroughIt) {
  EXPECT_TRUE(buildsThroughPopup(x4()));       // waited for off the bus, as the build page is
  EXPECT_TRUE(buildsThroughPopup(sdmmcS3()));  // the slice runs through any refresh
  EXPECT_FALSE(buildsThroughPopup(x3()));      // the finish waits with the bus held: the slice would wait
  EXPECT_FALSE(buildsThroughPopup(t5s3()));
  EXPECT_FALSE(buildsThroughPopup(darkMode(x4())));
  EXPECT_FALSE(buildsThroughPopup(darkMode(sdmmcS3())));
}

TEST(IndexingPopupGate, TxtAndMarkdownIndexWhileThePopupPaintsWhereTheCardCanBeRead) {
  EXPECT_TRUE(indexesThroughPopup(x3()));
  EXPECT_TRUE(indexesThroughPopup(x4()));
  EXPECT_TRUE(indexesThroughPopup(sdmmcS3()));
  EXPECT_FALSE(indexesThroughPopup(t5s3()));
  EXPECT_FALSE(indexesThroughPopup(darkMode(x3())));
  Panel single = x3();
  single.hasSecondaryBuffer = false;  // the popup's frame would be the one the page is drawn into
  EXPECT_FALSE(indexesThroughPopup(single));
}

TEST(IndexingPopupGate, NothingAsyncWhereTheDriverBlocks) {
  Panel p;  // a driver that finishes the refresh in the call that starts it
  p.cardOffPanelBus = true;
  p.decodesThroughRefresh = true;
  p.lendsPsramArena = true;
  p.hasSecondaryBuffer = true;
  EXPECT_EQ(firstOpenLend(p), Lend::Blocking);
  EXPECT_EQ(chapterLend(p), Lend::Blocking);
  EXPECT_FALSE(buildsThroughPopup(p));
  EXPECT_FALSE(indexesThroughPopup(p));
}

}  // namespace
