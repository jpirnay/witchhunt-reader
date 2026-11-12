// The reader bars (ReaderBarsActivity) laid out by ReaderBarsLayout at every board's size, held upright and sideways,
// with Progress closed and open.
//
// What it holds: every control lies on a bar and on the screen; a strip of the page stays in view between the bars;
// each action is there once; touch boards get finger-sized Progress buttons; and onBars() answers for exactly the bars,
// so a tap on the page between them closes them and a tap on a control does not.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "activities/reader/ReaderBarsLayout.h"

namespace fui = freeink::ui;
namespace rb = ReaderBarsLayout;

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
  int16_t width;  // upright
  int16_t height;
  bool touch;
  int16_t tabBar;  // ListTabBar::HEIGHT: PanelMetrics::capped(54, 34)
  // UiFontLadder::STEPS: SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID line heights.
  int16_t small;
  int16_t body;
  int16_t title;
};

constexpr Board BOARDS[] = {
    {"X3", 528, 792, false, 54, 23, 25, 30},          {"X4", 480, 800, false, 54, 23, 25, 30},
    {"X4 Pro", 480, 800, true, 54, 23, 25, 30},       {"X4 Pro Large", 480, 800, true, 54, 25, 30, 35},
    {"T5S3", 540, 960, true, 54, 23, 25, 30},         {"Murphy", 240, 416, true, 34, 17, 20, 25},
    {"Murphy Large", 240, 416, true, 34, 23, 25, 30},
};

struct Laid {
  fui::Rect area;
  rb::Model model;
  rb::Metrics metrics;
  std::vector<fui::Interaction> hits;
  bool overflowed = false;
};

Laid layOut(const Board& board, const bool sideways, const bool progressOpen) {
  LineTarget target(board.small, board.body, board.title);
  const fui::ThemeTokens tokens = fui::themeTokensForLineHeight(board.body);
  fui::DeviceContext device;
  device.width = sideways ? board.height : board.width;
  device.height = sideways ? board.width : board.height;
  device.hasTouch = board.touch;
  const fui::InputSnapshot input{};
  fui::InteractionBuffer<24> interactions;  // UiAppHost::UiApp's interaction cap
  fui::Frame<24> frame(target, device, input, interactions);
  fui::Screen<24> screen(frame, tokens);

  Laid laid;
  laid.area = screen.contentRect();
  laid.model.title = "A Rather Long Book Title That Will Not Fit";
  laid.model.progressOpen = progressOpen;
  laid.model.bookLabel = "Book";
  laid.model.bookValue = "47%";
  laid.model.chapterLabel = "Chapter";
  laid.model.chapterValue = "3 / 12";
  laid.model.timeLabel = "Total time";
  laid.model.timeValue = "2 h 15 min";
  laid.model.goToLabel = "Go to %";
  laid.metrics = rb::metricsFor(target, tokens, board.tabBar, board.touch);

  fui::TabItem tabs[rb::kTabCount]{};
  const char* labels[rb::kTabCount] = {"Contents", "Progress", "More"};
  for (uint8_t i = 0; i < rb::kTabCount; ++i) {
    tabs[i].label = labels[i];
    tabs[i].value = i;
  }
  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = rb::kTabCount;
  props.action = rb::ACTION_TAB;
  rb::build(screen, laid.model, laid.metrics, props);
  laid.hits.assign(interactions.data(), interactions.data() + interactions.count());
  laid.overflowed = interactions.overflowed();
  return laid;
}

size_t countOf(const Laid& laid, const fui::ActionId action) {
  size_t n = 0;
  for (const auto& hit : laid.hits) n += hit.action == action ? 1 : 0;
  return n;
}

template <typename Check>
void forEveryLayout(Check check) {
  for (const Board& board : BOARDS) {
    for (const bool sideways : {false, true}) {
      for (const bool open : {false, true}) {
        SCOPED_TRACE(std::string(board.name) + (sideways ? " sideways" : " upright") + (open ? ", Progress" : ""));
        check(board, layOut(board, sideways, open));
      }
    }
  }
}

}  // namespace

