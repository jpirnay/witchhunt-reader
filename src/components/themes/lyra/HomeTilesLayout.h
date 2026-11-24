#pragma once

#include <cstdint>

#include "components/themes/HomeGridNav.h"

// Where the Lyra Tiles theme puts each part of Home. Home is a column of rows, each holding one
// widget across the screen or two side by side (Config); a row of More and Settings always closes
// it, so no layout can put Settings out of reach. A shortcut is a widget of its own, one tile that
// opens one destination; More holds every destination the layout does not show. The ready-made
// layouts are in `presets`; Reader is Lyra Tiles as it was before widgets: the book card, the Also
// reading row where it fits, and two rows of two tiles.
//
// Proportions rather than per-board numbers: the shortcut tiles take a share of the height, the
// other widgets their own heights, and the book card (or a picture across the screen) takes what
// they leave, so a taller panel gets a taller card. A row that would leave the card less than its
// floor is left out, from the bottom up. Held sideways, the book card takes the left column and the
// rest the right one instead of the rows being squeezed.
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
  // The Also reading row, its frame and label line included. It appears only where the book card
  // keeps minCardHeight beside it and the shortcut rows the layout asks for.
  int stripHeight;
  // One line of the small face (23 px on the 480 px panels) and the space under it.
  int stripLabelHeight;
  // Between the row's frame and what it holds.
  int stripPadding;
  int minCardHeight;
  // Inside the card: the padding, the progress line and bar at its foot, and the space between
  // the cover and the text column and between the cover and the foot.
  int cardPadding;
  int cardFootHeight;
  int coverToText;
  int coverToFoot;
  // The widgets' heights. A widget beside another has the plain height, one across the screen the
  // "full" one. The large book card, and a picture across the screen, take the height the other
  // rows leave, down to their minimum.
  int bookMinHeight;
  int bookCompactHeight;
  int clockHeight;
  int clockFullHeight;
  int calendarHeight;
  int summaryHeight;
  int summaryFullHeight;
  int weatherHeight;
  int weatherFullHeight;
  int pictureHeight;
  int pictureMinHeight;
};

inline constexpr Dimensions kRegular{/*gap=*/12,
                                     /*sidePadding=*/16,
                                     /*bottomPadding=*/8,
                                     /*bottomRowHeight=*/60,
                                     /*minTileHeight=*/104,
                                     /*maxTileHeight=*/136,
                                     /*maxSidewaysTileHeight=*/180,
                                     /*stripHeight=*/130,
                                     /*stripLabelHeight=*/28,
                                     /*stripPadding=*/6,
                                     /*minCardHeight=*/360,
                                     /*cardPadding=*/14,
                                     /*cardFootHeight=*/36,
                                     /*coverToText=*/18,
                                     /*coverToFoot=*/10,
                                     /*bookMinHeight=*/240,
                                     /*bookCompactHeight=*/210,
                                     /*clockHeight=*/170,
                                     /*clockFullHeight=*/130,
                                     /*calendarHeight=*/200,
                                     /*summaryHeight=*/170,
                                     /*summaryFullHeight=*/160,
                                     /*weatherHeight=*/170,
                                     /*weatherFullHeight=*/130,
                                     /*pictureHeight=*/170,
                                     /*pictureMinHeight=*/200};

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
                                     /*stripHeight=*/130,
                                     /*stripLabelHeight=*/28,
                                     /*stripPadding=*/4,
                                     /*minCardHeight=*/360,
                                     /*cardPadding=*/8,
                                     /*cardFootHeight=*/24,
                                     /*coverToText=*/10,
                                     /*coverToFoot=*/6,
                                     /*bookMinHeight=*/120,
                                     /*bookCompactHeight=*/120,
                                     /*clockHeight=*/96,
                                     /*clockFullHeight=*/80,
                                     /*calendarHeight=*/128,
                                     /*summaryHeight=*/96,
                                     /*summaryFullHeight=*/104,
                                     /*weatherHeight=*/96,
                                     /*weatherFullHeight=*/80,
                                     /*pictureHeight=*/96,
                                     /*pictureMinHeight=*/110};

