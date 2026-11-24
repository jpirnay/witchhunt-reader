#pragma once

#include "components/themes/HomeGridNav.h"

// Where the Lyra Tiles theme puts each part of Home: the card of the book in front, the "Also
// reading" row, the tiles of the destinations and the bottom row (More, Settings).
//
// Proportions rather than per-board numbers: the tiles and the bottom row take a share of the
// height, and the card takes what they leave, so a taller panel gets a taller card, and then the
// Also reading row once the card can spare the room. Held sideways, the card takes the left column
// and the tiles the right one instead of the rows being squeezed.
//
// Pure integer arithmetic over its inputs, with no theme or renderer dependency, so the host tests
// run it for every panel (test/home_tiles_layout).
namespace HomeTilesLayout {

struct Box {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

constexpr int kMaxTiles = 4;
constexpr int kStripBooks = 3;

// The sizes the proportions work with. Two sets: the 480 px panels and up, and the compact one
// (Murphy, 240 px), where every size of the large set would leave the card no room at all.
struct Dimensions {
  int gap;
  int sidePadding;
  int bottomPadding;
  // More and Settings: as tall as Lyra's menu rows, so as easy to hit, and set apart by being lighter.
  int bottomRowHeight;
  // Upright, a tile is 16 % of the content's height within these bounds.
  int minTileHeight;
  int maxTileHeight;
  // Sideways the tiles share the right column's height, up to this.
  int maxSidewaysTileHeight;
  // The Also reading row, its label line included. It appears only where the card keeps
  // minCardHeight beside it and a full grid of tiles (stripFits): a panel shows it or does not,
  // whatever the number of entries.
  int stripHeight;
  // One line of the small face (23 px on the 480 px panels) and the space under it.
  int stripLabelHeight;
  int minCardHeight;
  // Inside the card: the padding, the progress line and bar at its foot, and the space between
  // the cover and the text column and between the cover and the foot.
  int cardPadding;
  int cardFootHeight;
  int coverToText;
  int coverToFoot;
};

inline constexpr Dimensions kRegular{/*gap=*/12,
                                     /*sidePadding=*/16,
                                     /*bottomPadding=*/8,
                                     /*bottomRowHeight=*/60,
                                     /*minTileHeight=*/104,
                                     /*maxTileHeight=*/136,
                                     /*maxSidewaysTileHeight=*/180,
                                     /*stripHeight=*/118,
                                     /*stripLabelHeight=*/28,
                                     /*minCardHeight=*/360,
                                     /*cardPadding=*/14,
                                     /*cardFootHeight=*/36,
                                     /*coverToText=*/18,
                                     /*coverToFoot=*/10};

// Sized for Murphy's 240 x 416 with its smaller faces (a body line is 20 px): a 176 px card, two
// rows of 62 px tiles and a 36 px bottom row, as tall as its capped menu rows. No room for the
// Also reading row on any compact panel.
inline constexpr Dimensions kCompact{/*gap=*/6,
                                     /*sidePadding=*/8,
                                     /*bottomPadding=*/4,
                                     /*bottomRowHeight=*/36,
                                     /*minTileHeight=*/56,
                                     /*maxTileHeight=*/72,
                                     /*maxSidewaysTileHeight=*/90,
                                     /*stripHeight=*/118,
                                     /*stripLabelHeight=*/28,
                                     /*minCardHeight=*/360,
                                     /*cardPadding=*/8,
                                     /*cardFootHeight=*/24,
                                     /*coverToText=*/10,
                                     /*coverToFoot=*/6};

// Upright or sideways is decided by the content, not the panel: the X4 held in landscape is wide.
constexpr bool sideways(const Box& content) { return content.w > content.h; }

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
    g.firstRowCyclesBooks = true;
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

}  // namespace HomeTilesLayout
