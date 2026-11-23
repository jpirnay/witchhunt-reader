#pragma once

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

constexpr int kGap = 12;
constexpr int kSidePadding = 16;
constexpr int kBottomPadding = 8;
// More and Settings: as tall as Lyra's menu rows, so as easy to hit, and set apart by being lighter.
constexpr int kBottomRowHeight = 60;
// Upright, a tile is 16 % of the content's height within these bounds.
constexpr int kMinTileHeight = 104;
constexpr int kMaxTileHeight = 136;
// Sideways the tiles share the right column's height, up to this.
constexpr int kMaxSidewaysTileHeight = 180;
constexpr int kMaxTiles = 4;

// The Also reading row, its label line included. It appears only where the card keeps
// kMinCardHeight beside it and a full grid of tiles (stripFits): a panel shows it or does not,
// whatever the number of entries.
constexpr int kStripHeight = 118;
// One line of the small face (23 px on the 480 px panels) and the space under it. At 22 the line sat
// on the covers.
constexpr int kStripLabelHeight = 28;
constexpr int kStripBooks = 3;
constexpr int kMinCardHeight = 360;

// Inside the card: the padding, the progress line and bar at its foot, and the space between the
// cover and the text column and between the cover and the foot.
constexpr int kCardPadding = 14;
constexpr int kCardFootHeight = 36;
constexpr int kCoverToText = 18;
constexpr int kCoverToFoot = 10;

// Upright or sideways is decided by the content, not the panel: the X4 held in landscape is wide.
constexpr bool sideways(const Box& content) { return content.w > content.h; }

struct Regions {
  Box books;  // the card, and the Also reading row under it when it fits
  Box menu;   // the tiles, then the bottom row
};

// `content` is the screen above the button hints; `top` is the first line under the status line.
// `tileCount` is the destinations shown as tiles (at most kMaxTiles); the bottom row always exists.
constexpr Regions regions(const Box& content, const int top, const int tileCount) {
  const int tiles = tileCount < 0 ? 0 : (tileCount > kMaxTiles ? kMaxTiles : tileCount);
  const int bottom = content.y + content.h - kBottomPadding;
  Regions r{};
  if (!sideways(content)) {
    const int x = content.x + kSidePadding;
    const int w = content.w - 2 * kSidePadding;
    const int rows = (tiles + 1) / 2;
    int tileH = (content.h * 16 + 50) / 100;
    tileH = tileH < kMinTileHeight ? kMinTileHeight : (tileH > kMaxTileHeight ? kMaxTileHeight : tileH);
    // Each row of tiles brings the gap under it, down to the bottom row.
    const int menuH = rows * (tileH + kGap) + kBottomRowHeight;
    r.menu = Box{x, bottom - menuH, w, menuH};
    const int booksH = r.menu.y - kGap - top;
    r.books = Box{x, top, w, booksH > 0 ? booksH : 0};
    return r;
  }
  const int leftW = (content.w * 56 + 50) / 100 - kSidePadding;
  r.books = Box{content.x + kSidePadding, top, leftW, bottom - top};
  const int menuX = r.books.x + leftW + kGap;
  r.menu = Box{menuX, top, content.x + content.w - kSidePadding - menuX, bottom - top};
  return r;
}

// Whether the panel has room for the Also reading row: the card keeps kMinCardHeight with two full
// rows of tiles and the bottom row under it. Fewer tiles give the card more room, not the row.
constexpr bool stripFits(const Box& content, const int top) {
  return regions(content, top, kMaxTiles).books.h - kStripHeight - kGap >= kMinCardHeight;
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
constexpr Books books(const Box& area, const bool withStrip) {
  Books b{};
  b.card = area;
  if (withStrip && area.h - kStripHeight - kGap >= kMinCardHeight) {
    b.hasStrip = true;
    b.card.h = area.h - kStripHeight - kGap;
    b.strip = Box{area.x, area.y + area.h - kStripHeight, area.w, kStripHeight};
    const int slotW = (b.strip.w - (kStripBooks - 1) * kGap) / kStripBooks;
    const int slotH = kStripHeight - kStripLabelHeight;
    for (int i = 0; i < kStripBooks; ++i) {
      b.slots[i] = Box{b.strip.x + i * (slotW + kGap), b.strip.y + kStripLabelHeight, slotW, slotH};
      b.slotCovers[i] = Box{b.slots[i].x, b.slots[i].y, slotH * 3 / 5, slotH};
    }
  }

  const int innerW = b.card.w - 2 * kCardPadding;
  int coverAreaH = b.card.h - 2 * kCardPadding - kCardFootHeight - kCoverToFoot;
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
  b.cover = Box{b.card.x + kCardPadding, b.card.y + kCardPadding, coverW, coverH};
  const int textX = b.cover.x + coverW + kCoverToText;
  const int textW = b.card.x + b.card.w - kCardPadding - textX;
  b.text = Box{textX, b.card.y + kCardPadding, textW > 0 ? textW : 0, coverAreaH};
  b.foot = Box{b.card.x + kCardPadding, b.card.y + b.card.h - kCardPadding - kCardFootHeight, innerW > 0 ? innerW : 0,
               kCardFootHeight};
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
constexpr Menu menu(const Box& area, const int tileCount, const int bottomCount) {
  Menu m{};
  m.tileCount = tileCount < 0 ? 0 : (tileCount > kMaxTiles ? kMaxTiles : tileCount);
  m.bottomCount = bottomCount < 1 ? 1 : (bottomCount > 2 ? 2 : bottomCount);

  const int bottomY = area.y + area.h - kBottomRowHeight;
  const int halfW = (area.w - kGap) / 2;
  if (m.bottomCount == 1) {
    m.bottom[0] = Box{area.x, bottomY, area.w, kBottomRowHeight};
  } else {
    m.bottom[0] = Box{area.x, bottomY, halfW, kBottomRowHeight};
    m.bottom[1] = Box{area.x + area.w - halfW, bottomY, halfW, kBottomRowHeight};
  }

  const int rows = (m.tileCount + 1) / 2;
  if (rows == 0) return m;
  int tileH = (bottomY - kGap - area.y - (rows - 1) * kGap) / rows;
  if (tileH > kMaxSidewaysTileHeight) tileH = kMaxSidewaysTileHeight;
  if (tileH < 0) tileH = 0;
  const bool firstSpans = (m.tileCount % 2) == 1;
  for (int i = 0; i < m.tileCount; ++i) {
    // With an odd count the first tile spans the row, and the rest pair up after it.
    const int slot = firstSpans ? i + 1 : i;
    const int row = slot / 2;
    const int y = area.y + row * (tileH + kGap);
    if (firstSpans && i == 0) {
      m.tiles[i] = Box{area.x, y, area.w, tileH};
    } else {
      m.tiles[i] = Box{slot % 2 == 0 ? area.x : area.x + area.w - halfW, y, halfW, tileH};
    }
  }
  return m;
}

}  // namespace HomeTilesLayout
