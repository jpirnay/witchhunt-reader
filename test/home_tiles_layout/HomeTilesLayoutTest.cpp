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

// The books' part of Home as the theme lays it out: the card, and the Also reading row where the
// panel has room for it beside a full grid.
HomeTilesLayout::Books booksFor(const Box& content, const int tiles) {
  return HomeTilesLayout::books(HomeTilesLayout::regions(content, kTop, tiles).books,
                                HomeTilesLayout::stripFits(content, kTop));
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
        const auto books = booksFor(panel.content, tiles);
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
      const auto books = booksFor(panel.content, tiles);
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
  const auto books = booksFor({0, 0, 480, 760}, 4);
  const auto menu = HomeTilesLayout::menu(regions.menu, 4, 2);
  EXPECT_FALSE(books.hasStrip);
  EXPECT_EQ(books.card.h, 372);
  EXPECT_EQ(books.cover.w, 178);
  EXPECT_EQ(books.cover.h, 298);
  EXPECT_EQ(menu.tiles[0].w, 218);
  EXPECT_EQ(menu.tiles[0].h, 122);
  EXPECT_EQ(menu.bottom[0].h, HomeTilesLayout::kRegular.bottomRowHeight);
  // The bottom row ends kBottomPadding above the hints.
  EXPECT_EQ(menu.bottom[1].y + menu.bottom[1].h, 760 - HomeTilesLayout::kRegular.bottomPadding);
}

// The row is the panel's: it needs room beside two full rows of tiles and the bottom row, so the
// tall T5S3 has it and the X4 and X3 do not, whatever the number of entries. Fewer tiles give the
// card the room instead.
TEST(HomeTilesLayout, AlsoReadingAppearsOnlyBesideAFullGrid) {
  for (int tiles = 0; tiles <= HomeTilesLayout::kMaxTiles; ++tiles) {
    SCOPED_TRACE(std::to_string(tiles) + " tiles");
    EXPECT_TRUE(booksFor({0, 0, 540, 920}, tiles).hasStrip);
    EXPECT_FALSE(booksFor({0, 0, 480, 760}, tiles).hasStrip);
    EXPECT_FALSE(booksFor({0, 0, 528, 752}, tiles).hasStrip);
  }
  EXPECT_GT(booksFor({0, 0, 480, 760}, 2).card.h, booksFor({0, 0, 480, 760}, 4).card.h);
  for (const Panel& panel : kPanels) {
    for (int tiles = 0; tiles <= HomeTilesLayout::kMaxTiles; ++tiles) {
      const auto books = booksFor(panel.content, tiles);
      if (books.hasStrip) {
        EXPECT_GE(books.card.h, HomeTilesLayout::kRegular.minCardHeight) << panel.name;
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
  const auto books = booksFor({0, 0, 800, 440}, 4);
  // A wide card keeps the cover to its share of the width, and the 6:10 shape with it.
  EXPECT_LE(books.cover.w * 20, (books.card.w - 2 * HomeTilesLayout::kRegular.cardPadding) * 9);
}

// The row's label gets a whole line of the small face (23 px at the Normal UI size) and a gap above
// the covers, which keep the 54x90 box their thumbnails are made at.
TEST(HomeTilesLayout, TheAlsoReadingLabelClearsTheCovers) {
  const auto books = booksFor({0, 0, 540, 920}, 4);
  ASSERT_TRUE(books.hasStrip);
  EXPECT_GE(books.slots[0].y - books.strip.y, 23 + 4);
  EXPECT_EQ(books.slotCovers[0].w, 54);
  EXPECT_EQ(books.slotCovers[0].h, 90);
}

// The compact panel (Murphy, 240 x 416, hints 30 px, the body under a 28 px status line) with its
// own sizes: everything on screen and apart, nothing under its 36 px menu rows, a text column wide
// enough for a title, and no Also reading row.
TEST(HomeTilesLayout, TheCompactPanelFitsTheCardAndTwoRowsOfTiles) {
  constexpr int kCompactTop = 28;
  const Box murphyPanels[] = {{0, 0, 240, 386}, {0, 0, 416, 210}};
  for (const Box& content : murphyPanels) {
    for (int tiles = 0; tiles <= HomeTilesLayout::kMaxTiles; ++tiles) {
      for (int bottom = 1; bottom <= 2; ++bottom) {
        SCOPED_TRACE(describe(content) + ", " + std::to_string(tiles) + " tiles, " + std::to_string(bottom));
        const auto& d = HomeTilesLayout::kCompact;
        const auto regions = HomeTilesLayout::regions(content, kCompactTop, tiles, d);
        const auto books =
            HomeTilesLayout::books(regions.books, HomeTilesLayout::stripFits(content, kCompactTop, d), d);
        const auto menu = HomeTilesLayout::menu(regions.menu, tiles, bottom, d);
        const Box screen{content.x, kCompactTop, content.w, content.h - kCompactTop};
        EXPECT_FALSE(books.hasStrip);
        std::vector<Box> targets{books.card};
        for (int i = 0; i < menu.tileCount; ++i) targets.push_back(menu.tiles[i]);
        for (int i = 0; i < menu.bottomCount; ++i) targets.push_back(menu.bottom[i]);
        for (size_t i = 0; i < targets.size(); ++i) {
          EXPECT_TRUE(inside(targets[i], screen)) << describe(targets[i]);
          EXPECT_GE(targets[i].h, 36) << describe(targets[i]);
          for (size_t j = i + 1; j < targets.size(); ++j) {
            EXPECT_FALSE(overlap(targets[i], targets[j])) << describe(targets[i]) << " / " << describe(targets[j]);
          }
        }
        EXPECT_TRUE(inside(books.cover, books.card));
        EXPECT_FALSE(overlap(books.cover, books.text));
        EXPECT_GE(books.text.w, 100) << describe(books.text);
        EXPECT_GE(books.cover.h, 100) << describe(books.cover);
      }
    }
  }
}

// --- Moving by rows and columns (four direction keys) -------------------------------------------

namespace {
using HomeGridNav::Move;

int go(const HomeGridNav::Grid& grid, const int from, const Move move) {
  return HomeGridNav::step(grid, from, move).index;
}
}  // namespace

// The X3's Home: the card, Library and File Transfer, More and Settings. Selector positions: card 0,
// tiles 1-2, More 3, Settings 4.
TEST(HomeGridNav, UpAndDownGoBetweenRowsLeftAndRightAlongOne) {
  const auto grid = HomeTilesLayout::grid(/*shownBooks=*/1, /*tiles=*/2, /*bottom=*/2);
  ASSERT_EQ(grid.rowCount, 3);
  EXPECT_EQ(go(grid, 0, Move::Down), 1);  // card -> the first tile
  EXPECT_EQ(go(grid, 1, Move::Right), 2);
  EXPECT_EQ(go(grid, 2, Move::Right), 1);  // along a row it wraps
  EXPECT_EQ(go(grid, 2, Move::Down), 4);   // the same side: File Transfer -> Settings
  EXPECT_EQ(go(grid, 1, Move::Down), 3);   // Library -> More
  EXPECT_EQ(go(grid, 4, Move::Down), 0);   // past the bottom row, back to the card
  EXPECT_EQ(go(grid, 0, Move::Up), 3);     // and the other way round
  EXPECT_EQ(go(grid, 3, Move::Up), 1);
}

TEST(HomeGridNav, LeftAndRightOnTheCardCycleTheBooks) {
  const auto grid = HomeTilesLayout::grid(1, 4, 2);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Right).cycle, 1);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Left).cycle, -1);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Right).index, 0);  // the selection stays on the card
  EXPECT_EQ(HomeGridNav::step(grid, 1, Move::Right).cycle, 0);  // a tile moves, it does not cycle
}

