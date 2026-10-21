// Cover for HeaderLayout: where a screen's header sits and where the body below it starts.
//
// The failure this guards against is silent everywhere else: a body placed at a constant y drew
// over the header once the header outgrew the constant (the OPDS book detail put its cover and
// title at y=35, inside Base's 45 px band and Lyra's 84 px one), and a header placed at a bare y
// sat under the hint strip that PortraitInverted puts across the logical top. No golden or
// screenshot test looks at menu chrome, so these are checked here per theme and per UI font step.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "UiFontScale.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/HeaderLayout.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"

namespace {

struct NamedMetrics {
  std::string name;
  ThemeMetrics metrics;
};

// Every theme at every UI font step, scaled the way UITheme::setTheme() scales them.
std::vector<NamedMetrics> allThemesAllSteps() {
  const std::pair<const char*, ThemeMetrics> themes[] = {
      {"Classic", BaseMetrics::values},
      {"Lyra", LyraMetrics::values},
      {"Lyra3Covers", Lyra3CoversMetrics::values},
      {"LyraCarousel", LyraCarouselMetrics::values},
  };
  std::vector<NamedMetrics> out;
  for (const auto& [name, metrics] : themes) {
    for (size_t step = 0; step < UiFontLadder::STEP_COUNT; step++) {
      ThemeMetrics scaled = metrics;
      UiFontLadder::applyTo(scaled, UiFontLadder::growthAt(static_cast<uint8_t>(step)));
      out.push_back({std::string(name) + " step " + std::to_string(step), scaled});
    }
  }
  return out;
}

// What getContentRect() leaves on a 480x800 portrait panel with the bottom strip, and in
// PortraitInverted, where the same strip lies across the logical top.
Rect portraitContent(const ThemeMetrics& m) { return Rect{0, 0, 480, 800 - m.buttonHintsHeight}; }
Rect invertedContent(const ThemeMetrics& m) { return Rect{0, m.buttonHintsHeight, 480, 800 - m.buttonHintsHeight}; }

int bottomOf(const Rect& r) { return r.y + r.height; }

}  // namespace

TEST(HeaderLayoutTest, HeaderSitsOneTopPaddingIntoTheContent) {
  for (const auto& theme : allThemesAllSteps()) {
    SCOPED_TRACE(theme.name);
    for (const Rect& content : {portraitContent(theme.metrics), invertedContent(theme.metrics)}) {
      const Rect header = HeaderLayout::header(content, theme.metrics);
      EXPECT_EQ(header.x, content.x);
      EXPECT_EQ(header.width, content.width);
      EXPECT_EQ(header.y, content.y + theme.metrics.topPadding);
      EXPECT_EQ(header.height, theme.metrics.headerHeight);
    }
  }
}

// The regression itself: the body starts below the header band, by the theme's spacing, at every
// font step -- and keeps to the content area's width and bottom.
TEST(HeaderLayoutTest, BodyStartsBelowTheHeaderAndEndsWithTheContent) {
  for (const auto& theme : allThemesAllSteps()) {
    SCOPED_TRACE(theme.name);
    for (const Rect& content : {portraitContent(theme.metrics), invertedContent(theme.metrics)}) {
      const Rect header = HeaderLayout::header(content, theme.metrics);
      const Rect body = HeaderLayout::body(content, theme.metrics);
      EXPECT_EQ(body.y, bottomOf(header) + theme.metrics.verticalSpacing);
      EXPECT_EQ(body.x, content.x);
      EXPECT_EQ(body.width, content.width);
      EXPECT_EQ(bottomOf(body), bottomOf(content));
    }
  }
}

// In PortraitInverted the header moves down with the content area, past the strip at the top.
TEST(HeaderLayoutTest, InvertedHeaderClearsTheStripAtTheLogicalTop) {
  for (const auto& theme : allThemesAllSteps()) {
    SCOPED_TRACE(theme.name);
    const Rect header = HeaderLayout::header(invertedContent(theme.metrics), theme.metrics);
    EXPECT_GE(header.y, theme.metrics.buttonHintsHeight);
  }
}

// The default look does not move: Classic at the default step puts the body at 60, where the list
// screens (and the OPDS catalog's constant) always started.
TEST(HeaderLayoutTest, ClassicDefaultBodyStaysAtSixty) {
  EXPECT_EQ(HeaderLayout::body(portraitContent(BaseMetrics::values), BaseMetrics::values).y, 60);
}

TEST(HeaderLayoutTest, BelowKeepsTheContentBottom) {
  const Rect content{30, 40, 420, 720};
  const Rect rest = HeaderLayout::below(content, 100);
  EXPECT_EQ(rest.x, 30);
  EXPECT_EQ(rest.y, 100);
  EXPECT_EQ(rest.width, 420);
  EXPECT_EQ(bottomOf(rest), 760);
}

// The reader overlays' bare title. At the default step (title line 30 px) the body starts at 50,
// where the footnote list and the quick overrides always started; at LARGE (35 px) it moves down
// with the title instead of running into its descenders.
TEST(HeaderLayoutTest, OverlayBodyFollowsTheTitleLine) {
  for (size_t step = 0; step < UiFontLadder::STEP_COUNT; step++) {
    const int titleLine = UiFontLadder::STEPS[step].title;
    for (const Rect& content : {Rect{0, 0, 480, 760}, Rect{0, 40, 480, 760}}) {
      SCOPED_TRACE("step " + std::to_string(step) + " content.y " + std::to_string(content.y));
      EXPECT_EQ(HeaderLayout::overlayTitleY(content), content.y + HeaderLayout::OVERLAY_TITLE_TOP);
      EXPECT_EQ(HeaderLayout::overlayTitleBottom(content, titleLine), HeaderLayout::overlayTitleY(content) + titleLine);
      const Rect body = HeaderLayout::overlayBody(content, titleLine);
      EXPECT_EQ(body.y, HeaderLayout::overlayTitleBottom(content, titleLine) + HeaderLayout::OVERLAY_TITLE_GAP);
      EXPECT_EQ(bottomOf(body), bottomOf(content));
    }
  }
  EXPECT_EQ(HeaderLayout::overlayBody(Rect{0, 0, 480, 760}, UiFontLadder::STEPS[0].title).y, 50);
}
