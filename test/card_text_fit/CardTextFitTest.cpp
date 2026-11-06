// How the home card's text block gives way when it does not fit its tile.
//
// The bug this pins (#375): title, author and series were each wrapped to their own cap and the
// sum was never compared with the tile. A long title, a list of co-authors and a long series
// name made a block taller than the tile, and centring it put the title above the selection
// highlight and the status line below it.
//
// The numbers are Lyra's at the Normal UI font step: a 30 px bold title, 25 px author / series /
// status rows, the 23 px non-scaling face for the history, and 12 / 4 px gaps.

#include <gtest/gtest.h>

#include "components/themes/CardTextFit.h"

namespace {

using CardTextFit::Author;
using CardTextFit::History;
using CardTextFit::Series;
using CardTextFit::Status;
using CardTextFit::Title;

constexpr int kTitleH = 30;
constexpr int kBodyH = 25;
constexpr int kHistoryH = 23;
constexpr int kGap = 12;
constexpr int kTightGap = 4;

// A Lyra card: one gap above the author/series group, the status and the history. The series
// carries no gap of its own while an author sits above it.
CardTextFit::Parts lyraParts(int title, int author, int series, int status, int history) {
  CardTextFit::Parts parts{};
  parts[Title] = {kTitleH, title, 0};
  parts[Author] = {kBodyH, author, kGap};
  parts[Series] = {kBodyH, series, author > 0 ? 0 : kGap};
  parts[Status] = {kBodyH, status, kGap};
  parts[History] = {kHistoryH, history, kGap};
  return parts;
}

CardTextFit::Lines natural(const CardTextFit::Parts& parts) {
  CardTextFit::Lines lines{};
  for (size_t i = 0; i < parts.size(); i++) lines[i] = parts[i].lines;
  return lines;
}

TEST(CardTextFit, BlockThatFitsIsLeftExactlyAsItWas) {
  const auto parts = lyraParts(1, 1, 0, 1, 2);
  const auto result = CardTextFit::fit(parts, 226, kTightGap);

  EXPECT_TRUE(result.fits);
  EXPECT_FALSE(result.tight);
  EXPECT_EQ(result.lines, natural(parts));
}

TEST(CardTextFit, HeightCountsGapsOnlyBetweenDrawnParts) {
  // No author, series or history: title, gap, status. The series' gap is not owed because the
  // series is not drawn, and the first part drawn never carries one.
  const auto parts = lyraParts(2, 0, 0, 1, 0);
  EXPECT_EQ(CardTextFit::height(parts, natural(parts), false, kTightGap), 2 * kTitleH + kGap + kBodyH);
  EXPECT_EQ(CardTextFit::height(parts, natural(parts), true, kTightGap), 2 * kTitleH + kTightGap + kBodyH);
}

TEST(CardTextFit, TopsFollowTheSameWalkAsTheHeight) {
  // Drawn from the result, so what is painted is what was measured: a series under an author
  // follows it with no gap, an absent part takes no room, and the block ends where the height
  // says it does.
  const auto parts = lyraParts(2, 1, 1, 1, 0);
  const auto result = CardTextFit::fit(parts, 500, kTightGap);

  EXPECT_EQ(result.tops[Title], 0);
  EXPECT_EQ(result.tops[Author], 2 * kTitleH + kGap);
  EXPECT_EQ(result.tops[Series], result.tops[Author] + kBodyH);
  EXPECT_EQ(result.tops[Status], result.tops[Series] + kBodyH + kGap);
  EXPECT_EQ(result.height, result.tops[Status] + kBodyH);
  EXPECT_EQ(result.height, CardTextFit::height(parts, result.lines, result.tight, kTightGap));
}

TEST(CardTextFit, TopsUseTheTightGapsTheFitSettledOn) {
  const auto parts = lyraParts(3, 2, 2, 1, 0);
  const auto result = CardTextFit::fit(parts, 226, kTightGap);

  ASSERT_TRUE(result.tight);
  EXPECT_EQ(result.tops[Author], 3 * kTitleH + kTightGap);
  EXPECT_EQ(result.height, 223);
}

TEST(CardTextFit, GapsShrinkBeforeAnyContentIsShed) {
  // 3 + 2 + 2 + status: 239 px with roomy gaps, 223 with tight ones, in a 226 px tile.
  const auto parts = lyraParts(3, 2, 2, 1, 0);
  const auto result = CardTextFit::fit(parts, 226, kTightGap);

  EXPECT_TRUE(result.fits);
  EXPECT_TRUE(result.tight);
  EXPECT_EQ(result.lines, natural(parts));
}

TEST(CardTextFit, Issue375SeriesGivesWayAndTitleAndAuthorStayWhole) {
  // The reported card: a three-line title, two lines of co-authors, a two-line series and the
  // status. The seven-entry menu in the report needs 7 * 64 + 6 * 8 = 496 px of the 760 px
  // content height; computeHomeScreenLayout takes the gap down to 4 and the 242 px tile down to
  // 204, which leaves 188 inside the selection padding.
  const auto parts = lyraParts(3, 2, 2, 1, 0);
  constexpr int kAvailable = 188;
  ASSERT_GT(CardTextFit::height(parts, natural(parts), true, kTightGap), kAvailable);

  const auto result = CardTextFit::fit(parts, kAvailable, kTightGap);

  EXPECT_TRUE(result.fits);
  EXPECT_EQ(result.lines[Series], 0);
  EXPECT_EQ(result.lines[Title], 3);
  EXPECT_EQ(result.lines[Author], 2);
  EXPECT_EQ(result.lines[Status], 1);
  EXPECT_LE(CardTextFit::height(parts, result.lines, result.tight, kTightGap), kAvailable);
}

TEST(CardTextFit, HistoryGivesWayFirstThroughItsOneLineForm) {
  // Over by one history row: the history drops to one line (the caller's compact form) and
  // nothing else moves.
  const auto parts = lyraParts(1, 1, 1, 1, 2);
  const int full = CardTextFit::height(parts, natural(parts), true, kTightGap);
  const auto result = CardTextFit::fit(parts, full - 1, kTightGap);

  EXPECT_TRUE(result.fits);
  EXPECT_EQ(result.lines[History], 1);
  EXPECT_EQ(result.lines[Series], 1);
  EXPECT_EQ(result.lines[Author], 1);
  EXPECT_EQ(result.lines[Title], 1);
}

TEST(CardTextFit, SeriesGoesBeforeAuthorAndAuthorBeforeTitle) {
  const auto parts = lyraParts(3, 2, 2, 1, 2);

  // Room for everything but one author row: history and series are gone, the title is whole.
  const int authorShort = 3 * kTitleH + kTightGap + kBodyH + kTightGap + kBodyH;
  const auto a = CardTextFit::fit(parts, authorShort, kTightGap);
  EXPECT_TRUE(a.fits);
  EXPECT_EQ(a.lines[History], 0);
  EXPECT_EQ(a.lines[Series], 0);
  EXPECT_EQ(a.lines[Author], 1);
  EXPECT_EQ(a.lines[Title], 3);

  // One title row less again: the title is the last to give way.
  const auto b = CardTextFit::fit(parts, authorShort - 1, kTightGap);
  EXPECT_TRUE(b.fits);
  EXPECT_EQ(b.lines[Author], 1);
  EXPECT_EQ(b.lines[Title], 2);
}

TEST(CardTextFit, EachStepTriesRoomyGapsBeforeTightOnes) {
  // Dropping the series frees enough that the roomy gaps come back.
  const auto parts = lyraParts(1, 1, 2, 1, 0);
  const int roomyWithoutSeries = kTitleH + kGap + kBodyH + kGap + kBodyH;
  const auto result = CardTextFit::fit(parts, roomyWithoutSeries, kTightGap);

  EXPECT_TRUE(result.fits);
  EXPECT_FALSE(result.tight);
  EXPECT_EQ(result.lines[Series], 0);
}

TEST(CardTextFit, AFourLineTitleGivesUpOneLineAtATime) {
  // A title in a smaller face can run to four lines. Three still fit here: 90 + 4 + 25 + 4 + 25.
  const auto result = CardTextFit::fit(lyraParts(4, 1, 0, 1, 0), 150, kTightGap);
  EXPECT_TRUE(result.fits);
  EXPECT_EQ(result.lines[Title], 3);
}

TEST(CardTextFit, StatusIsNeverShed) {
  const auto parts = lyraParts(3, 2, 2, 1, 2);
  const auto result = CardTextFit::fit(parts, 1, kTightGap);

  // Nothing fits one pixel; the leanest block is returned and the caller anchors it.
  EXPECT_FALSE(result.fits);
  EXPECT_TRUE(result.tight);
  EXPECT_EQ(result.lines[Status], 1);
  EXPECT_EQ(result.lines[Title], 1);
  EXPECT_EQ(result.lines[Author], 1);
  EXPECT_EQ(result.lines[Series], 0);
  EXPECT_EQ(result.lines[History], 0);
}

TEST(CardTextFit, StepsNeverGrowAPart) {
  // A one-line title stays one line through the "title to two lines" step.
  const auto parts = lyraParts(1, 0, 0, 1, 0);
  const auto result = CardTextFit::fit(parts, 1, kTightGap);
  EXPECT_EQ(result.lines[Title], 1);
}

TEST(CardTextFit, LargeFontAtTheSmallestTileStillShowsTitleAuthorAndStatus) {
  // Lyra's floor: a 170 px tile, 154 px inside the selection padding, at the Large step
  // (35 / 30 px rows, 15 px gaps).
  CardTextFit::Parts parts{};
  parts[Title] = {35, 3, 0};
  parts[Author] = {30, 2, 15};
  parts[Series] = {30, 2, 0};
  parts[Status] = {30, 1, 15};
  parts[History] = {kHistoryH, 2, 15};
  const auto result = CardTextFit::fit(parts, 154, kTightGap);

  EXPECT_TRUE(result.fits);
  EXPECT_GE(result.lines[Title], 1);
  EXPECT_GE(result.lines[Author], 1);
  EXPECT_EQ(result.lines[Status], 1);
}

}  // namespace