// Upright or sideways is decided by the content, not the panel: the X4 held in landscape is wide.
constexpr bool sideways(const Box& content) { return content.w > content.h; }

// ---- Inside the book card and the Also reading row ----------------------------------------------

struct Card {
  Box cover;  // 6:10, the thumbnail's exact box
  Box text;   // title, author, series, history
  Box foot;   // progress line and bar, across the card
};

constexpr Card card(const Box& box, const Dimensions& d = kRegular) {
  Card c{};
  const int innerW = box.w - 2 * d.cardPadding;
  int coverAreaH = box.h - 2 * d.cardPadding - d.cardFootHeight - d.coverToFoot;
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
  c.cover = Box{box.x + d.cardPadding, box.y + d.cardPadding, coverW, coverH};
  const int textX = c.cover.x + coverW + d.coverToText;
  const int textW = box.x + box.w - d.cardPadding - textX;
  c.text = Box{textX, box.y + d.cardPadding, textW > 0 ? textW : 0, coverAreaH};
  c.foot = Box{box.x + d.cardPadding, box.y + box.h - d.cardPadding - d.cardFootHeight, innerW > 0 ? innerW : 0,
               d.cardFootHeight};
  return c;
}

struct Strip {
  Box slots[kStripBooks];
  Box slotCovers[kStripBooks];
};

// The three books of an Also reading row: inside its frame the label line on top, then cover and
// text. The 480 px panels' row keeps the covers at 54 x 90, the size their thumbnails were made at.
constexpr Strip strip(const Box& box, const Dimensions& d = kRegular) {
  Strip s{};
  const Box inner{box.x + d.stripPadding, box.y + d.stripPadding, box.w - 2 * d.stripPadding,
                  box.h - 2 * d.stripPadding};
  const int slotW = (inner.w - (kStripBooks - 1) * d.gap) / kStripBooks;
  const int slotH = inner.h - d.stripLabelHeight;
  for (int i = 0; i < kStripBooks; ++i) {
    s.slots[i] = Box{inner.x + i * (slotW + d.gap), inner.y + d.stripLabelHeight, slotW, slotH};
    s.slotCovers[i] = Box{s.slots[i].x, s.slots[i].y, slotH * 3 / 5, slotH};
  }
  return s;
}

// ---- The widgets and the layouts made of them -----------------------------------------------------

enum class Widget : uint8_t { Book, AlsoReading, Shortcut, Clock, Calendar, Summary, Weather, Picture };

// A shortcut's destination (Cell::option), in the order of BaseTheme's HomeDestination.
namespace destination {
constexpr uint8_t Library = 0;
constexpr uint8_t ReadingStats = 1;
constexpr uint8_t Bookmarks = 2;
constexpr uint8_t FileTransfer = 3;
constexpr uint8_t Weather = 4;
constexpr uint8_t Recent = 5;  // the Library's Recent tab
constexpr uint8_t Count = 6;
}  // namespace destination

// The widgets that may share a row; the rest take the row across.
constexpr bool pairs(const Widget w) {
  switch (w) {
    case Widget::Shortcut:
    case Widget::Clock:
    case Widget::Calendar:
    case Widget::Summary:
    case Widget::Weather:
    case Widget::Picture:
      return true;
    case Widget::Book:
    case Widget::AlsoReading:
      return false;
  }
  return false;
}

// The widgets that open something when chosen: a shortcut its destination, the reading summary
// and the calendar Reading Stats, the weather Weather. The book card and Also reading are chosen
// as books; the clock and pictures are only looked at.
constexpr bool opensSomething(const Widget w) {
  return w == Widget::Shortcut || w == Widget::Summary || w == Widget::Calendar || w == Widget::Weather;
}

constexpr int kMaxRows = 6;
constexpr int kMaxPictures = 2;
constexpr int kMaxBottom = 3;

struct Cell {
  Widget widget = Widget::Book;
  // Book: 0 large, 1 compact. Shortcut: its destination (namespace destination). Picture: which of
  // the layout's pictures, 0 to kMaxPictures - 1. Others: unused.
  uint8_t option = 0;
};

struct Row {
  Cell cells[2]{};
  int count = 0;
};

