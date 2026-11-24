#include "LyraTilesTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/BookProgressPresentation.h"
#include "components/UITheme.h"
#include "components/themes/CardTextFit.h"
#include "components/themes/TapTargets.h"
#include "components/themes/lyra/HomeTilesLayout.h"
#include "fontIds.h"

namespace {

using HomeTilesLayout::Box;

using PanelMetrics::capped;  // the compact panel's size where it is smaller (PanelMetrics.h)

constexpr int kCornerRadius = capped(8, 6);
// Selection is a heavier frame around the card or an Also reading book, and a solid black tile:
// no grey fills, which the panel can only dither.
// One step between a frame and a selected one, whatever the panel: the card cannot show its
// selection inverted, as a tile does, without hiding its cover. The compact panel's 240 px take a
// step lighter.
constexpr int kCardFrameWidth = 1;
constexpr int kSelectedFrameWidth = capped(3, 2);
constexpr int kSelectedFrameOutset = 3;
constexpr int kTileFrameWidth = capped(2, 1);
constexpr int kBottomFrameWidth = 1;
constexpr int kIconSize = 32;  // the icons' own size
constexpr int kBottomIconSize = capped(32, 20);
// The tiles' icons, drawn up from the 32 px set (drawIconScaled), with the label beside them where at
// least kMinBesideLabelWidth is left for it.
constexpr int kTileIconSize = capped(48, 20);
constexpr int kTileIconToLabel = capped(14, 4);
constexpr int kMinBesideLabelWidth = capped(96, 60);
constexpr int kTileInset = capped(16, 6);
constexpr int kIconToLabel = capped(10, 6);
constexpr int kEyebrowGap = capped(6, 2);
constexpr int kBarHeight = capped(10, 6);
constexpr int kStripCoverToText = 8;
constexpr int kStripLabelToCovers = 4;

// Titles run long, and the card's column has the height for them: four lines of UI 12 bold, then
// five of UI 10 bold, before any word is cut.
constexpr CardTextFit::Face kTitleFaces[] = {{UI_12_FONT_ID, 4}, {UI_10_FONT_ID, 5}};

// The geometry's sizes for this panel (HomeTilesLayout::Dimensions).
const HomeTilesLayout::Dimensions& dims() {
  return PanelMetrics::COMPACT ? HomeTilesLayout::kCompact : HomeTilesLayout::kRegular;
}

Box toBox(const Rect& r) { return Box{r.x, r.y, r.width, r.height}; }
Rect toRect(const Box& b) { return Rect{b.x, b.y, b.w, b.h}; }

// A book's thumbnail at `box`'s exact size, which the cover pass writes cropped to it
// (HomeLayout::thumbSizes).
BaseTheme::CoverThumb drawCoverThumb(const GfxRenderer& renderer, const RecentBook& book, const Box& box) {
  return BaseTheme::withCoverThumb(UITheme::getCoverThumbPath(book.coverBmpPath, box.w, box.h), [&](Bitmap& bitmap) {
    renderer.fillRect(box.x, box.y, box.w, box.h, false);
    renderer.drawBitmap(bitmap, box.x, box.y, box.w, box.h);
  });
}

// A 32 px icon at `size` px. Each target pixel takes the ink under its centre, bilinear over the
// four nearest source pixels and set where they are at least half ink, which keeps the 2 px strokes
// even at a scale that is not a whole number. Integer maths: the C3 has no FPU. The icons are stored
// for the upright frame drawIcon() writes them to: logical (u, v) is source row 31 - u, bit v, and a
// 0 bit is ink.
void drawIconScaled(const GfxRenderer& renderer, const uint8_t* icon, const int x, const int y, const int size,
                    const bool ink) {
  constexpr int kSource = kIconSize;
  constexpr int kStride = kSource / 8;
  constexpr int kOne = 256;  // fixed point: 1/256 of a source pixel
  const auto inkAt = [icon](const int u, const int v) {
    if (u < 0 || v < 0 || u >= kSource || v >= kSource) return 0;
    const int row = kSource - 1 - u;
    return ((icon[row * kStride + v / 8] >> (7 - v % 8)) & 1) == 0 ? 1 : 0;
  };
  // The source coordinate under a target pixel's centre, less half a pixel, as a whole part and a fraction.
  const auto locate = [size](const int d, int& whole, int& fraction) {
    const int at = (2 * d + 1) * kSource * kOne / (2 * size) - kOne / 2;
    whole = at >= 0 ? at / kOne : -1;
    fraction = at - whole * kOne;
  };
  for (int dy = 0; dy < size; ++dy) {
    int v0 = 0;
    int fv = 0;
    locate(dy, v0, fv);
    for (int dx = 0; dx < size; ++dx) {
      int u0 = 0;
      int fu = 0;
      locate(dx, u0, fu);
      const int weight = (kOne - fu) * (kOne - fv) * inkAt(u0, v0) + fu * (kOne - fv) * inkAt(u0 + 1, v0) +
                         (kOne - fu) * fv * inkAt(u0, v0 + 1) + fu * fv * inkAt(u0 + 1, v0 + 1);
      if (2 * weight >= kOne * kOne) renderer.drawPixel(x + dx, y + dy, ink);
    }
  }
}

// A tile's icon and label. Where the tile is wide enough the label stands beside a large icon and
// wraps over as many lines as the tile holds; a narrow tile (sideways) puts the label under it.
// The label steps down from UI 12 to UI 10, then to the small face, before a word is cut: a long
// word ("Bookmarks" on the compact panel, many translations) fits that way.
void drawTileContent(const GfxRenderer& renderer, const Box& tile, const uint8_t* icon, const std::string& label,
                     const bool selected) {
  const bool ink = !selected;
  const int besideW = tile.w - 2 * kTileInset - kTileIconSize - kTileIconToLabel;
  const bool beside = besideW >= kMinBesideLabelWidth;
  if (icon) {
    const int iconY = beside ? tile.y + (tile.h - kTileIconSize) / 2 : tile.y + kTileInset;
    drawIconScaled(renderer, icon, tile.x + kTileInset, iconY, kTileIconSize, ink);
  }

  const int textX = beside ? tile.x + kTileInset + kTileIconSize + kTileIconToLabel : tile.x + kTileInset;
  const int textW = beside ? besideW : tile.w - 2 * kTileInset;
  const int textTop = beside ? tile.y + kTileInset : tile.y + kTileInset + kTileIconSize + kTileIconToLabel;
  const int textH = tile.y + tile.h - kTileInset - textTop;
  std::vector<std::string> lines;
  int fontId = UI_12_FONT_ID;
  for (const int face : {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID}) {
    fontId = face;
    const int maxLines = std::max(1, textH / renderer.getLineHeight(face));
    bool whole = false;
    lines = renderer.wrappedText(face, label.c_str(), textW, maxLines, EpdFontFamily::BOLD, &whole);
    if (whole) break;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  const int blockH = static_cast<int>(lines.size()) * lineHeight;
  // Beside the icon the label is centred on it; under it, it sits at the tile's foot.
  int y = beside ? tile.y + (tile.h - blockH) / 2 : tile.y + tile.h - kTileInset - blockH;
  for (const auto& line : lines) {
    renderer.drawText(fontId, textX, y, line.c_str(), ink, EpdFontFamily::BOLD);
    y += lineHeight;
  }
}

void drawLines(const GfxRenderer& renderer, const int fontId, const int x, int y, const std::vector<std::string>& lines,
               const EpdFontFamily::Style style) {
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    renderer.drawText(fontId, x, y, line.c_str(), true, style);
    y += lineHeight;
  }
}

}  // namespace