TEST(ReaderBarsLayout, EveryControlIsOnABarAndOnTheScreen) {
  forEveryLayout([](const Board&, const Laid& laid) {
    EXPECT_FALSE(laid.overflowed);
    const fui::Rect top = rb::topRect(laid.area, laid.metrics);
    const fui::Rect bottom = rb::bottomRect(laid.area, laid.model, laid.metrics);
    for (const auto& hit : laid.hits) {
      SCOPED_TRACE(hit.action);
      EXPECT_GE(hit.rect.x, 0);
      EXPECT_LE(hit.rect.right(), laid.area.width);
      EXPECT_GE(hit.rect.y, 0);
      EXPECT_LE(hit.rect.bottom(), laid.area.height);
      const int16_t cy = static_cast<int16_t>(hit.rect.y + hit.rect.height / 2);
      EXPECT_TRUE(cy < top.bottom() || cy >= bottom.y) << "a control in the page between the bars";
    }
  });
}

// The page between the bars is what a tap closes them on, and what is left to read: at least a finger wide and a
// quarter of the screen. On the Murphy held sideways (240 px) that is all Progress leaves, with its values on one line.
TEST(ReaderBarsLayout, TheBarsLeaveAStripOfThePage) {
  forEveryLayout([](const Board& board, const Laid& laid) {
    const int page =
        rb::bottomRect(laid.area, laid.model, laid.metrics).y - rb::topRect(laid.area, laid.metrics).bottom();
    EXPECT_GE(page, fui::themeTokensForLineHeight(board.body).minTouchSize);
    EXPECT_GE(page * 4, laid.area.height);
  });
}

TEST(ReaderBarsLayout, EachActionIsThereOnce) {
  forEveryLayout([](const Board&, const Laid& laid) {
    EXPECT_EQ(countOf(laid, rb::ACTION_EXIT), 1u);
    EXPECT_EQ(countOf(laid, rb::ACTION_STAR), 1u);
    EXPECT_EQ(countOf(laid, rb::ACTION_TAB), rb::kTabCount);
    const size_t progress = laid.model.progressOpen ? 1u : 0u;
    EXPECT_EQ(countOf(laid, rb::ACTION_PREV_CHAPTER), progress);
    EXPECT_EQ(countOf(laid, rb::ACTION_GO_TO), progress);
    EXPECT_EQ(countOf(laid, rb::ACTION_NEXT_CHAPTER), progress);
  });
}

TEST(ReaderBarsLayout, TouchBoardsGetFingerSizedProgressButtons) {
  forEveryLayout([](const Board& board, const Laid& laid) {
    if (!board.touch || !laid.model.progressOpen) return;
    const int16_t minTouch = fui::themeTokensForLineHeight(board.body).minTouchSize;
    for (const fui::ActionId action : {rb::ACTION_PREV_CHAPTER, rb::ACTION_GO_TO, rb::ACTION_NEXT_CHAPTER}) {
      for (const auto& hit : laid.hits) {
        if (hit.action != action) continue;
        EXPECT_GE(hit.rect.height, minTouch);
        EXPECT_GE(hit.rect.width, minTouch);
      }
    }
  });
}

TEST(ReaderBarsLayout, OnBarsAnswersForTheBarsAndNotThePage) {
  forEveryLayout([](const Board&, const Laid& laid) {
    for (const auto& hit : laid.hits) {
      const int cx = hit.rect.x + hit.rect.width / 2;
      const int cy = hit.rect.y + hit.rect.height / 2;
      EXPECT_TRUE(rb::onBars(laid.area, laid.model, laid.metrics, cx, cy)) << "action " << hit.action;
    }
    const fui::Rect top = rb::topRect(laid.area, laid.metrics);
    const fui::Rect bottom = rb::bottomRect(laid.area, laid.model, laid.metrics);
    const int pageMiddle = (top.bottom() + bottom.y) / 2;
    EXPECT_FALSE(rb::onBars(laid.area, laid.model, laid.metrics, laid.area.width / 2, pageMiddle));
  });
}
