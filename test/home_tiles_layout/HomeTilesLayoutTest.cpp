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

// Lyra Tiles' layout before widgets, kept as the reference the Reader layout is held to.
namespace legacy {
using namespace HomeTilesLayout;
constexpr int kMaxTiles = 4;

struct Regions {
  Box books;  // the card, and the Also reading row under it when it fits
  Box menu;   // the tiles, then the bottom row
};

// `content` is the screen above the button hints; `top` is the first line under the status line.
// `tileCount` is the destinations shown as tiles (at most kMaxTiles); the bottom row always exists.
constexpr Regions regions(const Box& content, const int top, const int tileCount, const Dimensions& d = kRegular) {
  const int tiles = tileCount < 0 ? 0 : (tileCount > kMaxTiles ? kMaxTiles : tileCount);
  const int bottom = content.y + content.h - d.bottomPadding;
  Regions r{};
  if (!sideways(content)) {
    const int x = content.x + d.sidePadding;
    const int w = content.w - 2 * d.sidePadding;
    const int rows = (tiles + 1) / 2;
    int tileH = (content.h * 16 + 50) / 100;
    tileH = tileH < d.minTileHeight ? d.minTileHeight : (tileH > d.maxTileHeight ? d.maxTileHeight : tileH);
    // Each row of tiles brings the gap under it, down to the bottom row.
    const int menuH = rows * (tileH + d.gap) + d.bottomRowHeight;
    r.menu = Box{x, bottom - menuH, w, menuH};
    const int booksH = r.menu.y - d.gap - top;
    r.books = Box{x, top, w, booksH > 0 ? booksH : 0};
    return r;
  }
  const int leftW = (content.w * 56 + 50) / 100 - d.sidePadding;
  r.books = Box{content.x + d.sidePadding, top, leftW, bottom - top};
  const int menuX = r.books.x + leftW + d.gap;
  r.menu = Box{menuX, top, content.x + content.w - d.sidePadding - menuX, bottom - top};
  return r;
}

// Whether the panel has room for the Also reading row: the card keeps minCardHeight with two full
// rows of tiles and the bottom row under it. Fewer tiles give the card more room, not the row.
constexpr bool stripFits(const Box& content, const int top, const Dimensions& d = kRegular) {
  return regions(content, top, kMaxTiles, d).books.h - d.stripHeight - d.gap >= d.minCardHeight;
}

struct Books {
  Box card;
  Box cover;  // 6:10, the thumbnail's exact box
  Box text;   // title, author, series, history
  Box foot;   // progress line and bar, across the card
  bool hasStrip = false;
  Box strip;  // the Also reading row, label line included
  Box slots[kStripBooks];
  Box slotCovers[kStripBooks];
};

// `withStrip` from stripFits(); the row then takes the foot of `area`.
constexpr Books books(const Box& area, const bool withStrip, const Dimensions& d = kRegular) {
  Books b{};
  b.card = area;
  if (withStrip && area.h - d.stripHeight - d.gap >= d.minCardHeight) {
    b.hasStrip = true;
    b.card.h = area.h - d.stripHeight - d.gap;
    b.strip = Box{area.x, area.y + area.h - d.stripHeight, area.w, d.stripHeight};
    const int slotW = (b.strip.w - (kStripBooks - 1) * d.gap) / kStripBooks;
    const int slotH = d.stripHeight - d.stripLabelHeight;
    for (int i = 0; i < kStripBooks; ++i) {
      b.slots[i] = Box{b.strip.x + i * (slotW + d.gap), b.strip.y + d.stripLabelHeight, slotW, slotH};
      b.slotCovers[i] = Box{b.slots[i].x, b.slots[i].y, slotH * 3 / 5, slotH};
    }
  }

  const int innerW = b.card.w - 2 * d.cardPadding;
  int coverAreaH = b.card.h - 2 * d.cardPadding - d.cardFootHeight - d.coverToFoot;
  if (coverAreaH < 0) coverAreaH = 0;
  // Thumbnails are 6:10. A wide card (sideways) would make the cover take the text's room, so the
  // cover keeps to 45 % of the card's width and gives up height instead.
  int coverH = coverAreaH;
  int coverW = coverH * 3 / 5;
  const int maxCoverW = innerW * 9 / 20;
  if (coverW > maxCoverW) {
    coverW = maxCoverW;
    coverH = coverW * 5 / 3;
  }
  b.cover = Box{b.card.x + d.cardPadding, b.card.y + d.cardPadding, coverW, coverH};
  const int textX = b.cover.x + coverW + d.coverToText;
  const int textW = b.card.x + b.card.w - d.cardPadding - textX;
  b.text = Box{textX, b.card.y + d.cardPadding, textW > 0 ? textW : 0, coverAreaH};
  b.foot = Box{b.card.x + d.cardPadding, b.card.y + b.card.h - d.cardPadding - d.cardFootHeight,
               innerW > 0 ? innerW : 0, d.cardFootHeight};
  return b;
}

struct Menu {
  int tileCount = 0;
  Box tiles[kMaxTiles];
  int bottomCount = 0;
  Box bottom[2];
};

// `tileCount` destinations as tiles, two to a row, an odd first one across the whole row; then
// `bottomCount` (1: Settings, 2: More and Settings) along the foot of `area`.
constexpr Menu menu(const Box& area, const int tileCount, const int bottomCount, const Dimensions& d = kRegular) {
  Menu m{};
  m.tileCount = tileCount < 0 ? 0 : (tileCount > kMaxTiles ? kMaxTiles : tileCount);
  m.bottomCount = bottomCount < 1 ? 1 : (bottomCount > 2 ? 2 : bottomCount);

  const int bottomY = area.y + area.h - d.bottomRowHeight;
  const int halfW = (area.w - d.gap) / 2;
  if (m.bottomCount == 1) {
    m.bottom[0] = Box{area.x, bottomY, area.w, d.bottomRowHeight};
  } else {
    m.bottom[0] = Box{area.x, bottomY, halfW, d.bottomRowHeight};
    m.bottom[1] = Box{area.x + area.w - halfW, bottomY, halfW, d.bottomRowHeight};
  }

  const int rows = (m.tileCount + 1) / 2;
  if (rows == 0) return m;
  int tileH = (bottomY - d.gap - area.y - (rows - 1) * d.gap) / rows;
  if (tileH > d.maxSidewaysTileHeight) tileH = d.maxSidewaysTileHeight;
  if (tileH < 0) tileH = 0;
  const bool firstSpans = (m.tileCount % 2) == 1;
  for (int i = 0; i < m.tileCount; ++i) {
    // With an odd count the first tile spans the row, and the rest pair up after it.
    const int slot = firstSpans ? i + 1 : i;
    const int row = slot / 2;
    const int y = area.y + row * (tileH + d.gap);
    if (firstSpans && i == 0) {
      m.tiles[i] = Box{area.x, y, area.w, tileH};
    } else {
      m.tiles[i] = Box{slot % 2 == 0 ? area.x : area.x + area.w - halfW, y, halfW, tileH};
    }
  }
  return m;
}

// The screen's rows for HomeGridNav, in the order of Home's selector: the card, the Also reading
// books, the tiles as menu() places them, then the bottom row. `shownBooks` counts the card.
constexpr HomeGridNav::Grid grid(const int shownBooks, const int tileCount, const int bottomCount) {
  HomeGridNav::Grid g{};
  int next = 0;
  if (shownBooks > 0) {
    g.addRow(next, 1);
    g.markCycleRow();
    next += 1;
    g.addRow(next, shownBooks - 1);
    next += shownBooks - 1;
  }
  const int tiles = tileCount < 0 ? 0 : (tileCount > kMaxTiles ? kMaxTiles : tileCount);
  int placed = 0;
  if (tiles % 2 == 1) {
    g.addRow(next, 1);  // the first tile spans its row
    placed = 1;
  }
  for (; placed < tiles; placed += 2) g.addRow(next + placed, 2);
  next += tiles;
  g.addRow(next, bottomCount);
  return g;
}

}  // namespace legacy

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
legacy::Books booksFor(const Box& content, const int tiles) {
  return legacy::books(legacy::regions(content, kTop, tiles).books, legacy::stripFits(content, kTop));
}

