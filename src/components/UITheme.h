#pragma once

#include <I18n.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "UiFontScale.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  BaseTheme& getMutableTheme() { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);

  // Growth of each logical UI font slot at the active SETTINGS.uiFontSize step, relative to the
  // default step (see UiFontLadder). Themes add these to the baked offsets that position stacked
  // text: each line shifts down by the total growth of the lines ABOVE it, which preserves the
  // spacing the design was tuned for instead of re-deriving it from line heights that were
  // deliberately overlapped.
  static UiFontLadder::Step fontGrowth();
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string makeSeparatorTitle(const std::string& title);
  static std::string makeSeparatorTitle(StrId labelId);
  static bool isSeparatorTitle(const std::string& title);
  static std::string stripSeparatorTitle(const std::string& title);
  // Returns a selectable predicate for use with ButtonNavigator::setSelectablePredicate().
  // Items whose title is marked as a separator (via makeSeparatorTitle) are skipped during
  // navigation. Pass the same title getter you pass to drawList so rendering and navigation
  // always agree on which items are separators.
  //
  // Preferred approach: derive from MenuListActivity (see MenuListActivity.h) which handles
  // separator skipping, navigation, and drawList automatically via SettingInfo items.
  //
  // Manual usage (for activities that don't derive from MenuListActivity).
  // Assumes `items` is a member variable (std::vector<SettingInfo> items):
  //
  //   // populate in constructor or onEnter:
  //   items.push_back(SettingInfo::Separator(StrId::STR_MY_SECTION));
  //   items.push_back(SettingInfo::Toggle(StrId::STR_MY_TOGGLE, &CrossPointSettings::myFlag));
  //
  //   // onEnter: wire navigation so separators are skipped:
  //   const auto pred = UITheme::makeSelectablePredicate(items.size(),
  //       [this](int i) { return items[i].getTitle(); });
  //   buttonNavigator.setSelectablePredicate(pred, items.size());
  //
  //   // render: pass the same getter to drawList; separator rows are drawn automatically:
  //   const auto& s = items;  // local ref for lambda capture
  //   GUI.drawList(renderer, rect, s.size(), selectedIndex,
  //       [&s](int i) { return s[i].getTitle(); },
  //       nullptr, nullptr,
  //       [&s](int i) { return s[i].getDisplayValue(); }, true);
  static std::function<bool(int)> makeSelectablePredicate(int total, std::function<std::string(int)> titleGetter);

  // Returns the drawable content Rect accounting for screen orientation and visible button hints.
  // Bottom hints occupy the physical bottom edge.
  // Side hints occupy the physical right edge on X4, and both physical sides on X3.
  // The mapping to logical edges is orientation-dependent. A strip that ends up on the logical left
  // or right edge also keeps HeaderLayout::HINT_COLUMN_GAP between it and the content.
  static Rect getContentRect(const GfxRenderer& renderer, bool hasBottomHints, bool hasSideHints);
  // The top strip the header, clock and battery are drawn in: one topPadding into the content area
  // getContentRect(renderer, hasBottomHints, hasSideHints) leaves (HeaderLayout::header), so it
  // clears a hint strip that lies across the logical top -- the bottom strip in PortraitInverted,
  // and in landscape the side boxes on boards whose side keys sit on that edge. Pass the flags the
  // screen's body uses, so getBodyRect() starts below this.
  //
  // Full width even on a screen that reserves the side edge for button hints, because in portrait
  // those boxes sit at mid-height, beside the body. Building the header from the narrowed content
  // rect instead pulls the clock and battery inward by the width of the side strip, which is why
  // they sat further right on Home -- the one screen of these that reserves no side edge -- than
  // on any other.
  static Rect getHeaderRect(const GfxRenderer& renderer, bool hasBottomHints = true, bool hasSideHints = false);
  // The area below the theme header: getContentRect(renderer, hasBottomHints, hasSideHints) less
  // the header band at its top and the theme's verticalSpacing under it (HeaderLayout::body).
  // Where a screen with a header starts its body, so the body moves with the header -- which grows
  // with the UI font size and differs by theme -- instead of sitting at a constant it outgrows.
  //
  // `subtitle` is the one the screen gives drawHeader(). With text, and a theme that draws it below
  // the body (Base), the body ends above getSubtitleLine(); null or empty, nothing is reserved.
  static Rect getBodyRect(const GfxRenderer& renderer, bool hasBottomHints, bool hasSideHints,
                          const char* subtitle = nullptr);
  // The line a header subtitle is drawn in when the theme keeps it out of the band (Base): the foot
  // of the content with every hint strip out. Height 0 for a theme that draws it in the band.
  static Rect getSubtitleLine(const GfxRenderer& renderer);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static std::string getCoverThumbPath(std::string coverBmpPath, int width, int height);
  // Edge length of the "this book has no usable cover" marker BMP written by
  // ReaderActivity::writeCoverPlaceholderBmp(). It is a real, complete 1x1 BMP so
  // isCoverThumbComplete() reports the book as resolved and the cover loops never re-open the
  // EPUB to rediscover the absence — while being a size no genuine thumbnail can have, so the
  // themes can recognise it (isCoverPlaceholderBmp) and draw their own no-cover tile instead of
  // blitting a blank rectangle over the slot.
  static constexpr int COVER_PLACEHOLDER_DIM = 1;
  // True if a parsed cover BMP is the no-cover marker rather than real artwork. Call after
  // parseHeaders() succeeds and BEFORE drawBitmap(): the marker must never be scaled into a slot.
  static bool isCoverPlaceholderBmp(const int width, const int height) {
    return width == COVER_PLACEHOLDER_DIM && height == COVER_PLACEHOLDER_DIM;
  }
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarTopHeight(bool forceStatusItems = false);
  static int getStatusBarBottomHeight(bool forceStatusItems = false);
  static int getStatusBarHeight(bool forceStatusItems = false);
  static int getStatusBarItemsHeight();
  static int getProgressBarHeight(uint8_t progressBar, uint8_t thickness);

 private:
  // A copy rather than a pointer into the theme's constexpr table: the UI font size scales
  // several of these at runtime. 160-odd bytes of RAM against 18% usage, versus a second
  // constexpr table per theme in the flash that is the binding constraint here.
  ThemeMetrics currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
