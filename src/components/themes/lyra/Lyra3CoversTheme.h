

#pragma once

#include <algorithm>

#include "CrossPointSettings.h"
#include "UiFontScale.h"
#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace Lyra3CoversMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 40,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 300,
                                 .homeRecentBooksCount = 3,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90};

// How the height of a tile is divided between the cover and the text under it.
//
// This lives in the header because TWO places need the same answer: the theme, which draws the
// cover, and HomeActivity, which generates the cover thumbnail. The generated thumbnail's HEIGHT
// is part of its FILENAME, so if the two ever compute it differently the theme asks the SD card
// for a file nobody writes and the tile reads "Loading..." for ever. That is not hypothetical --
// it is what happened when the theme started sizing covers dynamically and HomeActivity kept
// subtracting a flat 58. Change the arithmetic here and both sides follow.
constexpr int titleLineBudget = 3;        // wrapped title lines the text block is sized to hold
constexpr int historyRowHeight = 23;      // FIT_SMALL_FONT_ID line height; deliberately does not scale
constexpr int textBlockPadding = 5;       // below the cover, above the title
constexpr int textBlockBottomMargin = 3;  // clearance so the last line clears the tile edge
constexpr int tileTopPadding = 8;         // matches hPaddingInSelection in the theme
constexpr int minCoverHeight = 80;

// Title lines grow with the UI font size, so the block under the cover has to as well. The
// history row does not: it is set in a face that ignores the setting.
inline int textBlockHeight() {
  const int titleLineHeight = UiFontLadder::STEPS[0].small + UiFontLadder::growthAt(SETTINGS.uiFontSize).small;
  return titleLineBudget * titleLineHeight + historyRowHeight + textBlockPadding + textBlockBottomMargin;
}

// Whatever the text block does not need, the cover gets.
inline int coverRenderHeight(int recentTileHeight) {
  return std::max(minCoverHeight, recentTileHeight - tileTopPadding - textBlockHeight());
}
}  // namespace Lyra3CoversMetrics

class Lyra3CoversTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
};
