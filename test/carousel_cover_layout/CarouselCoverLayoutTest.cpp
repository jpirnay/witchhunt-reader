// Carousel cover slots: which book each of the three tiles shows, and where it sits.
//
// The bug this pins: the draw wrapped its neighbours and gated them on the book count, the tap
// recorder clamped at the ends and did not gate. So at centre index 0 -- where the home screen
// opens -- the left cover was painted showing the last book and tapping it did nothing, and a
// two-book carousel recorded a target for a tile it never drew.
//
// Every test round-trips: compute the slots, then hit-test a pixel inside the tile that was
// computed, and assert the book that comes back is the book that was drawn there.

#include <gtest/gtest.h>

#include "components/themes/lyra/CarouselCoverLayout.h"

namespace {

// The real LyraCarousel tile sizes, restated so the expectations below are readable.
constexpr CarouselCoverLayout::Dimensions kDims{/*centreW=*/340, /*centreH=*/540, /*sideW=*/200,
                                                /*sideH=*/390,   /*overlap=*/60,  /*topPad=*/10};
constexpr int kScreenW = 480;
constexpr int kRectY = 56;  // LyraCarouselMetrics homeTopPadding

// A point inside a slot, away from its edges.
int midX(const CarouselCoverLayout::Slot& s) { return s.x + s.w / 2; }
int midY(const CarouselCoverLayout::Slot& s) { return s.y + s.h / 2; }

// The strip of a side tile the centre does NOT cover -- what the reader can actually see and aim
// at. The left tile's visible part is its left edge up to the centre's left edge.
int visibleLeftX(const CarouselCoverLayout::Slots& s) { return s.left.x + (s.left.w - kDims.overlap) / 2; }
int visibleRightX(const CarouselCoverLayout::Slots& s) {
  return s.right.x + kDims.overlap + (s.right.w - kDims.overlap) / 2;
}

}  // namespace

TEST(CarouselCoverLayout, EmptyShelfHasNoSlots) {
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 0, 0);
  EXPECT_EQ(s.centre.bookIndex, -1);
  EXPECT_EQ(s.left.bookIndex, -1);
  EXPECT_EQ(s.right.bookIndex, -1);
  EXPECT_EQ(CarouselCoverLayout::hitTest(s, kScreenW / 2, kRectY + 100), -1);
}

TEST(CarouselCoverLayout, SingleBookHasOnlyTheCentre) {
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 0, 1);
  EXPECT_EQ(s.centre.bookIndex, 0);
  EXPECT_EQ(s.left.bookIndex, -1);
  EXPECT_EQ(s.right.bookIndex, -1);
  EXPECT_EQ(CarouselCoverLayout::hitTest(s, midX(s.centre), midY(s.centre)), 0);
  // The side geometry still exists but must never answer a tap.
  EXPECT_EQ(CarouselCoverLayout::hitTest(s, visibleLeftX(s), midY(s.left)), -1);
  EXPECT_EQ(CarouselCoverLayout::hitTest(s, visibleRightX(s), midY(s.right)), -1);
}

// Two books: a right neighbour but no left one, or the same cover would appear on both sides.
TEST(CarouselCoverLayout, TwoBooksShowOnlyTheRightNeighbourFromEitherEnd) {
  for (int centre = 0; centre < 2; ++centre) {
    const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, centre, 2);
    EXPECT_EQ(s.centre.bookIndex, centre);
    EXPECT_EQ(s.left.bookIndex, -1) << "centre=" << centre;
    EXPECT_EQ(s.right.bookIndex, 1 - centre) << "centre=" << centre;

    // The regression: the recorder used to publish a LEFT target here for a tile that is not
    // drawn, so a tap on empty canvas selected a book.
    EXPECT_EQ(CarouselCoverLayout::hitTest(s, visibleLeftX(s), midY(s.left)), -1) << "centre=" << centre;
    EXPECT_EQ(CarouselCoverLayout::hitTest(s, visibleRightX(s), midY(s.right)), 1 - centre) << "centre=" << centre;
  }
}

// The regression proper: at both ends of the shelf the wrapped neighbour is drawn, so it must be
// tappable. Index 0 is the position the home screen opens on.
TEST(CarouselCoverLayout, NeighboursWrapAtBothEnds) {
  constexpr int kBooks = 5;

  const auto first = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 0, kBooks);
  EXPECT_EQ(first.left.bookIndex, kBooks - 1);
  EXPECT_EQ(first.right.bookIndex, 1);
  EXPECT_EQ(CarouselCoverLayout::hitTest(first, visibleLeftX(first), midY(first.left)), kBooks - 1);

  const auto last = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, kBooks - 1, kBooks);
  EXPECT_EQ(last.left.bookIndex, kBooks - 2);
  EXPECT_EQ(last.right.bookIndex, 0);
  EXPECT_EQ(CarouselCoverLayout::hitTest(last, visibleRightX(last), midY(last.right)), 0);
}

// Walk the whole shelf and assert the round trip at every position: each visible tile hit-tests
// back to the book that tile shows.
TEST(CarouselCoverLayout, EveryPositionRoundTrips) {
  for (int books = 3; books <= 12; ++books) {
    for (int centre = 0; centre < books; ++centre) {
      const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, centre, books);
      const std::string where = " books=" + std::to_string(books) + " centre=" + std::to_string(centre);

      EXPECT_EQ(CarouselCoverLayout::hitTest(s, midX(s.centre), midY(s.centre)), centre) << where;
      EXPECT_EQ(CarouselCoverLayout::hitTest(s, visibleLeftX(s), midY(s.left)), s.left.bookIndex) << where;
      EXPECT_EQ(CarouselCoverLayout::hitTest(s, visibleRightX(s), midY(s.right)), s.right.bookIndex) << where;

      // Every slot shows a different book: the three tiles must never repeat one.
      EXPECT_NE(s.left.bookIndex, centre) << where;
      EXPECT_NE(s.right.bookIndex, centre) << where;
      EXPECT_NE(s.left.bookIndex, s.right.bookIndex) << where;
    }
  }
}

// The centre is drawn last and so covers the inner `overlap` px of each neighbour. A tap there
// must reach the centre, not the tile hidden underneath it.
TEST(CarouselCoverLayout, CentreWinsTheOverlappingStrip) {
  const auto s = CarouselCoverLayout::compute(kDims, kScreenW, kRectY, 2, 5);
  ASSERT_GE(s.left.bookIndex, 0);
  ASSERT_GE(s.right.bookIndex, 0);

  // One pixel inside the centre, still within the left tile's extent.
  const int overlappedLeft = s.centre.x + 1;
  ASSERT_LT(overlappedLeft, s.left.x + s.left.w);
  EXPECT_EQ(CarouselCoverLayout::hitTest(s, overlappedLeft, midY(s.left)), 2);

  const int overlappedRight = s.centre.x + s.centre.w - 1;
  ASSERT_GE(overlappedRight, s.right.x);
  EXPECT_EQ(CarouselCoverLayout::hitTest(s, overlappedRight, midY(s.right)), 2);
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
