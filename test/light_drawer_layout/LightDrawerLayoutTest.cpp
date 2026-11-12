// The light drawer (FrontlightPanelActivity) laid out by LightDrawerLayout at every lit board's portrait size, with
// the theme tokens and Lyra header band each has at both UI font sizes.
//
// What it holds: the sheet is as tall as what build() takes; every control lies on the sheet; each slider keeps a
// capsule at least two buttons wide (on the Murphy the brightness capsule had no width at all, the band being three
// 56 px squares); and the light and Dark Mode switches are both there, one each, big enough for their icons, in the
// slider rows with a warmth channel and on a line of their own without one.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "activities/util/LightDrawerLayout.h"

namespace fui = freeink::ui;
namespace ld = LightDrawerLayout;

namespace {

// Line heights are all the layout reads from the faces.
class LineTarget final : public fui::DrawTarget {
 public:
  LineTarget(int16_t small, int16_t body, int16_t title) : lines{small, body, title} {}
  fui::Size measureText(const fui::FontId font, const char* text, fui::TextStyle) const override {
    return fui::Size{static_cast<int16_t>(std::strlen(text) * lineHeight(font) / 2), lineHeight(font)};
  }
  int16_t lineHeight(const fui::FontId font) const override { return lines[font < 3 ? font : 1]; }
  void fill(fui::Rect, fui::Paint, uint8_t, uint8_t) override {}
  void stroke(fui::Rect, fui::Paint, uint8_t, uint8_t, uint8_t) override {}
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(fui::Rect, const char*, fui::TextStyle) override {}
  void bitmap(fui::Rect, fui::BitmapRef, fui::BitmapMode, fui::Paint, fui::Rotation) override {}

 private:
  int16_t lines[3];
};

struct Board {
  const char* name;
  int16_t width;
  int16_t height;
  bool compact;  // PanelMetrics::COMPACT on that build
  bool warmth;   // Frontlight.hasColorTemperature()
  // UiFontLadder::STEPS for that build and step: SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID line heights.
  int16_t small;
  int16_t body;
  int16_t title;
  // Lyra's topPadding + headerHeight (forPanel() caps it on the compact panel; UiFontLadder::applyTo grows it by the
  // title and small growth on the Large step).
  int16_t headerBand;
};

constexpr Board BOARDS[] = {
    {"Murphy", 240, 416, true, false, 17, 20, 25, 5 + 52},
    {"Murphy Large", 240, 416, true, false, 23, 25, 30, 5 + 52 + 11},
    {"X4 Pro", 480, 800, false, true, 23, 25, 30, 5 + 84},
    {"X4 Pro Large", 480, 800, false, true, 25, 30, 35, 5 + 84 + 7},
    {"T5S3", 540, 960, false, false, 23, 25, 30, 5 + 84},
    {"T5S3 Large", 540, 960, false, false, 25, 30, 35, 5 + 84 + 7},
};

struct Laid {
  ld::Metrics metrics;
  int16_t sheetHeight = 0;
  int16_t contentBottom = 0;  // the sheet minus its grabber band
  int16_t leftOver = 0;       // body height Screen had left after build()
  std::vector<fui::Interaction> hits;
  bool overflowed = false;
};

Laid layOut(const Board& board) {
  LineTarget target(board.small, board.body, board.title);
  // uiThemeTokens(): the SDK's tokens for the UI_10 line height.
  const fui::ThemeTokens tokens = fui::themeTokensForLineHeight(board.body);
  fui::DeviceContext device;
  device.width = board.width;
  device.height = board.height;
  device.hasTouch = true;
  const fui::InputSnapshot input{};
  fui::InteractionBuffer<24> interactions;  // UiAppHost::UiApp's interaction cap
  fui::Frame<24> frame(target, device, input, interactions);
  fui::Screen<24> screen(frame, tokens);

  ld::Model model;
  model.hasWarmth = board.warmth;
  model.brightnessLabel = "Brightness";
  model.warmthLabel = "Warmth";

  Laid laid;
  laid.metrics = ld::metricsFor(tokens, board.headerBand, board.compact);
  laid.sheetHeight = ld::sheetHeight(target, tokens, model, laid.metrics);
  ld::build(screen, model, laid.metrics, laid.sheetHeight);
  laid.leftOver = screen.body().height;
  const fui::SheetProps chrome;
  laid.contentBottom =
      static_cast<int16_t>(laid.sheetHeight - chrome.grabberMargin - chrome.grabberHeight - chrome.grabberInset);
  laid.hits.assign(interactions.data(), interactions.data() + interactions.count());
  laid.overflowed = interactions.overflowed();
  return laid;
}

std::vector<fui::Interaction> hitsFor(const Laid& laid, const fui::ActionId action) {
  std::vector<fui::Interaction> found;
  for (const auto& hit : laid.hits) {
    if (hit.action == action) found.push_back(hit);
  }
  return found;
}

}  // namespace

