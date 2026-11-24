
#pragma once

#include <utility>
#include <vector>

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;
class Epub;
class Txt;
class Xtc;

// Lyra Carousel theme metrics (zero runtime cost)
namespace LyraCarouselMetrics {
constexpr ThemeMetrics values = forPanel({.batteryWidth = 16,
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
                                          .homeCoverHeight = 600,
                                          .homeCoverTileHeight = 660,
                                          .homeRecentBooksCount = 5,
                                          .homeContinueReadingInMenu = false,
                                          .homeMenuTopOffset = 16,
                                          .buttonHintsHeight = 40,
                                          .sideButtonHintsWidth = 30,
                                          .progressBarHeight = 16,
                                          .progressBarMarginTop = 1,
                                          .statusBarHorizontalMargin = 5,
                                          .statusBarVerticalMargin = 19,
                                          .keyboardKeyWidth = 31,
                                          .keyboardKeyHeight = 50,
                                          .keyboardKeySpacing = 0,
                                          .keyboardCenteredText = true,
                                          .keyboardVerticalOffset = -7,
                                          .keyboardTextFieldWidthPercent = 85,
                                          .keyboardKeyCornerRadius = 6});
}

class LyraCarouselTheme : public LyraTheme {
 public:
  // Exact pixel dimensions for each carousel slot — used for exact-size thumbnail generation
  static constexpr int kCenterCoverW = 340;
  static constexpr int kCenterCoverH = LyraCarouselMetrics::values.homeCoverHeight - 60;  // 540
  static constexpr int kSideCoverW = 200;
  static constexpr int kSideCoverH = LyraCarouselMetrics::values.homeCoverHeight - 210;  // 390

  static void setPreRenderIndex(int idx);

  HomeNavigation getHomeNavigation() const override { return HomeNavigation::Carousel; }
  std::vector<std::pair<int, int>> getCoverThumbSizes(int /*coverHeight*/) const override {
    return {{kCenterCoverW, kCenterCoverH}, {kSideCoverW, kSideCoverH}};
  }
  bool tryFastHomeRender(GfxRenderer& renderer, const std::vector<RecentBook>& recentBooks, int selectorIndex,
                         int menuCount, const std::function<std::string(int)>& menuLabel,
                         const std::function<UIIcon(int)>& menuIcon, const char* hintBtn1, const char* hintBtn2,
                         const char* hintBtn3, const char* hintBtn4) const override;
  void onBookWillClose(const std::string& path, Epub* epub, Xtc* xtc, Txt* txt) override;
  void invalidateFrameCache() override;
  void markFrameCacheDirty() override;
  size_t frameCacheRegionBytes(const GfxRenderer& renderer) const override;
  void setFrameCacheRegion(uint8_t* region, size_t bytes) override;

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

 protected:
  // Classic's floors, not Lyra's: the carousel was always laid out by them, and only its fallback
  // draw reads the result (its covers and icon row sit at fixed places).
  StackedHomeRules stackedHomeRules() const override { return BaseTheme::stackedHomeRules(); }
  int stackedCoverHeight(const int tileHeight) const override { return BaseTheme::stackedCoverHeight(tileHeight); }

  // Lyra's lists, with the selected row a solid black bar.
  bool invertsListSelection() const override { return true; }
};