struct Config {
  Row rows[kMaxRows]{};
  int rowCount = 0;

  constexpr Config& add(const Cell a) {
    if (rowCount < kMaxRows) {
      rows[rowCount].cells[0] = a;
      rows[rowCount].count = 1;
      ++rowCount;
    }
    return *this;
  }
  constexpr Config& add(const Cell a, const Cell b) {
    if (rowCount < kMaxRows) {
      rows[rowCount].cells[0] = a;
      rows[rowCount].cells[1] = b;
      rows[rowCount].count = 2;
      ++rowCount;
    }
    return *this;
  }
};

namespace presets {
// Lyra Tiles as it has been: the book card, Also reading where it fits, two rows of two tiles.
constexpr Config reader() {
  Config c;
  c.add({Widget::Book})
      .add({Widget::AlsoReading})
      .add({Widget::Shortcut, destination::Library}, {Widget::Shortcut, destination::ReadingStats})
      .add({Widget::Shortcut, destination::FileTransfer}, {Widget::Shortcut, destination::Bookmarks});
  return c;
}
// The book card, a clock beside a picture, and the week's reading beside the Library.
constexpr Config dashboard() {
  Config c;
  c.add({Widget::Book})
      .add({Widget::Clock}, {Widget::Picture, 0})
      .add({Widget::Summary}, {Widget::Shortcut, destination::Library});
  return c;
}
// A picture across the top, the compact book card, two tiles.
constexpr Config gallery() {
  Config c;
  c.add({Widget::Picture, 0})
      .add({Widget::Book, 1})
      .add({Widget::Shortcut, destination::Library}, {Widget::Shortcut, destination::FileTransfer});
  return c;
}
}  // namespace presets

// `cfg` without its shortcuts to destinations with nothing behind them (bit d of `available`: destination d
// has), as Home shows it: the neighbour of one left out takes its row, and a row left empty goes.
constexpr Config withAvailable(const Config& cfg, const uint32_t available) {
  Config shown;
  for (int r = 0; r < cfg.rowCount && r < kMaxRows; ++r) {
    Cell cells[2]{};
    int count = 0;
    for (int c = 0; c < cfg.rows[r].count; ++c) {
      const Cell& cell = cfg.rows[r].cells[c];
      if (cell.widget == Widget::Shortcut && (available & (1u << cell.option)) == 0) continue;
      cells[count++] = cell;
    }
    if (count == 1) shown.add(cells[0]);
    if (count == 2) shown.add(cells[0], cells[1]);
  }
  return shown;
}

// ---- Placement ----------------------------------------------------------------------------------

struct Placed {
  Cell cell;
  Box box;
  bool full = true;  // across the screen (or the column), not beside another widget
  int row = 0;       // the layout's row it came from
};

struct Placement {
  Placed items[kMaxRows * 2]{};  // in reading order: row by row, the left column first sideways
  int count = 0;
  Box bottom[kMaxBottom]{};  // the bottom row, in menu order
  int bottomCount = 0;
  int dropped = 0;  // bit r: the layout's row r did not fit and is left out
  bool sideways = false;

  constexpr const Placed* find(const Widget w) const {
    for (int i = 0; i < count; ++i) {
      if (items[i].cell.widget == w) return &items[i];
    }
    return nullptr;
  }
};

