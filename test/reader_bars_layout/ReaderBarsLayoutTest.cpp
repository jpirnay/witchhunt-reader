// The reader bars (ReaderBarsController) laid out by ReaderBarsLayout at every board's size, held upright and sideways,
// with no panel, Progress and Layout open.
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
  ReaderLayoutSheet::Model sheet;
  rb::Metrics metrics;
  std::vector<fui::Interaction> hits;
  bool overflowed = false;
};

Laid layOut(const Board& board, const bool sideways, const rb::Panel panel) {
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
  laid.model.panel = panel;
  laid.model.bookLabel = "Book";
  laid.model.bookValue = "47%";
  laid.model.chapterLabel = "Chapter";
  laid.model.chapterValue = "3 / 12";
  laid.model.timeLabel = "Total time";
  laid.model.timeValue = "2 h 15 min";
  laid.model.goToLabel = "Go to %";
  laid.model.syncPullLabel = "Pull";
  laid.model.syncPushLabel = "Push";
  // The longest lists the bars make: Navigation with a printed page, the four orientations, one marked.
  if (panel == rb::Panel::Navigation) {
    laid.model.list[0] = {"Select Chapter", rb::Button::Chapters, false};
    laid.model.list[1] = {"Starred Pages", rb::Button::StarredPages, false};
    laid.model.list[2] = {"Go to Printed Page", rb::Button::PrintedPage, false};
    laid.model.list[3] = {"Go Home", rb::Button::GoHome, false};
    laid.model.listCount = 4;
  } else if (panel == rb::Panel::Orientation) {
    laid.model.list[0] = {"Portrait", rb::Button::Portrait, true};
    laid.model.list[1] = {"Landscape CW", rb::Button::LandscapeCw, false};
    laid.model.list[2] = {"Inverted", rb::Button::Inverted, false};
    laid.model.list[3] = {"Landscape CCW", rb::Button::LandscapeCcw, false};
    laid.model.listCount = 4;
  }
  laid.sheet.font = {"Font", "Literata"};
  laid.sheet.thisBook = {"This book only", "On"};
  const char* controlLabels[ReaderLayout::kControlCount] = {"Size",       "Lines",     "Margin",  "Darkness",
                                                            "Paragraphs", "Alignment", "Hyphens", "Book style"};
  for (uint8_t i = 0; i < ReaderLayout::kControlCount; ++i) laid.sheet.controls[i] = {controlLabels[i], "Maximum"};
  laid.model.layout = &laid.sheet;
  laid.metrics = rb::metricsFor(target, tokens, board.tabBar, board.touch, device.width, device.height);

  fui::TabItem tabs[rb::kTabCount]{};
  const freeink::Icon* icons[rb::kTabCount] = {&icon_navigation_32, &icon_progress_32, &icon_layout_32,
                                               &icon_orientation_32, &icon_more_32};
  for (uint8_t i = 0; i < rb::kTabCount; ++i) {
    tabs[i].icon = fui::bitmapFromIcon(*icons[i]);
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

size_t countOf(const Laid& laid, const fui::ActionId action, const int16_t value) {
  size_t n = 0;
  for (const auto& hit : laid.hits) n += hit.action == action && hit.value == value ? 1 : 0;
  return n;
}

size_t countOf(const Laid& laid, const rb::Button button) {
  return countOf(laid, rb::ACTION_BAR, static_cast<int16_t>(button));
}

template <typename Check>
void forEveryLayout(Check check) {
  for (const Board& board : BOARDS) {
    for (const bool sideways : {false, true}) {
      for (const rb::Panel panel :
           {rb::Panel::None, rb::Panel::Progress, rb::Panel::Layout, rb::Panel::Navigation, rb::Panel::Orientation}) {
        const char* names[] = {"", ", Progress", ", Layout", ", Navigation", ", Orientation"};
        SCOPED_TRACE(std::string(board.name) + (sideways ? " sideways" : " upright") + names[static_cast<int>(panel)]);
        const Laid laid = layOut(board, sideways, panel);
        // A screen the panel does not fit has none: its tab hands over to the reader (panelFits).
        if (!rb::panelFits(laid.model, laid.metrics, laid.area.height)) continue;
        check(board, laid);
      }
    }
  }
}

}  // namespace

TEST(ReaderBarsLayout, EveryControlIsOnABarAndOnTheScreen) {
  forEveryLayout([](const Board&, const Laid& laid) {
    EXPECT_FALSE(laid.overflowed);
    const fui::Rect top = rb::topRect(laid.area, laid.model, laid.metrics);
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
    // The sheet and the lists: their own rule below.
    if (laid.model.panel != rb::Panel::None && laid.model.panel != rb::Panel::Progress) return;
    const int page = rb::bottomRect(laid.area, laid.model, laid.metrics).y -
                     rb::topRect(laid.area, laid.model, laid.metrics).bottom();
    EXPECT_GE(page, fui::themeTokensForLineHeight(board.body).minTouchSize);
    EXPECT_GE(page * 4, laid.area.height);
  });
}

// The Layout sheet leaves a finger's width of page to show its changes on, or, where it cannot, takes the whole
// screen; and on the boards the bars were made for it is there at all, upright and sideways. Only the Murphy held
// sideways (240 px) may go without it.
TEST(ReaderBarsLayout, TheLayoutSheetLeavesAStripOrCoversThePage) {
  for (const Board& board : BOARDS) {
    for (const bool sideways : {false, true}) {
      SCOPED_TRACE(std::string(board.name) + (sideways ? " sideways" : " upright"));
      const Laid laid = layOut(board, sideways, rb::Panel::Layout);
      const bool fits = rb::layoutFits(laid.metrics, laid.area.height);
      if (std::string(board.name).rfind("Murphy", 0) != 0 || !sideways) EXPECT_TRUE(fits);
      if (!fits) continue;
      if (rb::coversPage(laid.model, laid.metrics, laid.area.height)) {
        EXPECT_EQ(rb::topRect(laid.area, laid.model, laid.metrics).height, 0);
        EXPECT_EQ(countOf(laid, rb::Button::Exit), 0u);
        continue;
      }
      const int page = rb::bottomRect(laid.area, laid.model, laid.metrics).y -
                       rb::topRect(laid.area, laid.model, laid.metrics).bottom();
      EXPECT_GE(page, laid.metrics.minPage);
    }
  }
}

// The Navigation and Orientation lists are there on every board, upright and sideways (two columns where one would
// crowd the page), and leave a finger's width of page or take the whole screen.
TEST(ReaderBarsLayout, TheListsFitAndLeaveAStripOrCoverThePage) {
  for (const Board& board : BOARDS) {
    for (const bool sideways : {false, true}) {
      for (const rb::Panel panel : {rb::Panel::Navigation, rb::Panel::Orientation}) {
        SCOPED_TRACE(std::string(board.name) + (sideways ? " sideways" : " upright") +
                     (panel == rb::Panel::Navigation ? ", Navigation" : ", Orientation"));
        const Laid laid = layOut(board, sideways, panel);
        ASSERT_TRUE(rb::panelFits(laid.model, laid.metrics, laid.area.height));
        if (rb::coversPage(laid.model, laid.metrics, laid.area.height)) {
          EXPECT_EQ(rb::topRect(laid.area, laid.model, laid.metrics).height, 0);
          continue;
        }
        const int page = rb::bottomRect(laid.area, laid.model, laid.metrics).y -
                         rb::topRect(laid.area, laid.model, laid.metrics).bottom();
        EXPECT_GE(page, laid.metrics.minPage);
      }
    }
  }
}

// KOReader sync's row is on Progress wherever a finger's width of page is left with it: on every board but the Murphy
// held sideways.
TEST(ReaderBarsLayout, TheSyncRowIsThereWhereThePageAllowsIt) {
  for (const Board& board : BOARDS) {
    for (const bool sideways : {false, true}) {
      SCOPED_TRACE(std::string(board.name) + (sideways ? " sideways" : " upright"));
      const Laid laid = layOut(board, sideways, rb::Panel::Progress);
      const bool murphySideways = std::string(board.name).rfind("Murphy", 0) == 0 && sideways;
      if (!murphySideways) EXPECT_TRUE(rb::showsSyncRow(laid.model, laid.metrics, laid.area.height));
    }
  }
}

// Every row of the sheet full: no row ends with a lone cell and empty space beside it.
TEST(ReaderBarsLayout, TheLayoutSheetsRowsAreFull) {
  forEveryLayout([](const Board&, const Laid& laid) {
    if (laid.model.panel != rb::Panel::Layout) return;
    const ReaderLayoutSheet::Metrics& m = laid.metrics.sheet;
    EXPECT_EQ(ReaderLayout::kStepperCount % m.stepperColumns, 0) << int{m.stepperColumns} << " stepper columns";
    EXPECT_EQ((ReaderLayout::kControlCount - ReaderLayout::kStepperCount) % m.pressColumns, 0)
        << int{m.pressColumns} << " press columns";
  });
}

TEST(ReaderBarsLayout, TheLayoutSheetHasEachControlOnce) {
  forEveryLayout([](const Board& board, const Laid& laid) {
    if (laid.model.panel != rb::Panel::Layout) return;
    using ReaderLayoutSheet::ACTION_SHEET;
    EXPECT_EQ(countOf(laid, ACTION_SHEET, ReaderLayoutSheet::kFontValue), 1u);
    EXPECT_EQ(countOf(laid, ACTION_SHEET, ReaderLayoutSheet::kThisBookValue), 1u);
    for (uint8_t c = 0; c < ReaderLayout::kControlCount; ++c) {
      const auto control = static_cast<ReaderLayout::Control>(c);
      EXPECT_EQ(countOf(laid, ACTION_SHEET, ReaderLayoutSheet::stepValue(control, true)), 1u) << "control " << int{c};
      EXPECT_EQ(countOf(laid, ACTION_SHEET, ReaderLayoutSheet::stepValue(control, false)),
                ReaderLayout::isStepper(control) ? 1u : 0u)
          << "control " << int{c};
    }
    if (!board.touch) return;
    const int16_t minTouch = fui::themeTokensForLineHeight(board.body).minTouchSize;
    for (const auto& hit : laid.hits) {
      if (hit.action != ReaderLayoutSheet::ACTION_SHEET) continue;
      EXPECT_GE(hit.rect.height, minTouch) << "action " << hit.action;
      EXPECT_GE(hit.rect.width, minTouch * 3 / 4) << "action " << hit.action;
    }
  });
}

TEST(ReaderBarsLayout, TheLayoutSheetsKeysGoBackTheWayTheyCame) {
  forEveryLayout([](const Board&, const Laid& laid) {
    if (laid.model.panel != rb::Panel::Layout) return;
    ReaderLayoutSheet::Target all[ReaderLayoutSheet::kMaxTargets];
    const fui::Rect sheet = rb::sheetRect(laid.area, laid.model, laid.metrics);
    const uint8_t count = ReaderLayoutSheet::targets(sheet, laid.metrics.sheet, all);
    EXPECT_EQ(count, ReaderLayoutSheet::kMaxTargets);
    for (int16_t i = 0; i < count; ++i) {
      // Every target lies inside the sheet.
      EXPECT_GE(all[i].rect.y, sheet.y);
      EXPECT_LE(all[i].rect.bottom(), sheet.bottom());
      const int16_t down = ReaderLayoutSheet::targetAcross(all, count, i, +1);
      if (down < 0) continue;
      EXPECT_EQ(all[down].row, all[i].row + 1);
      const int16_t back = ReaderLayoutSheet::targetAcross(all, count, down, -1);
      ASSERT_GE(back, 0);
      EXPECT_EQ(all[back].row, all[i].row);
    }
  });
}

TEST(ReaderBarsLayout, EachActionIsThereOnce) {
  forEveryLayout([](const Board&, const Laid& laid) {
    const size_t topBar = rb::coversPage(laid.model, laid.metrics, laid.area.height) ? 0u : 1u;
    EXPECT_EQ(countOf(laid, rb::Button::Exit), topBar);
    EXPECT_EQ(countOf(laid, rb::Button::Star), topBar);
    EXPECT_EQ(countOf(laid, rb::Button::Info), topBar);
    EXPECT_EQ(countOf(laid, rb::ACTION_TAB), rb::kTabCount);
    const size_t progress = laid.model.panel == rb::Panel::Progress ? 1u : 0u;
    EXPECT_EQ(countOf(laid, rb::Button::PrevChapter), progress);
    EXPECT_EQ(countOf(laid, rb::Button::GoTo), progress);
    EXPECT_EQ(countOf(laid, rb::Button::NextChapter), progress);
    const size_t sync = progress != 0 && rb::showsSyncRow(laid.model, laid.metrics, laid.area.height) ? 1u : 0u;
    EXPECT_EQ(countOf(laid, rb::Button::SyncPull), sync);
    EXPECT_EQ(countOf(laid, rb::Button::SyncPush), sync);
    const size_t navigation = laid.model.panel == rb::Panel::Navigation ? 1u : 0u;
    for (const rb::Button button :
         {rb::Button::Chapters, rb::Button::StarredPages, rb::Button::PrintedPage, rb::Button::GoHome}) {
      EXPECT_EQ(countOf(laid, button), navigation) << "button " << static_cast<int>(button);
    }
    const size_t orientation = laid.model.panel == rb::Panel::Orientation ? 1u : 0u;
    for (const rb::Button button :
         {rb::Button::Portrait, rb::Button::LandscapeCw, rb::Button::Inverted, rb::Button::LandscapeCcw}) {
      EXPECT_EQ(countOf(laid, button), orientation) << "button " << static_cast<int>(button);
    }
    // Three actions in all: FreeInkApp keeps 8 handlers a screen and drops any past that without a sign.
    for (const auto& hit : laid.hits) {
      EXPECT_TRUE(hit.action == rb::ACTION_BAR || hit.action == rb::ACTION_TAB ||
                  hit.action == ReaderLayoutSheet::ACTION_SHEET)
          << "action " << hit.action;
    }
  });
}

TEST(ReaderBarsLayout, TouchBoardsGetFingerSizedPanelButtons) {
  forEveryLayout([](const Board& board, const Laid& laid) {
    if (!board.touch || laid.model.panel == rb::Panel::None || laid.model.panel == rb::Panel::Layout) return;
    const int16_t minTouch = fui::themeTokensForLineHeight(board.body).minTouchSize;
    for (const auto& hit : laid.hits) {
      if (hit.action != rb::ACTION_BAR) continue;
      const auto button = static_cast<rb::Button>(hit.value);
      if (button == rb::Button::Exit || button == rb::Button::Star || button == rb::Button::Info) continue;
      EXPECT_GE(hit.rect.height, minTouch) << "button " << hit.value;
      EXPECT_GE(hit.rect.width, minTouch) << "button " << hit.value;
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
    const fui::Rect top = rb::topRect(laid.area, laid.model, laid.metrics);
    const fui::Rect bottom = rb::bottomRect(laid.area, laid.model, laid.metrics);
    if (bottom.y <= top.bottom()) return;  // the Layout sheet covers the page: there is none to tap
    const int pageMiddle = (top.bottom() + bottom.y) / 2;
    EXPECT_FALSE(rb::onBars(laid.area, laid.model, laid.metrics, laid.area.width / 2, pageMiddle));
  });
}