using HomeTilesLayout::Widget;

HomeTilesLayout::Placement LyraTilesTheme::placement(const Rect content, const int menuCount) const {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  // Home's whole menu is the bottom row: More when anything is behind it, and Settings.
  return HomeTilesLayout::place(toBox(content), content.y + metrics.homeTopPadding, config_, std::max(1, menuCount),
                                dims());
}

HomeTilesLayout::Placement LyraTilesTheme::placement(const GfxRenderer& renderer) const {
  return placement(UITheme::getContentRect(renderer, true, false), menuCount_);
}

void LyraTilesTheme::prepareHome(const uint32_t available) {
  // A shortcut to a destination with nothing behind it (no bookmarks, weather not set up) is left
  // out, as its menu entry would be; its neighbour then takes the row.
  const HomeTilesLayout::Config layout = HomeTilesLayout::presets::reader();
  HomeTilesLayout::Config shown;
  for (int r = 0; r < layout.rowCount; ++r) {
    HomeTilesLayout::Cell cells[2]{};
    int count = 0;
    for (int c = 0; c < layout.rows[r].count; ++c) {
      const HomeTilesLayout::Cell& cell = layout.rows[r].cells[c];
      if (cell.widget == Widget::Shortcut && (available & (1u << cell.option)) == 0) continue;
      cells[count++] = cell;
    }
    if (count == 1) shown.add(cells[0]);
    if (count == 2) shown.add(cells[0], cells[1]);
  }
  config_ = shown;
}