// The T5S3: the card, three Also reading books (1-3), four tiles (4-7), More 8, Settings 9.
TEST(HomeGridNav, TheAlsoReadingRowIsARowOfThree) {
  const auto grid = HomeTilesLayout::grid(4, 4, 2);
  ASSERT_EQ(grid.rowCount, 5);
  EXPECT_EQ(go(grid, 0, Move::Down), 1);
  EXPECT_EQ(go(grid, 1, Move::Right), 2);
  EXPECT_EQ(go(grid, 3, Move::Right), 1);
  EXPECT_EQ(go(grid, 3, Move::Down), 5);  // the right-hand book -> the right-hand tile
  EXPECT_EQ(go(grid, 1, Move::Down), 4);
  EXPECT_EQ(go(grid, 4, Move::Up), 1);
  EXPECT_EQ(go(grid, 5, Move::Up), 3);
  EXPECT_EQ(go(grid, 6, Move::Down), 8);
  EXPECT_EQ(go(grid, 7, Move::Down), 9);
}

// Three tiles: Library spans its row, so it is a row of one.
TEST(HomeGridNav, ASpanningTileIsARowOfOne) {
  const auto grid = HomeTilesLayout::grid(1, 3, 1);
  ASSERT_EQ(grid.rowCount, 4);
  EXPECT_EQ(go(grid, 0, Move::Down), 1);
  EXPECT_EQ(go(grid, 1, Move::Right), 1);  // nothing beside it
  EXPECT_EQ(go(grid, 1, Move::Down), 2);
  EXPECT_EQ(go(grid, 3, Move::Up), 1);
  EXPECT_EQ(go(grid, 3, Move::Down), 4);  // Settings alone
  EXPECT_EQ(go(grid, 4, Move::Down), 0);
}

// No recent book: no card row, and the first row is a row of tiles that does not cycle anything.
TEST(HomeGridNav, WithoutBooksTheTilesComeFirst) {
  const auto grid = HomeTilesLayout::grid(0, 2, 2);
  ASSERT_EQ(grid.rowCount, 2);
  EXPECT_FALSE(grid.firstRowCyclesBooks);
  EXPECT_EQ(go(grid, 0, Move::Right), 1);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Right).cycle, 0);
}