namespace detail {

constexpr int clampInt(const int v, const int lo, const int hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr int maxInt(const int a, const int b) { return a > b ? a : b; }

// Upright, a tile is 16 % of the content's height within the dimensions' bounds.
constexpr int tileHeight(const Box& content, const Dimensions& d) {
  return clampInt((content.h * 16 + 50) / 100, d.minTileHeight, d.maxTileHeight);
}

struct Need {
  int height = 0;  // the fixed height, or the floor of a row that takes what is left
  bool flex = false;
  bool optional = false;  // Also reading: left out quietly
  bool book = false;      // holds the book card: never left out
  bool tile = false;      // holds a shortcut tile
};

// What a row needs upright, its tiles `tileH` tall.
constexpr Need need(const Row& row, const int tileH, const Dimensions& d) {
  Need n{};
  const bool full = row.count == 1;
  int fixed = 0;
  int floor = 0;
  for (int c = 0; c < row.count; ++c) {
    const Cell& cell = row.cells[c];
    switch (cell.widget) {
      case Widget::Book:
        n.book = true;
        if (cell.option == 1) {
          fixed = maxInt(fixed, d.bookCompactHeight);
        } else {
          n.flex = true;
          floor = maxInt(floor, d.bookMinHeight);
        }
        break;
      case Widget::AlsoReading:
        n.optional = true;
        fixed = maxInt(fixed, d.stripHeight);
        break;
      case Widget::Shortcut:
        n.tile = true;
        fixed = maxInt(fixed, tileH);
        break;
      case Widget::Clock:
        fixed = maxInt(fixed, full ? d.clockFullHeight : d.clockHeight);
        break;
      case Widget::Calendar:
        fixed = maxInt(fixed, d.calendarHeight);
        break;
      case Widget::Summary:
        fixed = maxInt(fixed, full ? d.summaryFullHeight : d.summaryHeight);
        break;
      case Widget::Weather:
        fixed = maxInt(fixed, full ? d.weatherFullHeight : d.weatherHeight);
        break;
      case Widget::Picture:
        if (full) {
          n.flex = true;
          floor = maxInt(floor, d.pictureMinHeight);
        } else {
          fixed = maxInt(fixed, d.pictureHeight);
        }
        break;
    }
  }
  n.height = n.flex ? maxInt(floor, fixed) : fixed;
  return n;
}

// The heights of the rows marked `on`, stacked in `avail` with a gap between; rows that take what
// is left share it. 0 for rows that are off.
constexpr void stack(const Need* needs, const bool* on, const int count, const int avail, const Dimensions& d,
                     int* heights) {
  int fixed = 0;
  int flexCount = 0;
  int rows = 0;
  for (int r = 0; r < count; ++r) {
    heights[r] = 0;
    if (!on[r]) continue;
    ++rows;
    if (needs[r].flex) {
      ++flexCount;
    } else {
      fixed += needs[r].height;
    }
  }
  const int left = avail - fixed - (rows > 1 ? (rows - 1) * d.gap : 0);
  int given = 0;
  int seen = 0;
  for (int r = 0; r < count; ++r) {
    if (!on[r]) continue;
    if (needs[r].flex) {
      ++seen;
      // The last such row takes the remainder, so the rows end exactly where the room does.
      heights[r] = seen == flexCount ? left - given : left / flexCount;
      given += heights[r];
    } else {
      heights[r] = needs[r].height;
    }
  }
}

constexpr int needed(const Need* needs, const bool* on, const int count, const Dimensions& d) {
  int total = 0;
  int rows = 0;
  for (int r = 0; r < count; ++r) {
    if (!on[r]) continue;
    total += needs[r].height;
    ++rows;
  }
  return total + (rows > 1 ? (rows - 1) * d.gap : 0);
}

// Leave out rows until the rest fit `avail`: the Also reading row first (quietly), then from the
// bottom up, never a row that holds the book card.
constexpr void fit(const Need* needs, bool* on, const int count, const int avail, const Dimensions& d, int& dropped) {
  while (needed(needs, on, count, d) > avail) {
    int victim = -1;
    for (int r = count - 1; r >= 0; --r) {
      if (on[r] && needs[r].optional) {
        victim = r;
        break;
      }
    }
    const bool quiet = victim >= 0;
    if (victim < 0) {
      for (int r = count - 1; r >= 0; --r) {
        if (on[r] && !needs[r].book) {
          victim = r;
          break;
        }
      }
    }
    if (victim < 0) return;
    on[victim] = false;
    if (!quiet) dropped |= 1 << victim;
  }
}

// The Also reading row only where the large book card keeps minCardHeight beside it.
constexpr void keepStripOnlyBesideATallCard(const Row* rows, const Need* needs, bool* on, const int count,
                                            const int avail, const Dimensions& d) {
  int stripRow = -1;
  int bookRow = -1;
  for (int r = 0; r < count; ++r) {
    if (!on[r]) continue;
    if (needs[r].optional) stripRow = r;
    for (int c = 0; c < rows[r].count; ++c) {
      if (rows[r].cells[c].widget == Widget::Book && rows[r].cells[c].option != 1) bookRow = r;
    }
  }
  if (stripRow < 0) return;
  if (bookRow < 0) {
    on[stripRow] = false;
    return;
  }
  int heights[kMaxRows]{};
  stack(needs, on, count, avail, d, heights);
  if (heights[bookRow] < d.minCardHeight) on[stripRow] = false;
}

constexpr void placeBottom(Placement& p, const Box& area, const int count, const Dimensions& d) {
  p.bottomCount = clampInt(count, 1, kMaxBottom);
  const int y = area.y + area.h - d.bottomRowHeight;
  if (p.bottomCount == 1) {
    p.bottom[0] = Box{area.x, y, area.w, d.bottomRowHeight};
  } else if (p.bottomCount == 2) {
    const int halfW = (area.w - d.gap) / 2;
    p.bottom[0] = Box{area.x, y, halfW, d.bottomRowHeight};
    p.bottom[1] = Box{area.x + area.w - halfW, y, halfW, d.bottomRowHeight};
  } else {
    const int w = (area.w - 2 * d.gap) / 3;
    p.bottom[0] = Box{area.x, y, w, d.bottomRowHeight};
    p.bottom[1] = Box{area.x + (area.w - w) / 2, y, w, d.bottomRowHeight};
    p.bottom[2] = Box{area.x + area.w - w, y, w, d.bottomRowHeight};
  }
}

// Rows `on` stacked from `y` in the column at x, w; each row's widgets across or side by side.
constexpr void placeRows(Placement& p, const Config& cfg, const bool* on, const int* heights, const int x, const int w,
                         int y, const Dimensions& d) {
  for (int r = 0; r < cfg.rowCount && r < kMaxRows; ++r) {
    if (!on[r]) continue;
    const Row& row = cfg.rows[r];
    const int h = heights[r];
    if (row.count == 1) {
      p.items[p.count++] = Placed{row.cells[0], Box{x, y, w, h}, true, r};
    } else {
      const int halfW = (w - d.gap) / 2;
      p.items[p.count++] = Placed{row.cells[0], Box{x, y, halfW, h}, false, r};
      p.items[p.count++] = Placed{row.cells[1], Box{x + w - halfW, y, halfW, h}, false, r};
    }
    y += h + d.gap;
  }
}

constexpr bool holdsBook(const Row& row) {
  for (int c = 0; c < row.count; ++c) {
    if (row.cells[c].widget == Widget::Book || row.cells[c].widget == Widget::AlsoReading) return true;
  }
  return false;
}

}  // namespace detail

// `content` is the screen above the button hints; `top` is the first line under the status line.
// `bottomCount` is the entries in the bottom row (1 to kMaxBottom): More when anything is behind it,
// and Settings.
constexpr Placement place(const Box& content, const int top, const Config& cfg, const int bottomCount,
                          const Dimensions& d = kRegular) {
  using namespace detail;
  Placement p{};
  const int bottom = content.y + content.h - d.bottomPadding;
  const int count = cfg.rowCount < kMaxRows ? cfg.rowCount : kMaxRows;

  bool anyBookRow = false;
  for (int r = 0; r < count; ++r) anyBookRow = anyBookRow || holdsBook(cfg.rows[r]);

  if (!sideways(content) || !anyBookRow) {
    p.sideways = sideways(content);
    const int x = content.x + d.sidePadding;
    const int w = content.w - 2 * d.sidePadding;
    placeBottom(p, Box{x, top, w, bottom - top}, bottomCount, d);
    const int avail = (bottom - d.bottomRowHeight - d.gap) - top;
    const int tileH = tileHeight(content, d);
    Need needs[kMaxRows]{};
    bool on[kMaxRows]{};
    for (int r = 0; r < count; ++r) {
      needs[r] = need(cfg.rows[r], tileH, d);
      on[r] = true;
    }
    keepStripOnlyBesideATallCard(cfg.rows, needs, on, count, avail, d);
    fit(needs, on, count, avail, d, p.dropped);
    int heights[kMaxRows]{};
    stack(needs, on, count, avail, d, heights);
    placeRows(p, cfg, on, heights, x, w, top, d);
    return p;
  }

  // Sideways: the rows with the book card and Also reading in the left column, the rest and the
  // bottom row in the right one.
  p.sideways = true;
  const int leftX = content.x + d.sidePadding;
  const int leftW = (content.w * 56 + 50) / 100 - d.sidePadding;
  const int rightX = leftX + leftW + d.gap;
  const int rightW = content.x + content.w - d.sidePadding - rightX;
  placeBottom(p, Box{rightX, top, rightW, bottom - top}, bottomCount, d);

  Need needs[kMaxRows]{};
  bool left[kMaxRows]{};
  bool right[kMaxRows]{};
  int cap[kMaxRows]{};
  for (int r = 0; r < count; ++r) {
    needs[r] = need(cfg.rows[r], d.minTileHeight, d);
    // Sideways the rows of tiles share the right column's height, down to the smallest tiles and up
    // to maxSidewaysTileHeight; a widget beside a tile keeps at least its own height.
    if (needs[r].tile) {
      cap[r] = maxInt(needs[r].height, d.maxSidewaysTileHeight);
      needs[r].flex = true;
    }
    const bool inLeft = holdsBook(cfg.rows[r]);
    left[r] = inLeft;
    right[r] = !inLeft;
  }
  const int leftAvail = bottom - top;
  keepStripOnlyBesideATallCard(cfg.rows, needs, left, count, leftAvail, d);
  fit(needs, left, count, leftAvail, d, p.dropped);
  int leftHeights[kMaxRows]{};
  stack(needs, left, count, leftAvail, d, leftHeights);
  placeRows(p, cfg, left, leftHeights, leftX, leftW, top, d);

  const int rightAvail = (bottom - d.bottomRowHeight - d.gap) - top;
  fit(needs, right, count, rightAvail, d, p.dropped);
  int rightHeights[kMaxRows]{};
  stack(needs, right, count, rightAvail, d, rightHeights);
  for (int r = 0; r < count; ++r) {
    if (cap[r] > 0 && rightHeights[r] > cap[r]) rightHeights[r] = cap[r];
  }
  placeRows(p, cfg, right, rightHeights, rightX, rightW, top, d);
  return p;
}

// The widgets that open something, in reading order: Home numbers them after its menu.
constexpr int actions(const Placement& p, Widget* out, const int max) {
  int n = 0;
  for (int i = 0; i < p.count && n < max; ++i) {
    if (opensSomething(p.items[i].cell.widget)) out[n++] = p.items[i].cell.widget;
  }
  return n;
}

// The screen's rows for HomeGridNav, in reading order. Home's selector runs: the books it shows
// (the card 0, Also reading after it), the menu (the bottom row: More, Settings), then the widgets
// that open something, as actions() lists them.
constexpr HomeGridNav::Grid grid(const Placement& p, const int shownBooks, const int menuCount) {
  HomeGridNav::Grid g{};
  const int actionBase = shownBooks + menuCount;
  int action = 0;
  int i = 0;
  while (i < p.count) {
    const int row = p.items[i].row;
    int widgets[2]{};
    int widgetCount = 0;
    for (; i < p.count && p.items[i].row == row; ++i) {
      switch (p.items[i].cell.widget) {
        case Widget::Book:
          if (shownBooks > 0) {
            g.addRow(0, 1);
            g.markCycleRow();
          }
          break;
        case Widget::AlsoReading:
          if (shownBooks > 1) g.addRow(1, shownBooks - 1);
          break;
        case Widget::Clock:
        case Widget::Picture:
          break;
        case Widget::Shortcut:
        case Widget::Calendar:
        case Widget::Summary:
        case Widget::Weather:
          widgets[widgetCount++] = actionBase + action++;
          break;
      }
    }
    if (widgetCount > 0) g.addCells(widgets, widgetCount);
  }
  g.addRow(shownBooks, p.bottomCount);
  return g;
}

}  // namespace HomeTilesLayout