uint32_t LyraTilesTheme::homeShownDestinations(const GfxRenderer& /*renderer*/, const Rect content) const {
  const HomeTilesLayout::Placement p = placement(content, 2);
  uint32_t mask = 0;
  for (int i = 0; i < p.count; ++i) {
    if (p.items[i].cell.widget == Widget::Shortcut) mask |= 1u << p.items[i].cell.option;
  }
  return mask;
}

void LyraTilesTheme::setHomeMenu(const int menuCount, const int menuTrailing) {
  menuCount_ = menuCount;
  menuTrailing_ = menuTrailing;
}

HomeLayout LyraTilesTheme::homeLayout(const GfxRenderer& /*renderer*/, const Rect content, const int menuCount,
                                      const int menuTrailing) const {
  (void)menuTrailing;
  const HomeTilesLayout::Placement p = placement(content, menuCount);
  HomeLayout layout;
  // Both draw calls place the widgets from the content again, with the same inputs.
  layout.books = content;
  layout.menu = content;
  layout.bookSlots = 0;
  if (const HomeTilesLayout::Placed* book = p.find(Widget::Book)) {
    const HomeTilesLayout::Card card = HomeTilesLayout::card(book->box, dims());
    // Only the card's cover is kept between renders. The text and frames are cheap to draw again,
    // and the whole card would cost the X3 three times the heap.
    layout.coverSnapshot = toRect(card.cover);
    layout.coverHeight = card.cover.h;
    layout.thumbSizes[0] = {card.cover.w, card.cover.h};
    layout.thumbSizeCount = 1;
    layout.bookSlots = 1;
    if (const HomeTilesLayout::Placed* also = p.find(Widget::AlsoReading)) {
      const HomeTilesLayout::Strip strip = HomeTilesLayout::strip(also->box, dims());
      layout.thumbSizes[1] = {strip.slotCovers[0].w, strip.slotCovers[0].h};
      layout.thumbSizeCount = 2;
      layout.bookSlots += HomeTilesLayout::kStripBooks;
    }
  }
  return layout;
}

// No places for menu entries: the layout's shortcuts are widgets, and every destination it does not
// show is behind More (homeShownDestinations).
int LyraTilesTheme::homeMenuCapacity() const { return 0; }

namespace {
// What a widget that opens something opens.
HomeDestination destinationOf(const HomeTilesLayout::Cell& cell) {
  switch (cell.widget) {
    case Widget::Shortcut:
      return static_cast<HomeDestination>(cell.option);
    case Widget::Weather:
      return HomeDestination::Weather;
    case Widget::Summary:
    case Widget::Calendar:
    case Widget::Book:
    case Widget::AlsoReading:
    case Widget::Clock:
    case Widget::Picture:
      return HomeDestination::ReadingStats;
  }
  return HomeDestination::ReadingStats;
}

struct DestinationLook {
  StrId label;
  UIIcon icon;
};
// In HomeDestination's order, as the Home menu labels and draws them (HomeMenu.cpp).
constexpr DestinationLook kDestinations[] = {
    {StrId::STR_LIBRARY, Folder},         {StrId::STR_READING_STATS, Stats}, {StrId::STR_GLOBAL_BOOKMARKS, Book},
    {StrId::STR_FILE_TRANSFER, Transfer}, {StrId::STR_WEATHER, Weather},
};
static_assert(std::size(kDestinations) == static_cast<size_t>(HomeDestination::Count));
static_assert(HomeTilesLayout::destination::Count == static_cast<uint8_t>(HomeDestination::Count));
static_assert(HomeTilesLayout::destination::Weather == static_cast<uint8_t>(HomeDestination::Weather));
}  // namespace

