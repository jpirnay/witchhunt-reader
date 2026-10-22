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

// Base draws a header subtitle in a line of its own, one SMALL_FONT_ID line at every UI font step;
// the Lyra themes fit it in the band and keep no line.
TEST(HeaderLayoutTest, OnlyBaseKeepsASubtitleLine) {
  for (size_t step = 0; step < UiFontLadder::STEP_COUNT; step++) {
    SCOPED_TRACE("step " + std::to_string(step));
    const UiFontLadder::Step growth = UiFontLadder::growthAt(static_cast<uint8_t>(step));
    ThemeMetrics base = BaseMetrics::values;
    UiFontLadder::applyTo(base, growth);
    EXPECT_EQ(base.headerSubtitleLine, UiFontLadder::STEPS[step].small);
    for (ThemeMetrics lyra : {LyraMetrics::values, Lyra3CoversMetrics::values, LyraCarouselMetrics::values}) {
      UiFontLadder::applyTo(lyra, growth);
      EXPECT_EQ(lyra.headerSubtitleLine, 0);
    }
  }
}

// The line sits at the foot of the content, clear of the strips. On the X4's portrait content that
// is [737, 760), where the fixed y = 738 put it 1 px into the hint strip; on other panel heights
// (the X3's 752, the T5S3's 920, landscape) it follows the content instead of a constant.
TEST(HeaderLayoutTest, SubtitleLineIsTheFootOfTheContent) {
  const ThemeMetrics& m = BaseMetrics::values;
  for (const int contentBottom : {760, 752, 920, 450, 498}) {
    const Rect area{0, 0, 480, contentBottom};
    const Rect line = HeaderLayout::subtitleLine(area, m);
    EXPECT_EQ(bottomOf(line), contentBottom);
    EXPECT_EQ(line.height, m.headerSubtitleLine);
  }
  EXPECT_EQ(HeaderLayout::subtitleLine(portraitContent(m), m).y, 737);
}

// A subtitle with text shortens the body by the line; a null or empty one reserves nothing, and
// neither does a theme without the line.
TEST(HeaderLayoutTest, BodyEndsAboveTheSubtitleLine) {
  EXPECT_FALSE(HeaderLayout::hasText(nullptr));
  EXPECT_FALSE(HeaderLayout::hasText(""));
  EXPECT_TRUE(HeaderLayout::hasText("1 / 3"));
  for (const auto& theme : allThemesAllSteps()) {
    SCOPED_TRACE(theme.name);
    const Rect content = portraitContent(theme.metrics);
    const Rect body = HeaderLayout::body(content, theme.metrics);
    const Rect line = HeaderLayout::subtitleLine(content, theme.metrics);
    const Rect shortened = HeaderLayout::above(body, line);
    EXPECT_EQ(shortened.y, body.y);
    EXPECT_EQ(bottomOf(shortened), line.y);
    EXPECT_EQ(body.height - shortened.height, theme.metrics.headerSubtitleLine);
  }
  // A body that already ends above the line is left as it is.
  const Rect body{0, 60, 480, 600};
  EXPECT_EQ(HeaderLayout::above(body, Rect{0, 737, 480, 23}).height, 600);
}
