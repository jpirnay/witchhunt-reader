// Carousel cover slots: which book each of the three tiles shows, and where it sits.
//
// The bug this pins: the draw wrapped its neighbours and gated them on the book count, the tap
// recorder clamped at the ends and did not gate. So at centre index 0 -- where the home screen
// opens -- the left cover was painted showing the last book and tapping it did nothing, and a
// two-book carousel recorded a target for a tile it never drew.
//
// The tests assert the book each slot shows, and the clamp the recorder applies to a slot.

#include <gtest/gtest.h>

#include <string>

#include "components/themes/lyra/CarouselCoverLayout.h"

namespace {

// The real LyraCarousel tile sizes, restated so the expectations below are readable.
constexpr CarouselCoverLayout::Dimensions kDims{/*centreW=*/340, /*centreH=*/540, /*sideW=*/200,
                                                /*sideH=*/390,   /*overlap=*/60,  /*topPad=*/10};
constexpr int kScreenW = 480;
constexpr int kRectY = 56;  // LyraCarouselMetrics homeTopPadding

}  // namespace

TEST(CarouselCoverLayout, EmptyShelfHasNoSlots) {
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 0, 0);
  EXPECT_EQ(s.centre.bookIndex, -1);
  EXPECT_EQ(s.left.bookIndex, -1);
  EXPECT_EQ(s.right.bookIndex, -1);
}

TEST(CarouselCoverLayout, SingleBookHasOnlyTheCentre) {
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 0, 1);
  EXPECT_EQ(s.centre.bookIndex, 0);
  EXPECT_EQ(s.left.bookIndex, -1);
  EXPECT_EQ(s.right.bookIndex, -1);
}

// Two books: a right neighbour but no left one, or the same cover would appear on both sides.
// The recorder skips a slot whose bookIndex is -1, so this is also what keeps a phantom target out.
TEST(CarouselCoverLayout, TwoBooksShowOnlyTheRightNeighbourFromEitherEnd) {
  for (int centre = 0; centre < 2; ++centre) {
    const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, centre, 2);
    EXPECT_EQ(s.centre.bookIndex, centre);
    EXPECT_EQ(s.left.bookIndex, -1) << "centre=" << centre;
    EXPECT_EQ(s.right.bookIndex, 1 - centre) << "centre=" << centre;
  }
}

// The regression proper: at both ends of the shelf the wrapped neighbour is drawn, so it must be
// recorded. Index 0 is the position the home screen opens on.
TEST(CarouselCoverLayout, NeighboursWrapAtBothEnds) {
  constexpr int kBooks = 5;

  const auto first = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 0, kBooks);
  EXPECT_EQ(first.left.bookIndex, kBooks - 1);
  EXPECT_EQ(first.right.bookIndex, 1);

  const auto last = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, kBooks - 1, kBooks);
  EXPECT_EQ(last.left.bookIndex, kBooks - 2);
  EXPECT_EQ(last.right.bookIndex, 0);
}

// Walk the whole shelf: the neighbours wrap, and the three tiles never repeat a book.
TEST(CarouselCoverLayout, EveryPositionShowsThreeDistinctBooks) {
  for (int books = 3; books <= 12; ++books) {
    for (int centre = 0; centre < books; ++centre) {
      const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, centre, books);
      const std::string where = " books=" + std::to_string(books) + " centre=" + std::to_string(centre);

      EXPECT_EQ(s.centre.bookIndex, centre) << where;
      EXPECT_EQ(s.left.bookIndex, (centre + books - 1) % books) << where;
      EXPECT_EQ(s.right.bookIndex, (centre + 1) % books) << where;
      EXPECT_NE(s.left.bookIndex, centre) << where;
      EXPECT_NE(s.right.bookIndex, centre) << where;
      EXPECT_NE(s.left.bookIndex, s.right.bookIndex) << where;
    }
  }
}

// Side tiles are vertically centred on the taller centre tile, and the centre sits topPad below
// the rect the theme hands the cover strip.
TEST(CarouselCoverLayout, TilesSitWhereTheThemeDrawsThem) {
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 1, 4);
  EXPECT_EQ(s.centre.y, kRectY + kDims.topPad);
  EXPECT_EQ(s.centre.x, (kScreenW - kDims.centreW) / 2);
  EXPECT_EQ(s.left.y, s.centre.y + (kDims.centreH - kDims.sideH) / 2);
  EXPECT_EQ(s.right.y, s.left.y);
  // Each side tile slides exactly `overlap` px behind the centre.
  EXPECT_EQ(s.left.x + s.left.w, s.centre.x + kDims.overlap);
  EXPECT_EQ(s.right.x, s.centre.x + s.centre.w - kDims.overlap);
}

// --- Clipping to the screen ---------------------------------------------------
// The device bug this pins: the left tile's x is `centreX - sideW + overlap`, which on a 540 px
// portrait panel is -40. The tile is therefore DRAWN clipped, but the recorded tap target kept
// its full unclipped width -- so a press in the empty margin beside a partly-drawn cover opened
// a book that was not under the finger. Reported on a LilyGo T5S3.

TEST(CarouselCoverLayout, ClampingRemovesTheOffScreenPart) {
  constexpr int narrowW = 540;
  constexpr int screenH = 960;
  const auto s = CarouselCoverLayout::compute(kDims, narrowW, kRectY, 1, 4);
  ASSERT_LT(s.left.x, 0) << "the left tile starts off-screen on this panel";
  const auto clipped = CarouselCoverLayout::clampToScreen(s.left, narrowW, screenH);

  EXPECT_EQ(clipped.x, 0);
  // The visible width is what was on-screen: -40 + 200 = 160.
  EXPECT_EQ(clipped.w, s.left.x + s.left.w);
  EXPECT_EQ(clipped.bookIndex, s.left.bookIndex) << "clipping must not change which book it is";
  // Vertical extent is untouched here: the tile fits.
  EXPECT_EQ(clipped.y, s.left.y);
  EXPECT_EQ(clipped.h, s.left.h);
}

TEST(CarouselCoverLayout, ClampingIsIdentityForAnOnScreenTile) {
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 1, 4);
  const auto clipped = CarouselCoverLayout::clampToScreen(s.centre, kScreenW, 800);
  EXPECT_EQ(clipped.x, s.centre.x);
  EXPECT_EQ(clipped.y, s.centre.y);
  EXPECT_EQ(clipped.w, s.centre.w);
  EXPECT_EQ(clipped.h, s.centre.h);
}

TEST(CarouselCoverLayout, ATileEntirelyOffScreenClampsToNothing) {
  // w/h 0 is the signal the recorder treats as "not there", so a slot that is completely
  // off-screen can never be pressed.
  const CarouselCoverLayout::Slot offLeft{/*bookIndex=*/3, /*x=*/-300, /*y=*/100, /*w=*/200, /*h=*/390};
  const auto clipped = CarouselCoverLayout::clampToScreen(offLeft, 540, 960);
  EXPECT_EQ(clipped.w, 0);
}

TEST(CarouselCoverLayout, ClampingAlsoTrimsTheBottom) {
  // The tall centre tile overruns a short panel; the part below the screen must not be tappable
  // either, for the same reason as the left margin.
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 1, 4);
  constexpr int shortH = 400;
  const auto clipped = CarouselCoverLayout::clampToScreen(s.centre, kScreenW, shortH);
  EXPECT_EQ(clipped.y, s.centre.y);
  EXPECT_EQ(clipped.y + clipped.h, shortH);
}