int LyraTilesTheme::homeWidgetActions(const GfxRenderer& /*renderer*/, const Rect content, HomeDestination* out,
                                      const int max) const {
  const HomeTilesLayout::Placement p = placement(content, menuCount_);
  int n = 0;
  for (int i = 0; i < p.count && n < max; ++i) {
    if (HomeTilesLayout::opensSomething(p.items[i].cell.widget)) out[n++] = destinationOf(p.items[i].cell);
  }
  return n;
}

HomeGridNav::Grid LyraTilesTheme::homeGrid(const GfxRenderer& /*renderer*/, const Rect content, const int shownBooks,
                                           const int menuCount, const int menuTrailing) const {
  (void)menuTrailing;
  return HomeTilesLayout::grid(placement(content, menuCount), shownBooks, menuCount);
}

void LyraTilesTheme::drawRecentBookCover(GfxRenderer& renderer, const Rect /*rect*/,
                                         const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                         bool& coverRendered, bool& coverBufferStored, bool& /*bufferRestored*/,
                                         std::function<bool()> storeCoverBuffer) const {
  const HomeTilesLayout::Placement p = placement(renderer);
  const HomeTilesLayout::Placed* book = p.find(Widget::Book);
  const HomeTilesLayout::Placed* also = book ? p.find(Widget::AlsoReading) : nullptr;
  const int bookCount = static_cast<int>(recentBooks.size());
  const int stripBooks = also ? std::min(HomeTilesLayout::kStripBooks, std::max(0, bookCount - 1)) : 0;

  // Published ahead of the draw, so a repaint from the stored cover records them too. Book values
  // are positions in recentBooks: the card is 0, the Also reading books follow it.
  {
    TapTargets::Recorder::Builder targets;
    if (book && bookCount > 0) targets.add(book->box.x, book->box.y, book->box.w, book->box.h, 0);
    if (also) {
      const HomeTilesLayout::Strip strip = HomeTilesLayout::strip(also->box, dims());
      for (int i = 0; i < stripBooks; ++i) {
        targets.add(strip.slots[i].x, strip.slots[i].y, strip.slots[i].w, strip.slots[i].h, i + 1);
      }
    }
    TapTargets::homeCovers().record(targets);
  }

  if (book)
    drawBookCard(renderer, book->box, recentBooks, selectorIndex, coverRendered, coverBufferStored, storeCoverBuffer);
  if (also && stripBooks > 0) drawAlsoReading(renderer, also->box, recentBooks, stripBooks, selectorIndex);
}