TEST(LightDrawerLayout, SheetIsAsTallAsWhatBuildTakes) {
  for (const Board& board : BOARDS) {
    SCOPED_TRACE(board.name);
    const Laid laid = layOut(board);
    // sheetHeight() reserves spaceLg of air under the last row; anything else means the two disagree.
    EXPECT_EQ(laid.leftOver, fui::themeTokensForLineHeight(board.body).spaceLg);
    EXPECT_LT(laid.sheetHeight, board.height);
  }
}

TEST(LightDrawerLayout, EveryControlIsOnTheSheet) {
  for (const Board& board : BOARDS) {
    SCOPED_TRACE(board.name);
    const Laid laid = layOut(board);
    EXPECT_FALSE(laid.overflowed);
    for (const auto& hit : laid.hits) {
      if (hit.action == ld::ACTION_DISMISS) continue;  // the page below the sheet
      SCOPED_TRACE(hit.action);
      EXPECT_GE(hit.rect.x, 0);
      EXPECT_LE(hit.rect.right(), board.width);
      EXPECT_GE(hit.rect.y, laid.metrics.headerBand);
      EXPECT_LE(hit.rect.bottom(), laid.contentBottom);
    }
  }
}

TEST(LightDrawerLayout, EverySliderKeepsACapsule) {
  for (const Board& board : BOARDS) {
    SCOPED_TRACE(board.name);
    const Laid laid = layOut(board);
    const auto brightness = hitsFor(laid, ld::ACTION_BRIGHTNESS);
    ASSERT_EQ(brightness.size(), 1u);
    EXPECT_GE(brightness[0].rect.width, 2 * laid.metrics.controlHeight);
    const auto warmth = hitsFor(laid, ld::ACTION_WARMTH);
    ASSERT_EQ(warmth.size(), board.warmth ? 1u : 0u);
    if (board.warmth) {
      EXPECT_GE(warmth[0].rect.width, 2 * laid.metrics.controlHeight);
    }
  }
}

TEST(LightDrawerLayout, BothSwitchesAreThereAndFitTheirIcons) {
  for (const Board& board : BOARDS) {
    SCOPED_TRACE(board.name);
    const Laid laid = layOut(board);
    const auto light = hitsFor(laid, ld::ACTION_LIGHT);
    const auto dark = hitsFor(laid, ld::ACTION_DARK_MODE);
    ASSERT_EQ(light.size(), 1u);
    ASSERT_EQ(dark.size(), 1u);
    for (const auto& hit : {light[0], dark[0]}) {
      // ButtonProps::padding is {2, 4, 2, 4}; the icons are 32 px.
      EXPECT_GE(hit.rect.height - 4, 32);
      EXPECT_GE(hit.rect.width - 8, 32);
    }
    const fui::Rect& a = light[0].rect;
    const fui::Rect& b = dark[0].rect;
    EXPECT_TRUE(a.right() <= b.x || b.right() <= a.x || a.bottom() <= b.y || b.bottom() <= a.y);

    const fui::Rect capsule = hitsFor(laid, ld::ACTION_BRIGHTNESS)[0].rect;
    if (board.warmth) {
      // Each closes a slider row: the sun the brightness row, the moon the warmth row.
      const fui::Rect warmth = hitsFor(laid, ld::ACTION_WARMTH)[0].rect;
      EXPECT_EQ(light[0].rect.y, capsule.y);
      EXPECT_EQ(dark[0].rect.y, warmth.y);
      EXPECT_GT(light[0].rect.x, capsule.right());
      EXPECT_GT(dark[0].rect.x, warmth.right());
    } else {
      // A line of their own under the brightness row, side by side.
      EXPECT_GT(light[0].rect.y, capsule.bottom());
      EXPECT_EQ(light[0].rect.y, dark[0].rect.y);
      EXPECT_LT(light[0].rect.x, dark[0].rect.x);
    }
  }
}

TEST(LightDrawerLayout, CompactPanelUsesTheTouchFloor) {
  const fui::ThemeTokens tokens = fui::themeTokensForLineHeight(20);
  EXPECT_EQ(ld::metricsFor(tokens, 57, true).controlHeight, tokens.minTouchSize);
  EXPECT_EQ(ld::metricsFor(tokens, 57, false).controlHeight, tokens.minTouchSize + 12);
}