std::string describe(const Box& b) {
  return "{" + std::to_string(b.x) + "," + std::to_string(b.y) + " " + std::to_string(b.w) + "x" + std::to_string(b.h) +
         "}";
}

}  // namespace

TEST(HomeTilesLayout, EveryPartIsOnScreenApartAndBigEnoughToHit) {
  for (const Panel& panel : kPanels) {
    for (int tiles = 0; tiles <= legacy::kMaxTiles; ++tiles) {
      for (int bottom = 1; bottom <= 2; ++bottom) {
        SCOPED_TRACE(std::string(panel.name) + ", " + std::to_string(tiles) + " tiles, " + std::to_string(bottom) +
                     " in the bottom row");
        const auto regions = legacy::regions(panel.content, kTop, tiles);
        const auto books = booksFor(panel.content, tiles);
        const auto menu = legacy::menu(regions.menu, tiles, bottom);
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
    for (int tiles = 0; tiles <= legacy::kMaxTiles; ++tiles) {
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
  const auto regions = legacy::regions({0, 0, 480, 760}, kTop, 4);
  const auto books = booksFor({0, 0, 480, 760}, 4);
  const auto menu = legacy::menu(regions.menu, 4, 2);
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
  for (int tiles = 0; tiles <= legacy::kMaxTiles; ++tiles) {
    SCOPED_TRACE(std::to_string(tiles) + " tiles");
    EXPECT_TRUE(booksFor({0, 0, 540, 920}, tiles).hasStrip);
    EXPECT_FALSE(booksFor({0, 0, 480, 760}, tiles).hasStrip);
    EXPECT_FALSE(booksFor({0, 0, 528, 752}, tiles).hasStrip);
  }
  EXPECT_GT(booksFor({0, 0, 480, 760}, 2).card.h, booksFor({0, 0, 480, 760}, 4).card.h);
  for (const Panel& panel : kPanels) {
    for (int tiles = 0; tiles <= legacy::kMaxTiles; ++tiles) {
      const auto books = booksFor(panel.content, tiles);
      if (books.hasStrip) {
        EXPECT_GE(books.card.h, HomeTilesLayout::kRegular.minCardHeight) << panel.name;
      }
    }
  }
}

// An odd count gives the first tile, Library, the whole row rather than leaving a hole.
TEST(HomeTilesLayout, AnOddFirstTileSpansTheRow) {
  const auto regions = legacy::regions({0, 0, 480, 760}, kTop, 3);
  const auto menu = legacy::menu(regions.menu, 3, 1);
  EXPECT_EQ(menu.tiles[0].w, regions.menu.w);
  EXPECT_EQ(menu.tiles[1].y, menu.tiles[2].y);
  EXPECT_LT(menu.tiles[1].x, menu.tiles[2].x);
  EXPECT_GT(menu.tiles[1].y, menu.tiles[0].y);
  // Settings alone takes the whole bottom row.
  EXPECT_EQ(menu.bottom[0].w, regions.menu.w);
}

// More is the left half, Settings the right: the order the entries come in.
TEST(HomeTilesLayout, MoreSitsLeftOfSettings) {
  const auto regions = legacy::regions({0, 0, 480, 760}, kTop, 4);
  const auto menu = legacy::menu(regions.menu, 4, 2);
  EXPECT_EQ(menu.bottom[0].y, menu.bottom[1].y);
  EXPECT_LT(menu.bottom[0].x + menu.bottom[0].w, menu.bottom[1].x);
  EXPECT_EQ(menu.bottom[1].x + menu.bottom[1].w, regions.menu.x + regions.menu.w);
}

// Sideways the card takes the left column and the tiles the right one, both the full height.
TEST(HomeTilesLayout, SidewaysTheCardAndTheTilesShareTheWidth) {
  const auto regions = legacy::regions({0, 0, 800, 440}, kTop, 4);
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
  const auto p = HomeTilesLayout::place({0, 0, 540, 920}, kTop, HomeTilesLayout::presets::reader(), 2);
  const auto* also = p.find(HomeTilesLayout::Widget::AlsoReading);
  ASSERT_NE(also, nullptr);
  const auto strip = HomeTilesLayout::strip(also->box);
  EXPECT_GE(strip.slots[0].y - also->box.y, HomeTilesLayout::kRegular.stripPadding + 23 + 4);
  EXPECT_EQ(strip.slotCovers[0].w, 54);
  EXPECT_EQ(strip.slotCovers[0].h, 90);
  // Inside its frame at the foot too.
  EXPECT_EQ(strip.slots[0].y + strip.slots[0].h + HomeTilesLayout::kRegular.stripPadding, also->box.y + also->box.h);
}

// The compact panel (Murphy, 240 x 416, hints 30 px, the body under a 28 px status line) with its
// own sizes: everything on screen and apart, nothing under its 36 px menu rows, a text column wide
// enough for a title, and no Also reading row.
TEST(HomeTilesLayout, TheCompactPanelFitsTheCardAndTwoRowsOfTiles) {
  constexpr int kCompactTop = 28;
  const Box murphyPanels[] = {{0, 0, 240, 386}, {0, 0, 416, 210}};
  for (const Box& content : murphyPanels) {
    for (int tiles = 0; tiles <= legacy::kMaxTiles; ++tiles) {
      for (int bottom = 1; bottom <= 2; ++bottom) {
        SCOPED_TRACE(describe(content) + ", " + std::to_string(tiles) + " tiles, " + std::to_string(bottom));
        const auto& d = HomeTilesLayout::kCompact;
        const auto regions = legacy::regions(content, kCompactTop, tiles, d);
        const auto books = legacy::books(regions.books, legacy::stripFits(content, kCompactTop, d), d);
        const auto menu = legacy::menu(regions.menu, tiles, bottom, d);
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
  const auto grid = legacy::grid(/*shownBooks=*/1, /*tiles=*/2, /*bottom=*/2);
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
  const auto grid = legacy::grid(1, 4, 2);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Right).cycle, 1);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Left).cycle, -1);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Right).index, 0);  // the selection stays on the card
  EXPECT_EQ(HomeGridNav::step(grid, 1, Move::Right).cycle, 0);  // a tile moves, it does not cycle
}

// The T5S3: the card, three Also reading books (1-3), four tiles (4-7), More 8, Settings 9.
TEST(HomeGridNav, TheAlsoReadingRowIsARowOfThree) {
  const auto grid = legacy::grid(4, 4, 2);
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

// Up and Down alone step through the cells in screen order, wrapping at both ends.
TEST(HomeGridNav, NextFollowsTheScreenOrder) {
  HomeGridNav::Grid grid;
  grid.addRow(0, 1);
  grid.markCycleRow();
  const int widgets[] = {7, 6};  // positions numbered after the menu, drawn between card and tiles
  grid.addCells(widgets, 2);
  grid.addRow(1, 2);
  EXPECT_EQ(HomeGridNav::next(grid, 0, 1), 7);
  EXPECT_EQ(HomeGridNav::next(grid, 7, 1), 6);
  EXPECT_EQ(HomeGridNav::next(grid, 6, 1), 1);
  EXPECT_EQ(HomeGridNav::next(grid, 2, 1), 0);
  EXPECT_EQ(HomeGridNav::next(grid, 0, -1), 2);
  EXPECT_EQ(HomeGridNav::next(grid, 42, 1), 0);  // not on the grid: the first cell
  EXPECT_EQ(go(grid, 0, Move::Down), 7);
  EXPECT_EQ(go(grid, 6, Move::Down), 2);  // the right-hand widget -> the right-hand tile
  EXPECT_EQ(HomeGridNav::step(grid, 7, Move::Right).index, 6);
}

// Three tiles: Library spans its row, so it is a row of one.
TEST(HomeGridNav, ASpanningTileIsARowOfOne) {
  const auto grid = legacy::grid(1, 3, 1);
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
  const auto grid = legacy::grid(0, 2, 2);
  ASSERT_EQ(grid.rowCount, 2);
  EXPECT_EQ(grid.cycleRow, -1);
  EXPECT_EQ(go(grid, 0, Move::Right), 1);
  EXPECT_EQ(HomeGridNav::step(grid, 0, Move::Right).cycle, 0);
}

// --- Widgets -------------------------------------------------------------------------------------

namespace {

bool same(const Box& a, const Box& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

struct Screen {
  const char* name;
  Box content;
  int top;
  const HomeTilesLayout::Dimensions* d;
};
const Screen kScreens[] = {
    {"X4 upright", {0, 0, 480, 760}, kTop, &HomeTilesLayout::kRegular},
    {"X3 upright", {0, 0, 528, 752}, kTop, &HomeTilesLayout::kRegular},
    {"T5S3 upright", {0, 0, 540, 920}, kTop, &HomeTilesLayout::kRegular},
    {"X4 sideways", {0, 0, 800, 440}, kTop, &HomeTilesLayout::kRegular},
    {"X3 sideways", {0, 0, 792, 488}, kTop, &HomeTilesLayout::kRegular},
    {"T5S3 sideways", {0, 0, 960, 500}, kTop, &HomeTilesLayout::kRegular},
    {"X4 sideways, side hints", {30, 0, 740, 480}, kTop, &HomeTilesLayout::kRegular},
    {"Murphy upright", {0, 0, 240, 386}, 28, &HomeTilesLayout::kCompact},
    {"Murphy sideways", {0, 0, 416, 210}, 28, &HomeTilesLayout::kCompact},
};

using HomeTilesLayout::Widget;
namespace dest = HomeTilesLayout::destination;

std::vector<Box> shortcutBoxes(const HomeTilesLayout::Placement& p) {
  std::vector<Box> boxes;
  for (int i = 0; i < p.count; ++i) {
    if (p.items[i].cell.widget == Widget::Shortcut) boxes.push_back(p.items[i].box);
  }
  return boxes;
}

}  // namespace

// The Reader layout is Lyra Tiles as it was with its four tiles: every box where the old layout
// put it, on every panel, so switching Tiles to widgets moves no pixel, and the same rows to move
// through.
TEST(HomeWidgets, TheReaderLayoutIsLyraTilesAsItWas) {
  const auto reader = HomeTilesLayout::presets::reader();
  for (const Screen& s : kScreens) {
    for (int bottom = 1; bottom <= 2; ++bottom) {
      SCOPED_TRACE(std::string(s.name) + ", " + std::to_string(bottom) + " in the bottom row");
      const auto& d = *s.d;
      const auto regions = legacy::regions(s.content, s.top, 4, d);
      const auto books = legacy::books(regions.books, legacy::stripFits(s.content, s.top, d), d);
      const auto menu = legacy::menu(regions.menu, 4, bottom, d);
      const auto p = HomeTilesLayout::place(s.content, s.top, reader, bottom, d);

      const auto* book = p.find(Widget::Book);
      ASSERT_NE(book, nullptr);
      EXPECT_TRUE(same(book->box, books.card)) << describe(book->box) << " vs " << describe(books.card);
      const auto card = HomeTilesLayout::card(book->box, d);
      EXPECT_TRUE(same(card.cover, books.cover)) << describe(card.cover);
      EXPECT_TRUE(same(card.text, books.text)) << describe(card.text);
      EXPECT_TRUE(same(card.foot, books.foot)) << describe(card.foot);

      const auto* also = p.find(Widget::AlsoReading);
      EXPECT_EQ(also != nullptr, books.hasStrip);
      if (also != nullptr && books.hasStrip) {
        EXPECT_TRUE(same(also->box, books.strip)) << describe(also->box);
        // The row has a frame now, which the reference did not: its books sit inside it.
        const auto strip = HomeTilesLayout::strip(also->box, d);
        for (int i = 0; i < HomeTilesLayout::kStripBooks; ++i) {
          EXPECT_TRUE(inside(strip.slots[i], also->box)) << describe(strip.slots[i]);
          EXPECT_GE(strip.slots[i].x - also->box.x, d.stripPadding);
        }
      }

      const auto tiles = shortcutBoxes(p);
      ASSERT_EQ(static_cast<int>(tiles.size()), menu.tileCount);
      for (int i = 0; i < menu.tileCount; ++i) {
        EXPECT_TRUE(same(tiles[i], menu.tiles[i]))
            << i << ": " << describe(tiles[i]) << " vs " << describe(menu.tiles[i]);
      }
      ASSERT_EQ(p.bottomCount, menu.bottomCount);
      for (int i = 0; i < p.bottomCount; ++i) EXPECT_TRUE(same(p.bottom[i], menu.bottom[i])) << describe(p.bottom[i]);
      EXPECT_EQ(p.dropped, 0);

      // The same rows to move through; the tiles are numbered after the bottom row now.
      const int shown = books.hasStrip ? 4 : 1;
      const auto oldGrid = legacy::grid(shown, 4, bottom);
      const auto newGrid = HomeTilesLayout::grid(p, shown, bottom);
      ASSERT_EQ(newGrid.rowCount, oldGrid.rowCount);
      for (int r = 0; r < newGrid.rowCount; ++r) EXPECT_EQ(newGrid.count[r], oldGrid.count[r]);
      EXPECT_EQ(newGrid.cycleRow, oldGrid.cycleRow);
    }
  }
}

// Every ready-made layout on every panel: each widget on screen, inside the body and apart from the
// others, and the bottom row below them all.
TEST(HomeWidgets, EveryLayoutKeepsItsWidgetsOnScreenAndApart) {
  using HomeTilesLayout::Config;
  const std::pair<const char*, Config> layouts[] = {{"reader", HomeTilesLayout::presets::reader()},
                                                    {"dashboard", HomeTilesLayout::presets::dashboard()},
                                                    {"gallery", HomeTilesLayout::presets::gallery()}};
  for (const auto& [name, cfg] : layouts) {
    for (const Screen& s : kScreens) {
      SCOPED_TRACE(std::string(name) + " on " + s.name);
      const auto p = HomeTilesLayout::place(s.content, s.top, cfg, 2, *s.d);
      const Box body{s.content.x, s.top, s.content.w, s.content.h - s.top};
      std::vector<Box> boxes;
      for (int i = 0; i < p.count; ++i) boxes.push_back(p.items[i].box);
      for (int i = 0; i < p.bottomCount; ++i) boxes.push_back(p.bottom[i]);
      for (size_t i = 0; i < boxes.size(); ++i) {
        EXPECT_TRUE(inside(boxes[i], body)) << describe(boxes[i]);
        EXPECT_GT(boxes[i].h, 0) << describe(boxes[i]);
        for (size_t j = i + 1; j < boxes.size(); ++j) {
          EXPECT_FALSE(overlap(boxes[i], boxes[j])) << describe(boxes[i]) << " / " << describe(boxes[j]);
        }
      }
      ASSERT_NE(p.find(Widget::Book), nullptr);  // the book card is never the row left out
    }
  }
}

// Dashboard on the X3: the card takes what the clock and picture row and the summary row leave. On
// the Murphy the summary's row does not fit beside a readable card and is left out, and only it.
TEST(HomeWidgets, ARowThatDoesNotFitIsLeftOutFromTheBottom) {
  const auto dash = HomeTilesLayout::presets::dashboard();
  const auto x3 = HomeTilesLayout::place({0, 0, 528, 752}, kTop, dash, 2);
  EXPECT_EQ(x3.dropped, 0);
  ASSERT_NE(x3.find(Widget::Book), nullptr);
  EXPECT_GE(x3.find(Widget::Book)->box.h, HomeTilesLayout::kRegular.bookMinHeight);
  const auto* clock = x3.find(Widget::Clock);
  const auto* picture = x3.find(Widget::Picture);
  ASSERT_NE(clock, nullptr);
  ASSERT_NE(picture, nullptr);
  EXPECT_FALSE(clock->full);
  EXPECT_EQ(clock->box.y, picture->box.y);
  EXPECT_LT(clock->box.x, picture->box.x);
  // A tile beside the summary takes the row's height.
  const auto* summary = x3.find(Widget::Summary);
  const auto* library = x3.find(Widget::Shortcut);
  ASSERT_NE(summary, nullptr);
  ASSERT_NE(library, nullptr);
  EXPECT_EQ(library->box.h, summary->box.h);

  const auto murphy = HomeTilesLayout::place({0, 0, 240, 386}, 28, dash, 2, HomeTilesLayout::kCompact);
  EXPECT_EQ(murphy.dropped, 1 << 2);
  EXPECT_EQ(murphy.find(Widget::Summary), nullptr);
  EXPECT_EQ(murphy.find(Widget::Shortcut), nullptr);  // its row partner goes with it
  EXPECT_NE(murphy.find(Widget::Clock), nullptr);
}

// A picture across the screen takes the room left, like the large card; the compact card keeps its
// own height.
TEST(HomeWidgets, GalleryGivesThePictureTheRoomLeft) {
  const auto p = HomeTilesLayout::place({0, 0, 480, 760}, kTop, HomeTilesLayout::presets::gallery(), 2);
  const auto* picture = p.find(Widget::Picture);
  const auto* book = p.find(Widget::Book);
  ASSERT_NE(picture, nullptr);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->box.h, HomeTilesLayout::kRegular.bookCompactHeight);
  EXPECT_GE(picture->box.h, HomeTilesLayout::kRegular.pictureMinHeight);
  EXPECT_LT(picture->box.y, book->box.y);
  const auto tiles = shortcutBoxes(p);
  ASSERT_EQ(tiles.size(), 2u);
  // The rows end one gap above the bottom row.
  EXPECT_EQ(tiles[0].y + tiles[0].h + HomeTilesLayout::kRegular.gap, p.bottom[0].y);
}

// A tile alone in its row takes the whole row.
TEST(HomeWidgets, ATileAloneTakesTheRow) {
  HomeTilesLayout::Config cfg;
  cfg.add({Widget::Book}).add({Widget::Shortcut, dest::FileTransfer});
  const auto p = HomeTilesLayout::place({0, 0, 480, 760}, kTop, cfg, 2);
  const auto* tile = p.find(Widget::Shortcut);
  ASSERT_NE(tile, nullptr);
  EXPECT_TRUE(tile->full);
  EXPECT_EQ(tile->box.w, 480 - 2 * HomeTilesLayout::kRegular.sidePadding);
}

// Widgets that open something are numbered after the menu, in reading order, and the grid visits
// them where they are drawn.
TEST(HomeWidgets, WidgetsThatOpenSomethingFollowTheMenuInNumberAndTheScreenInOrder) {
  HomeTilesLayout::Config cfg;
  cfg.add({Widget::Book})
      .add({Widget::Weather}, {Widget::Summary})
      .add({Widget::Shortcut, dest::Library}, {Widget::Shortcut, dest::Bookmarks});
  const auto p = HomeTilesLayout::place({0, 0, 480, 760}, kTop, cfg, 2);
  Widget acts[6]{};
  ASSERT_EQ(HomeTilesLayout::actions(p, acts, 6), 4);
  EXPECT_EQ(acts[0], Widget::Weather);
  EXPECT_EQ(acts[1], Widget::Summary);
  EXPECT_EQ(acts[2], Widget::Shortcut);
  // Selector: card 0, More 1, Settings 2, then weather 3, summary 4, Library 5, Bookmarks 6.
  const auto g = HomeTilesLayout::grid(p, 1, 2);
  ASSERT_EQ(g.rowCount, 4);
  EXPECT_EQ(HomeGridNav::next(g, 0, 1), 3);
  EXPECT_EQ(HomeGridNav::next(g, 4, 1), 5);
  EXPECT_EQ(HomeGridNav::next(g, 6, 1), 1);
  EXPECT_EQ(HomeGridNav::next(g, 2, 1), 0);
  EXPECT_EQ(HomeGridNav::step(g, 4, HomeGridNav::Move::Down).index, 6);
  EXPECT_EQ(HomeGridNav::step(g, 5, HomeGridNav::Move::Down).index, 1);
}

// Sideways the book card keeps the left column and the other widgets share the right one with the
// bottom row; tiles there grow with the room, to their sideways cap.
TEST(HomeWidgets, SidewaysTheCardTakesTheLeftColumn) {
  const auto p = HomeTilesLayout::place({0, 0, 800, 440}, kTop, HomeTilesLayout::presets::dashboard(), 2);
  const auto* book = p.find(Widget::Book);
  const auto* clock = p.find(Widget::Clock);
  ASSERT_NE(book, nullptr);
  ASSERT_NE(clock, nullptr);
  EXPECT_TRUE(p.sideways);
  EXPECT_LT(book->box.x + book->box.w, clock->box.x);
  EXPECT_EQ(book->box.y, clock->box.y);
  for (const Box& tile : shortcutBoxes(p)) EXPECT_LE(tile.h, HomeTilesLayout::kRegular.maxSidewaysTileHeight + 60);
}