void LyraTilesTheme::drawBookCard(GfxRenderer& renderer, const Box& card, const std::vector<RecentBook>& recentBooks,
                                  const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                  const std::function<bool()>& storeCoverBuffer) const {
  if (recentBooks.empty()) {
    renderer.drawRoundedRect(card.x, card.y, card.w, card.h, kCardFrameWidth, kCornerRadius, true);
    drawEmptyRecents(renderer, toRect(card));
    return;
  }
  const HomeTilesLayout::Card layout = HomeTilesLayout::card(card, dims());

  const bool cardSelected = selectorIndex == 0;
  renderer.drawRoundedRect(card.x, card.y, card.w, card.h, cardSelected ? kSelectedFrameWidth : kCardFrameWidth,
                           kCornerRadius, true);

  // --- The cover: drawn once, then restored by Home from its copy (HomeLayout::coverSnapshot) ---
  const RecentBook& book = recentBooks[0];
  const Box& cover = layout.cover;
  if (!coverRendered) {
    const CoverThumb drawn = drawCoverThumb(renderer, book, cover);
    drawCoverStandIn(renderer, toRect(cover), drawn);
    renderer.drawRect(cover.x, cover.y, cover.w, cover.h, true);
    // Kept only once the cover is settled; while its thumbnail is being made the next render retries.
    if (drawn != CoverThumb::Pending) {
      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;
    }
  }

  // --- The text column: what the card is for, then the book's details ---
  const Box& column = layout.text;
  const int eyebrowHeight = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(
      SMALL_FONT_ID, column.x, column.y,
      renderer.truncatedText(SMALL_FONT_ID, tr(STR_CONTINUE_READING), column.w, EpdFontFamily::BOLD).c_str(), true,
      EpdFontFamily::BOLD);
  const int blockTop = column.y + eyebrowHeight + kEyebrowGap;
  const int blockHeight = column.y + column.h - blockTop;
  // The status line has a place of its own at the card's foot, so the column fits it out.
  const CardText text = fitCardText(renderer, book, std::string(), column.w, blockHeight, kTitleFaces,
                                    static_cast<int>(std::size(kTitleFaces)));
  for (const CardTextFit::PartId part : {CardTextFit::Title, CardTextFit::Author, CardTextFit::Series}) {
    drawCardTextPart(renderer, text, part, column.x, blockTop + text.fit.tops[part], column.w);
  }
  // The history closes the column, level with the cover's foot. The fit made room for it, so
  // moving it down cannot reach the series above.
  const int historyLines = static_cast<int>(text.lines[CardTextFit::History].size());
  if (historyLines > 0) {
    drawCardTextPart(renderer, text, CardTextFit::History, column.x,
                     column.y + column.h - historyLines * text.parts[CardTextFit::History].lineHeight, column.w);
  }

  // --- The foot: progress and pace, and the bar ---
  const int progressPercent = getRecentBookProgressPercent(book);
  const std::string status = BookProgressPresentation::formatStatus(book, progressPercent);
  if (!status.empty()) {
    const Box& foot = layout.foot;
    const int barY = foot.y + foot.h - kBarHeight;
    renderer.drawText(UI_10_FONT_ID, foot.x, barY - 2 - renderer.getLineHeight(UI_10_FONT_ID),
                      renderer.truncatedText(UI_10_FONT_ID, status.c_str(), foot.w, EpdFontFamily::BOLD).c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawRoundedRect(foot.x, barY, foot.w, kBarHeight, 1, kBarHeight / 2, true);
    const int filled = foot.w * std::clamp(progressPercent, 0, 100) / 100;
    if (filled >= kBarHeight) {
      renderer.fillRoundedRect(foot.x, barY, filled, kBarHeight, kBarHeight / 2, Color::Black);
    } else if (filled > 0) {
      renderer.fillRect(foot.x, barY, filled, kBarHeight, true);
    }
  }
}

void LyraTilesTheme::drawAlsoReading(GfxRenderer& renderer, const Box& row, const std::vector<RecentBook>& recentBooks,
                                     const int stripBooks, const int selectorIndex) const {
  const HomeTilesLayout::Strip layout = HomeTilesLayout::strip(row, dims());
  // Framed like the card above it and the tiles below.
  renderer.drawRoundedRect(row.x, row.y, row.w, row.h, kCardFrameWidth, kCornerRadius, true);
  // Set on the covers' top rather than on the row's, so it clears them at any UI font size.
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int labelX = layout.slots[0].x;
  const int labelW =
      layout.slots[HomeTilesLayout::kStripBooks - 1].x + layout.slots[HomeTilesLayout::kStripBooks - 1].w - labelX;
  renderer.drawText(SMALL_FONT_ID, labelX, layout.slots[0].y - kStripLabelToCovers - smallLine,
                    renderer.truncatedText(SMALL_FONT_ID, tr(STR_ALSO_READING), labelW, EpdFontFamily::BOLD).c_str(),
                    true, EpdFontFamily::BOLD);
  for (int i = 0; i < stripBooks; ++i) {
    const RecentBook& other = recentBooks[i + 1];
    const Box& slot = layout.slots[i];
    const Box& slotCover = layout.slotCovers[i];
    if (selectorIndex == i + 1) {
      renderer.drawRoundedRect(slot.x - kSelectedFrameOutset, slot.y - kSelectedFrameOutset,
                               slot.w + 2 * kSelectedFrameOutset, slot.h + 2 * kSelectedFrameOutset,
                               kSelectedFrameWidth, kCornerRadius, true);
    }
    drawCoverThumb(renderer, other, slotCover);
    renderer.drawRect(slotCover.x, slotCover.y, slotCover.w, slotCover.h, true);
    BookProgressPresentation::drawIndicator(renderer, toRect(slotCover), getRecentBookProgressPercent(other));

    const int textX = slotCover.x + slotCover.w + kStripCoverToText;
    const int textW = slot.x + slot.w - textX;
    if (textW <= 0) continue;
    const int maxLines = slot.h / smallLine;
    const auto title =
        renderer.wrappedText(SMALL_FONT_ID, other.title.c_str(), textW, std::min(3, maxLines), EpdFontFamily::BOLD);
    drawLines(renderer, SMALL_FONT_ID, textX, slot.y, title, EpdFontFamily::BOLD);
    const int authorLines = std::min(2, maxLines - static_cast<int>(title.size()));
    if (authorLines > 0) {
      drawLines(renderer, SMALL_FONT_ID, textX, slot.y + static_cast<int>(title.size()) * smallLine,
                renderer.wrappedText(SMALL_FONT_ID, other.author.c_str(), textW, authorLines), EpdFontFamily::REGULAR);
    }
  }
}

void LyraTilesTheme::drawHomeMenu(GfxRenderer& renderer, const Rect rect, const int buttonCount,
                                  const int /*trailingCount*/, const int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const HomeTilesLayout::Placement p = placement(rect, buttonCount);

  // Targets carry MENU-LOCAL indices, as every theme's menu does: the bottom row's entries, then the
  // widgets that open something, numbered on after them (homeWidgetActions).
  TapTargets::Recorder::Builder targets;
  const auto iconFor = [&](const int index) { return rowIcon ? iconForName(rowIcon(index), kIconSize) : nullptr; };

  // More and Settings: as easy to hit as the rows they replace, lighter to look at.
  const int bottomCount = std::min(p.bottomCount, buttonCount);
  for (int j = 0; j < bottomCount; ++j) {
    const Box& box = p.bottom[j];
    targets.add(box.x, box.y, box.w, box.h, j);
    const bool selected = j == selectedIndex;
    if (selected) {
      renderer.fillRoundedRect(box.x, box.y, box.w, box.h, kCornerRadius, Color::Black);
    } else {
      renderer.drawRoundedRect(box.x, box.y, box.w, box.h, kBottomFrameWidth, kCornerRadius, true);
    }
    const std::string label = buttonLabel(j);
    const uint8_t* icon = iconFor(j);
    const int iconRoom = icon ? kBottomIconSize + kIconToLabel : 0;
    const std::string fitted =
        renderer.truncatedText(UI_10_FONT_ID, label.c_str(), std::max(0, box.w - 2 * kTileInset - iconRoom));
    const int groupX = box.x + (box.w - iconRoom - renderer.getTextWidth(UI_10_FONT_ID, fitted.c_str())) / 2;
    if (icon) {
      drawIconScaled(renderer, icon, groupX, box.y + (box.h - kBottomIconSize) / 2, kBottomIconSize, !selected);
    }
    renderer.drawText(UI_10_FONT_ID, groupX + iconRoom, box.y + (box.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      fitted.c_str(), !selected);
  }

  int action = 0;
  for (int i = 0; i < p.count; ++i) {
    const HomeTilesLayout::Placed& item = p.items[i];
    if (!HomeTilesLayout::opensSomething(item.cell.widget)) continue;
    const int index = buttonCount + action++;
    const Box& box = item.box;
    targets.add(box.x, box.y, box.w, box.h, index);
    const bool selected = index == selectedIndex;
    switch (item.cell.widget) {
      case Widget::Shortcut: {
        const DestinationLook& look = kDestinations[item.cell.option < std::size(kDestinations) ? item.cell.option : 0];
        if (selected) {
          renderer.fillRoundedRect(box.x, box.y, box.w, box.h, kCornerRadius, Color::Black);
        } else {
          renderer.drawRoundedRect(box.x, box.y, box.w, box.h, kTileFrameWidth, kCornerRadius, true);
        }
        drawTileContent(renderer, box, iconForName(look.icon, kIconSize), I18N.get(look.label), selected);
        break;
      }
      case Widget::Summary:
      case Widget::Calendar:
      case Widget::Weather:
      case Widget::Book:
      case Widget::AlsoReading:
      case Widget::Clock:
      case Widget::Picture:
        break;
    }
  }

  TapTargets::homeMenu().record(targets);
}
