#pragma once

#include <I18n.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  BaseTheme& getMutableTheme() { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
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
  // The mapping to logical edges is orientation-dependent.
  static Rect getContentRect(const GfxRenderer& renderer, bool hasBottomHints, bool hasSideHints);
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
  // Reads the overall progress percent (0..100) for a recent book from its cached
  // progress.bin. Cheap: derives the cache path from the book path, no book parsing.
  // Returns -1 when the book was never opened or the percent byte isn't written yet.
  static int getBookProgressPercent(const RecentBook& book);
  // Draws a reading-progress overlay directly on a cover thumbnail: a thin bar
  // along the bottom edge while in progress (1..99%), a folded top-right corner
  // when finished (100%), and nothing for unread books (0% / <0). Drawn with a
  // white halo so it stays legible over any cover art on the 1-bit display.
  // `coverRect` is the cover frame including its 1px border.
  static void drawCoverProgressIndicator(const GfxRenderer& renderer, Rect coverRect, int progressPercent);
  // Builds the compact progress/ETA status string for a recent book, e.g.
  // "62% · ~45m" — the percentage plus a pace-based time-to-finish estimate
  // from the reading-stats layer. The ETA suffix is dropped when the book is
  // finished or when there's too little history to project a meaningful pace.
  // The "~<time>" form is intentionally language-neutral (no translation string
  // needed). Returns "" when the book has no progress data (progressPercent < 0).
  static std::string formatBookProgressStatus(const RecentBook& book, int progressPercent);
  // Draws the progress/ETA status (see formatBookProgressStatus) as a filled
  // pill badge inset into the top-right corner of a cover. Used where the cover
  // is large enough to carry an overlaid label (e.g. the carousel centre cover).
  // Draws nothing when the book has no progress data.
  static void drawCoverProgressBadge(const GfxRenderer& renderer, Rect coverRect, const RecentBook& book,
                                     int progressPercent);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarTopHeight(bool forceStatusItems = false);
  static int getStatusBarBottomHeight(bool forceStatusItems = false);
  static int getStatusBarHeight(bool forceStatusItems = false);
  static int getStatusBarItemsHeight();
  static int getProgressBarHeight(uint8_t progressBar, uint8_t thickness);

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
