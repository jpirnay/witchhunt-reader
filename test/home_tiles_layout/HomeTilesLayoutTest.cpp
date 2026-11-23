// Lyra Tiles' Home geometry on every panel it is offered on, upright and sideways, with each number
// of tiles it can show.
//
// What it guards: the card, the Also reading row, the tiles and the bottom row stay on screen and
// clear of each other, every target stays at least as tall as Lyra's menu rows were easy to hit
// (60 px), and the cover keeps the thumbnail's 6:10 shape, which the cover pass writes to exactly.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "components/themes/lyra/HomeTilesLayout.h"

namespace {

using HomeTilesLayout::Box;

// The content above the button hints, as UITheme::getContentRect gives it, and the body's top
// (LyraTilesMetrics::homeTopPadding).
struct Panel {
  const char* name;
  Box content;
};
constexpr int kTop = 40;
const Panel kPanels[] = {
    {"X4 upright", {0, 0, 480, 760}},
    {"X3 upright", {0, 0, 528, 752}},
    {"T5S3 upright", {0, 0, 540, 920}},
    {"X4 sideways", {0, 0, 800, 440}},
    {"X3 sideways", {0, 0, 792, 488}},
    {"T5S3 sideways", {0, 0, 960, 500}},
    {"X4 sideways, side hints", {30, 0, 740, 480}},
};

bool inside(const Box& inner, const Box& outer) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

bool overlap(const Box& a, const Box& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

std::string describe(const Box& b) {
  return "{" + std::to_string(b.x) + "," + std::to_string(b.y) + " " + std::to_string(b.w) + "x" + std::to_string(b.h) +
         "}";
}

}  // namespace

TEST(HomeTilesLayout, EveryPartIsOnScreenApartAndBigEnoughToHit) {
  for (const Panel& panel : kPanels) {
    for (int tiles = 0; tiles <= HomeTilesLayout::kMaxTiles; ++tiles) {
      for (int bottom = 1; bottom <= 2; ++bottom) {
        SCOPED_TRACE(std::string(panel.name) + ", " + std::to_string(tiles) + " tiles, " + std::to_string(bottom) +
                     " in the bottom row");
        const auto regions = HomeTilesLayout::regions(panel.content, kTop, tiles);
        const auto books = HomeTilesLayout::books(regions.books);
        const auto menu = HomeTilesLayout::menu(regions.menu, tiles, bottom);
        const Box screen{panel.content.x, kTop, panel.content.w, panel.content.h - kTop};

        std::vector<Box> targets{books.card};
        if (books.hasStrip) {
          for (const Box& slot : books.slots) targets.push_back(slot);
        }
        for (int i = 0; i < menu.tileCount; ++i) targets.push_back(menu.tiles[i]);
        for (int i = 0; i < menu.bottomCount; ++i) targets.push_back(menu.bottom[i]);

        for (size_t i = 0; i < targets.size(); ++i) {
          EXPECT_TRUE(inside(targets[i], screen)) << describe(targets[i]);
          EXPECT_GE(targets[i].h, 60) << describe(targets[i]);
          EXPECT_GE(targets[i].w, 60) << describe(targets[i]);
          for (size_t j = i + 1; j < targets.size(); ++j) {
            EXPECT_FALSE(overlap(targets[i], targets[j])) << describe(targets[i]) << " / " << describe(targets[j]);
          }
        }
        if (books.hasStrip) {
          EXPECT_FALSE(overlap(books.card, books.strip));
          EXPECT_TRUE(inside(books.strip, regions.books));
        }
      }
    }
  }
}

TEST(HomeTilesLayout, TheCardHoldsItsCoverTextAndFootApart) {
  for (const Panel& panel : kPanels) {
    for (int tiles = 0; tiles <= HomeTilesLayout::kMaxTiles; ++tiles) {
      SCOPED_TRACE(std::string(panel.name) + ", " + std::to_string(tiles) + " tiles");
      const auto books = HomeTilesLayout::books(HomeTilesLayout::regions(panel.content, kTop, tiles).books);
      EXPECT_TRUE(inside(books.cover, books.card)) << describe(books.cover);
      EXPECT_TRUE(inside(books.text, books.card)) << describe(books.text);
      EXPECT_TRUE(inside(books.foot, books.card)) << describe(books.foot);
      EXPECT_FALSE(overlap(books.cover, books.text));
      EXPECT_FALSE(overlap(books.cover, books.foot));
      EXPECT_FALSE(overlap(books.text, books.foot));
      // The text column must hold a title: at least a third of the card's width.
      EXPECT_GE(books.text.w * 3, books.card.w) << describe(books.text);
      // 6:10, to the pixel the integer division allows.
      EXPECT_NEAR(books.cover.w * 5 / 3, books.cover.h, 2) << describe(books.cover);
    }
  }
}

// The X4 upright with four tiles, as the proposal drew it: a 372 px card with a 178x298 cover,
// two rows of 218x122 tiles, and no room for the Also reading row.
TEST(HomeTilesLayout, X4UprightWithFourTiles) {
  const auto regions = HomeTilesLayout::regions({0, 0, 480, 760}, kTop, 4);
  const auto books = HomeTilesLayout::books(regions.books);
  const auto menu = HomeTilesLayout::menu(regions.menu, 4, 2);
  EXPECT_FALSE(books.hasStrip);
  EXPECT_EQ(books.card.h, 372);
  EXPECT_EQ(books.cover.w, 178);
  EXPECT_EQ(books.cover.h, 298);
  EXPECT_EQ(menu.tiles[0].w, 218);
  EXPECT_EQ(menu.tiles[0].h, 122);
  EXPECT_EQ(menu.bottom[0].h, HomeTilesLayout::kBottomRowHeight);
  // The bottom row ends kBottomPadding above the hints.
  EXPECT_EQ(menu.bottom[1].y + menu.bottom[1].h, 760 - HomeTilesLayout::kBottomPadding);
}

// The room is a matter of height: the tall T5S3 has it with four tiles, the X4 once two tiles
// share a single row.
TEST(HomeTilesLayout, AlsoReadingAppearsOnlyWhereTheCardCanSpareIt) {
  EXPECT_TRUE(HomeTilesLayout::books(HomeTilesLayout::regions({0, 0, 540, 920}, kTop, 4).books).hasStrip);
  EXPECT_FALSE(HomeTilesLayout::books(HomeTilesLayout::regions({0, 0, 480, 760}, kTop, 3).books).hasStrip);
  EXPECT_TRUE(HomeTilesLayout::books(HomeTilesLayout::regions({0, 0, 480, 760}, kTop, 2).books).hasStrip);
  for (const Panel& panel : kPanels) {
    for (int tiles = 0; tiles <= HomeTilesLayout::kMaxTiles; ++tiles) {
      const auto books = HomeTilesLayout::books(HomeTilesLayout::regions(panel.content, kTop, tiles).books);
      if (books.hasStrip) {
        EXPECT_GE(books.card.h, HomeTilesLayout::kMinCardHeight) << panel.name;
      }
    }
  }
}

// An odd count gives the first tile, Library, the whole row rather than leaving a hole.
TEST(HomeTilesLayout, AnOddFirstTileSpansTheRow) {
  const auto regions = HomeTilesLayout::regions({0, 0, 480, 760}, kTop, 3);
  const auto menu = HomeTilesLayout::menu(regions.menu, 3, 1);
  EXPECT_EQ(menu.tiles[0].w, regions.menu.w);
  EXPECT_EQ(menu.tiles[1].y, menu.tiles[2].y);
  EXPECT_LT(menu.tiles[1].x, menu.tiles[2].x);
  EXPECT_GT(menu.tiles[1].y, menu.tiles[0].y);
  // Settings alone takes the whole bottom row.
  EXPECT_EQ(menu.bottom[0].w, regions.menu.w);
}

// More is the left half, Settings the right: the order the entries come in.
TEST(HomeTilesLayout, MoreSitsLeftOfSettings) {
  const auto regions = HomeTilesLayout::regions({0, 0, 480, 760}, kTop, 4);
  const auto menu = HomeTilesLayout::menu(regions.menu, 4, 2);
  EXPECT_EQ(menu.bottom[0].y, menu.bottom[1].y);
  EXPECT_LT(menu.bottom[0].x + menu.bottom[0].w, menu.bottom[1].x);
  EXPECT_EQ(menu.bottom[1].x + menu.bottom[1].w, regions.menu.x + regions.menu.w);
}

// Sideways the card takes the left column and the tiles the right one, both the full height.
TEST(HomeTilesLayout, SidewaysTheCardAndTheTilesShareTheWidth) {
  const auto regions = HomeTilesLayout::regions({0, 0, 800, 440}, kTop, 4);
  EXPECT_LT(regions.books.x + regions.books.w, regions.menu.x);
  EXPECT_EQ(regions.books.y, regions.menu.y);
  EXPECT_EQ(regions.books.h, regions.menu.h);
  const auto books = HomeTilesLayout::books(regions.books);
  // A wide card keeps the cover to its share of the width, and the 6:10 shape with it.
  EXPECT_LE(books.cover.w * 20, (books.card.w - 2 * HomeTilesLayout::kCardPadding) * 9);
}
