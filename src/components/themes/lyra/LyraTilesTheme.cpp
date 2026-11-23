#include "LyraTilesTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/BookProgressPresentation.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/themes/CardTextFit.h"
#include "components/themes/TapTargets.h"
#include "components/themes/lyra/HomeTilesLayout.h"
#include "fontIds.h"

namespace {

using HomeTilesLayout::Box;

constexpr int kCornerRadius = 8;
// Selection is a heavier frame around the card or an Also reading book, and a solid black tile:
// no grey fills, which the panel can only dither.
constexpr int kCardFrameWidth = 1;
constexpr int kSelectedFrameWidth = 3;
constexpr int kSelectedFrameOutset = 3;
constexpr int kTileFrameWidth = 2;
constexpr int kBottomFrameWidth = 1;
constexpr int kIconSize = 32;
constexpr int kTileInset = 16;
constexpr int kIconToLabel = 10;
constexpr int kEyebrowGap = 6;
constexpr int kBarHeight = 10;
constexpr int kStripCoverToText = 8;
constexpr int kStripLabelToCovers = 4;

// Titles run long, and the card's column has the height for them: four lines of UI 12 bold, then
// five of UI 10 bold, before any word is cut.
constexpr CardTextFit::Face kTitleFaces[] = {{UI_12_FONT_ID, 4}, {UI_10_FONT_ID, 5}};

Box toBox(const Rect& r) { return Box{r.x, r.y, r.width, r.height}; }
Rect toRect(const Box& b) { return Rect{b.x, b.y, b.w, b.h}; }

// Whether Home has the Also reading row. homeLayout() decides it from the content HomeActivity
// passes, which is this rect; the card's draw gets only the books' rect, which a short menu makes
// taller, so it asks the panel again rather than judging by that height.
bool stripFitsHome(const GfxRenderer& renderer) {
  const Rect content = UITheme::getContentRect(renderer, true, false);
  return HomeTilesLayout::stripFits(toBox(content), content.y + UITheme::getInstance().getMetrics().homeTopPadding);
}

enum class Cover : uint8_t { Drawn, Pending, Absent };

// A book's thumbnail at `box`'s exact size, which the cover pass writes cropped to it
// (HomeLayout::thumbSizes). Pending while the pass has not written it yet.
Cover drawCoverThumb(const GfxRenderer& renderer, const RecentBook& book, const Box& box) {
  if (book.coverBmpPath.empty()) return Cover::Absent;
  FsFile file;
  if (!Storage.openFileForRead("HOME", UITheme::getCoverThumbPath(book.coverBmpPath, box.w, box.h), file)) {
    return Cover::Pending;
  }
  Bitmap bitmap(file);
  // Never draw the no-cover marker: scaling a 1x1 BMP into the box paints a solid block.
  if (bitmap.parseHeaders() != BmpReaderError::Ok ||
      UITheme::isCoverPlaceholderBmp(bitmap.getWidth(), bitmap.getHeight())) {
    return Cover::Absent;
  }
  renderer.fillRect(box.x, box.y, box.w, box.h, false);
  renderer.drawBitmap(bitmap, box.x, box.y, box.w, box.h);
  return Cover::Drawn;
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

HomeLayout LyraTilesTheme::homeLayout(const GfxRenderer& /*renderer*/, const Rect content, const int menuCount,
                                      const int menuTrailing) const {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int top = content.y + metrics.homeTopPadding;
  const auto regions = HomeTilesLayout::regions(toBox(content), top, menuCount - menuTrailing);
  const auto books = HomeTilesLayout::books(regions.books, HomeTilesLayout::stripFits(toBox(content), top));

  HomeLayout layout;
  layout.books = toRect(regions.books);
  layout.menu = toRect(regions.menu);
  // Only the card's cover is kept between renders. The text and frames are cheap to draw again,
  // and the whole card would cost the X3 three times the heap.
  layout.coverSnapshot = toRect(books.cover);
  layout.coverHeight = books.cover.h;
  layout.thumbSizes[0] = {books.cover.w, books.cover.h};
  layout.thumbSizeCount = 1;
  layout.bookSlots = 1;
  if (books.hasStrip) {
    layout.thumbSizes[1] = {books.slotCovers[0].w, books.slotCovers[0].h};
    layout.thumbSizeCount = 2;
    layout.bookSlots += HomeTilesLayout::kStripBooks;
  }
  return layout;
}

int LyraTilesTheme::homeMenuCapacity() const { return HomeTilesLayout::kMaxTiles; }

void LyraTilesTheme::drawRecentBookCover(GfxRenderer& renderer, const Rect rect,
                                         const std::vector<RecentBook>& recentBooks, const int selectorIndex,
                                         bool& coverRendered, bool& coverBufferStored, bool& /*bufferRestored*/,
                                         std::function<bool()> storeCoverBuffer) const {
  const HomeTilesLayout::Books layout = HomeTilesLayout::books(toBox(rect), stripFitsHome(renderer));
  const int bookCount = static_cast<int>(recentBooks.size());
  const int stripBooks = layout.hasStrip ? std::min(HomeTilesLayout::kStripBooks, std::max(0, bookCount - 1)) : 0;
  const Box& card = layout.card;

  // Published ahead of the draw, so a repaint from the stored cover records them too. Book values
  // are positions in recentBooks: the card is 0, the Also reading books follow it.
  {
    TapTargets::Recorder::Builder targets;
    if (bookCount > 0) targets.add(card.x, card.y, card.w, card.h, 0);
    for (int i = 0; i < stripBooks; ++i) {
      targets.add(layout.slots[i].x, layout.slots[i].y, layout.slots[i].w, layout.slots[i].h, i + 1);
    }
    TapTargets::homeCovers().record(targets);
  }

  if (bookCount == 0) {
    renderer.drawRoundedRect(card.x, card.y, card.w, card.h, kCardFrameWidth, kCornerRadius, true);
    drawEmptyRecents(renderer, toRect(card));
    return;
  }

  const bool cardSelected = selectorIndex == 0;
  renderer.drawRoundedRect(card.x, card.y, card.w, card.h, cardSelected ? kSelectedFrameWidth : kCardFrameWidth,
                           kCornerRadius, true);

  // --- The cover: drawn once, then restored by Home from its copy (HomeLayout::coverSnapshot) ---
  const RecentBook& book = recentBooks[0];
  const Box& cover = layout.cover;
  if (!coverRendered) {
    const Cover drawn = drawCoverThumb(renderer, book, cover);
    if (drawn == Cover::Pending) {
      const char* loading = tr(STR_LOADING);
      renderer.drawText(SMALL_FONT_ID, cover.x + (cover.w - renderer.getTextWidth(SMALL_FONT_ID, loading)) / 2,
                        cover.y + (cover.h - renderer.getLineHeight(SMALL_FONT_ID)) / 2, loading, true);
    } else if (drawn == Cover::Absent) {
      renderer.fillRect(cover.x, cover.y + cover.h / 3, cover.w, 2 * cover.h / 3, true);
      renderer.drawIcon(CoverIcon, cover.x + 24, cover.y + 24, 32, 32);
    }
    renderer.drawRect(cover.x, cover.y, cover.w, cover.h, true);
    // Kept only once the cover is settled; while its thumbnail is being made the next render retries.
    if (drawn != Cover::Pending) {
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

  // --- Also reading: the books after the one on the card ---
  if (stripBooks == 0) return;
  // Set on the covers' top rather than on the row's, so it clears them at any UI font size.
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawText(
      SMALL_FONT_ID, layout.strip.x, layout.slots[0].y - kStripLabelToCovers - smallLine,
      renderer.truncatedText(SMALL_FONT_ID, tr(STR_ALSO_READING), layout.strip.w, EpdFontFamily::BOLD).c_str(), true,
      EpdFontFamily::BOLD);
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
                                  const int trailingCount, const int selectedIndex,
                                  const std::function<std::string(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  const int tileCount = std::max(0, buttonCount - trailingCount);
  const HomeTilesLayout::Menu menu = HomeTilesLayout::menu(toBox(rect), tileCount, trailingCount);

  // Targets carry MENU-LOCAL indices, as every theme's menu does: tiles first, then the bottom row.
  TapTargets::Recorder::Builder targets;
  const auto iconFor = [&](const int index) { return rowIcon ? iconForName(rowIcon(index), kIconSize) : nullptr; };

  for (int i = 0; i < menu.tileCount; ++i) {
    const Box& tile = menu.tiles[i];
    targets.add(tile.x, tile.y, tile.w, tile.h, i);
    const bool selected = i == selectedIndex;
    if (selected) {
      renderer.fillRoundedRect(tile.x, tile.y, tile.w, tile.h, kCornerRadius, Color::Black);
    } else {
      renderer.drawRoundedRect(tile.x, tile.y, tile.w, tile.h, kTileFrameWidth, kCornerRadius, true);
    }
    if (const uint8_t* icon = iconFor(i)) {
      if (selected) {
        renderer.drawIconInverted(icon, tile.x + kTileInset, tile.y + kTileInset, kIconSize, kIconSize);
      } else {
        renderer.drawIcon(icon, tile.x + kTileInset, tile.y + kTileInset, kIconSize, kIconSize);
      }
    }
    const std::string label = buttonLabel(i);
    const int labelY = tile.y + tile.h - kTileInset - renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(
        UI_12_FONT_ID, tile.x + kTileInset, labelY,
        renderer.truncatedText(UI_12_FONT_ID, label.c_str(), tile.w - 2 * kTileInset, EpdFontFamily::BOLD).c_str(),
        !selected, EpdFontFamily::BOLD);
  }

  // More and Settings: as easy to hit as the rows they replace, lighter to look at.
  const int bottomCount = std::min(menu.bottomCount, std::max(0, buttonCount - tileCount));
  for (int j = 0; j < bottomCount; ++j) {
    const int index = tileCount + j;
    const Box& box = menu.bottom[j];
    targets.add(box.x, box.y, box.w, box.h, index);
    const bool selected = index == selectedIndex;
    if (selected) {
      renderer.fillRoundedRect(box.x, box.y, box.w, box.h, kCornerRadius, Color::Black);
    } else {
      renderer.drawRoundedRect(box.x, box.y, box.w, box.h, kBottomFrameWidth, kCornerRadius, true);
    }
    const std::string label = buttonLabel(index);
    const uint8_t* icon = iconFor(index);
    const int iconRoom = icon ? kIconSize + kIconToLabel : 0;
    const std::string fitted =
        renderer.truncatedText(UI_10_FONT_ID, label.c_str(), std::max(0, box.w - 2 * kTileInset - iconRoom));
    const int groupX = box.x + (box.w - iconRoom - renderer.getTextWidth(UI_10_FONT_ID, fitted.c_str())) / 2;
    if (icon) {
      const int iconY = box.y + (box.h - kIconSize) / 2;
      if (selected) {
        renderer.drawIconInverted(icon, groupX, iconY, kIconSize, kIconSize);
      } else {
        renderer.drawIcon(icon, groupX, iconY, kIconSize, kIconSize);
      }
    }
    renderer.drawText(UI_10_FONT_ID, groupX + iconRoom, box.y + (box.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      fitted.c_str(), !selected);
  }

  TapTargets::homeMenu().record(targets);
}
