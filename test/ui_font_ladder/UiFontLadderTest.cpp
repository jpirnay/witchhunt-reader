// Cover for the UI font ladder's effect on theme metrics.
//
// Neither half of this is visible anywhere else. The pipeline goldens only see what dumpPage()
// prints, which is reader text — no golden, screenshot or existing test looks at menu chrome. So
// the two ways this feature breaks are both silent until someone holds a device:
//
//   1. A row that does not grow with the font clips its own text. That is what the growth table
//      is for, and a metric left out of applyTo() is exactly the kind of omission a reviewer
//      reading a list of field names does not catch.
//   2. A metric that grows when it should not. The reader status bar is the dangerous one: it
//      feeds the text viewport, and moving the viewport invalidates every book's pagination
//      cache, so scaling it would silently re-paginate the user's library on a font-size change.
//
// The third assertion below is the one that protects everybody who never touches this setting:
// at the default step applyTo() must be the identity, so default rendering cannot drift.
#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "UiFontScale.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/lyra/LyraTilesTheme.h"

namespace {

struct NamedTheme {
  const char* name;
  ThemeMetrics metrics;
};

std::vector<NamedTheme> allThemes() {
  return {
      {"Classic", BaseMetrics::values},
      {"Lyra", LyraMetrics::values},
      {"LyraTiles", LyraTilesMetrics::values},
      {"LyraCarousel", LyraCarouselMetrics::values},
  };
}

TEST(UiFontLadderTest, DefaultStepGrowsNothing) {
  const UiFontLadder::Step growth = UiFontLadder::growthAt(CrossPointSettings::UI_FONT_SIZE_DEFAULT);
  EXPECT_EQ(growth.small, 0);
  EXPECT_EQ(growth.body, 0);
  EXPECT_EQ(growth.title, 0);
}

// applyTo() must be the identity at the default step, or every user who leaves the setting alone
// gets a silently relaid-out UI.
TEST(UiFontLadderTest, DefaultStepLeavesEveryThemeUntouched) {
  const UiFontLadder::Step growth = UiFontLadder::growthAt(CrossPointSettings::UI_FONT_SIZE_DEFAULT);
  for (auto& theme : allThemes()) {
    ThemeMetrics scaled = theme.metrics;
    UiFontLadder::applyTo(scaled, growth);
    EXPECT_EQ(0, memcmp(&scaled, &theme.metrics, sizeof(ThemeMetrics))) << theme.name << " changed at the default step";
  }
}

// An out-of-range step (a settings file written by a newer firmware, or a corrupt byte) must read
// as the default rather than index past the table.
TEST(UiFontLadderTest, OutOfRangeStepFallsBackToDefault) {
  for (const uint8_t step :
       {static_cast<uint8_t>(CrossPointSettings::UI_FONT_SIZE_COUNT), uint8_t{200}, uint8_t{255}}) {
    const UiFontLadder::Step growth = UiFontLadder::growthAt(step);
    EXPECT_EQ(growth.small, 0) << "step " << static_cast<int>(step);
    EXPECT_EQ(growth.body, 0) << "step " << static_cast<int>(step);
    EXPECT_EQ(growth.title, 0) << "step " << static_cast<int>(step);
  }
}

TEST(UiFontLadderTest, EveryStepIsAtLeastAsLargeAsTheOneBelow) {
  for (size_t i = 1; i < UiFontLadder::STEP_COUNT; i++) {
    const auto& prev = UiFontLadder::STEPS[i - 1];
    const auto& cur = UiFontLadder::STEPS[i];
    EXPECT_GE(cur.small, prev.small) << "step " << i;
    EXPECT_GE(cur.body, prev.body) << "step " << i;
    EXPECT_GE(cur.title, prev.title) << "step " << i;
  }
  // A step that grows nothing is a setting the user can select with no effect.
  for (size_t i = 1; i < UiFontLadder::STEP_COUNT; i++) {
    const UiFontLadder::Step growth = UiFontLadder::growthAt(static_cast<uint8_t>(i));
    EXPECT_GT(growth.small + growth.body + growth.title, 0) << "step " << i << " is indistinguishable from default";
  }
}

// Every metric that holds a line of UI text has to gain at least the pixels that line gained, or
// the text clips. Checked per theme, since each ships its own table.
TEST(UiFontLadderTest, TextBearingMetricsGrowWithTheirFont) {
  for (size_t i = 1; i < UiFontLadder::STEP_COUNT; i++) {
    const UiFontLadder::Step growth = UiFontLadder::growthAt(static_cast<uint8_t>(i));
    for (auto& theme : allThemes()) {
      ThemeMetrics scaled = theme.metrics;
      UiFontLadder::applyTo(scaled, growth);
      const ThemeMetrics& base = theme.metrics;
      SCOPED_TRACE(std::string(theme.name) + " step " + std::to_string(i));

      // Rows drawn in the body font.
      EXPECT_GE(scaled.listRowHeight - base.listRowHeight, growth.body);
      EXPECT_GE(scaled.menuRowHeight - base.menuRowHeight, growth.body);
      EXPECT_GE(scaled.tabBarHeight - base.tabBarHeight, growth.body);
      EXPECT_GE(scaled.buttonHintsHeight - base.buttonHintsHeight, growth.body);
      EXPECT_GE(scaled.keyboardKeyHeight - base.keyboardKeyHeight, growth.body);

      // A title line over up to two subtitle lines — the worst case drawList() paints.
      EXPECT_GE(scaled.listWithSubtitleRowHeight - base.listWithSubtitleRowHeight, growth.body + growth.small * 2);

      // A title line over an optional subtitle line.
      EXPECT_GE(scaled.headerHeight - base.headerHeight, growth.title + growth.small);
    }
  }
}

// The reader's own chrome must not move: getStatusBarHeight() feeds the text viewport, and the
// viewport is part of the section cache key.
TEST(UiFontLadderTest, ReaderStatusBarGeometryNeverScales) {
  for (size_t i = 0; i < UiFontLadder::STEP_COUNT; i++) {
    const UiFontLadder::Step growth = UiFontLadder::growthAt(static_cast<uint8_t>(i));
    for (auto& theme : allThemes()) {
      ThemeMetrics scaled = theme.metrics;
      UiFontLadder::applyTo(scaled, growth);
      SCOPED_TRACE(std::string(theme.name) + " step " + std::to_string(i));
      EXPECT_EQ(scaled.progressBarHeight, theme.metrics.progressBarHeight);
      EXPECT_EQ(scaled.progressBarMarginTop, theme.metrics.progressBarMarginTop);
      EXPECT_EQ(scaled.statusBarVerticalMargin, theme.metrics.statusBarVerticalMargin);
      EXPECT_EQ(scaled.statusBarHorizontalMargin, theme.metrics.statusBarHorizontalMargin);
      EXPECT_EQ(scaled.batteryBarHeight, theme.metrics.batteryBarHeight);
      EXPECT_EQ(scaled.batteryHeight, theme.metrics.batteryHeight);
    }
  }
}

}  // namespace
