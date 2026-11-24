#pragma once

#include <array>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "components/themes/CardTextFit.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = forPanel({.batteryWidth = 16,
                                          .batteryHeight = 12,
                                          .topPadding = 5,
                                          .batteryBarHeight = 40,
                                          .headerHeight = 84,
                                          .verticalSpacing = 16,
                                          .contentSidePadding = 20,
                                          .listRowHeight = 40,
                                          .listWithSubtitleRowHeight = 68,
                                          .menuRowHeight = 64,
                                          .menuSpacing = 8,
                                          .tabSpacing = 8,
                                          .tabBarHeight = 40,
                                          .scrollBarWidth = 4,
                                          .scrollBarRightOffset = 5,
                                          .homeTopPadding = 56,
                                          .homeCoverHeight = 226,
                                          .homeCoverTileHeight = 242,
                                          .homeRecentBooksCount = 1,
                                          .buttonHintsHeight = 40,
                                          .sideButtonHintsWidth = 30,
                                          .progressBarHeight = 16,
                                          .progressBarMarginTop = 1,
                                          .statusBarHorizontalMargin = 5,
                                          .statusBarVerticalMargin = 19,
                                          .keyboardKeyWidth = 31,
                                          .keyboardKeyHeight = 40,
                                          .keyboardKeySpacing = 0,
                                          .keyboardCenteredText = false,
                                          .keyboardVerticalOffset = -7,
                                          .keyboardTextFieldWidthPercent = 85});
}

class LyraTheme : public BaseTheme {
 public:
  // Component drawing methods
  //   int drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) override;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(GfxRenderer& renderer, const char* upBtn, const char* downBtn) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  Rect drawPopup(const GfxRenderer& renderer, const char* message, bool overlayDisplayedFrame = true,
                 PopupShip ship = PopupShip::Blocking) const override;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const override;
  void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                     int contentStartX = 0, int contentWidth = 0) const override;
  bool showsFileIcons() const override { return true; }

 protected:
  StackedHomeRules stackedHomeRules() const override { return {170, 4}; }
  // The cover sits inside the selection's 8 px padding, above and below.
  int stackedCoverHeight(const int tileHeight) const override { return tileHeight - 16 > 120 ? tileHeight - 16 : 120; }

  // drawList's selected row: Lyra's light grey bar under black text, or, where this is true, a
  // solid black bar with the text and icon knocked out of it (Carousel).
  virtual bool invertsListSelection() const { return false; }

  static int getRecentBookProgressPercent(const RecentBook& book);

  // A book's details as a home card shows them: title, author, series, the status line (progress
  // and pace) and the reading history, each wrapped to the card's text column, then fitted to its
  // height by CardTextFit, which decides what gives way.
  struct CardText {
    std::array<std::vector<std::string>, CardTextFit::kPartCount> lines;
    std::array<int, CardTextFit::kPartCount> fontIds{};
    CardTextFit::Parts parts{};
    CardTextFit::Result fit;
  };
  // `titleFaces` largest first; an empty `statusLine` leaves the status out.
  static CardText fitCardText(const GfxRenderer& renderer, const RecentBook& book, const std::string& statusLine,
                              int textWidth, int height, const CardTextFit::Face* titleFaces, int titleFaceCount);
  // One part of `text`, its first line at `y`. The status line is cut to `width`; the rest were
  // wrapped to it.
  static void drawCardTextPart(const GfxRenderer& renderer, const CardText& text, CardTextFit::PartId part, int x,
                               int y, int width);
  static const uint8_t* iconForName(UIIcon icon, int size);
};
