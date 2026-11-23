#pragma once

#include "components/themes/lyra/LyraTheme.h"

// Lyra Tiles: Lyra's look, with a Home laid out for the book in front. A card for the book takes
// what the menu leaves (cover, title, author, series, history, progress), four labelled tiles take
// the destinations, and More and Settings sit in a lighter row along the bottom. A horizontal swipe
// brings the next recent book to the card. Geometry: HomeTilesLayout.h.
namespace LyraTilesMetrics {
constexpr ThemeMetrics make() {
  ThemeMetrics m = LyraMetrics::values;
  // The status line (clock, battery) is all the header Home draws; the card starts under it.
  m.homeTopPadding = 40;
  // The card and the Also reading row show four; the fifth is a swipe away.
  m.homeRecentBooksCount = 5;
  return m;
}
constexpr ThemeMetrics values = make();
}  // namespace LyraTilesMetrics

class LyraTilesTheme : public LyraTheme {
 public:
  HomeLayout homeLayout(const GfxRenderer& renderer, Rect content, int menuCount, int menuTrailing) const override;
  int homeMenuCapacity() const override;
  bool homeSwipeCyclesBooks() const override { return true; }

  // The card of the book in front, and the Also reading row under it when the layout has one.
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  // The destinations as tiles, then More and Settings in the bottom row.
  void drawHomeMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int trailingCount, int selectedIndex,
                    const std::function<std::string(int index)>& buttonLabel,
                    const std::function<UIIcon(int index)>& rowIcon) const override;
};
