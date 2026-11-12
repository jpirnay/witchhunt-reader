#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIIcon.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "components/icons/readerBarIcons.h"

// The reader bars' layout, kept out of ReaderBarsActivity so that it needs FreeInkUI and nothing else of the
// firmware's: the activity draws it, and test/reader_bars_layout lays it out at every board's size in both
// orientations.
//
// Two bands over the page, which stays standing between them:
//  - the top bar: back out of the book on the left, the book's title, the page's star on the right;
//  - the bottom band: the tab bar (Contents, Progress, More) and, while Progress is open, its rows above it: where the
//    book and the chapter stand, the time spent in the book, and previous chapter / Go to % / next chapter.
//
// The arrangement follows the reader bars of the stock XT Licorice firmware (the idea only; nothing of its code).
namespace ReaderBarsLayout {
namespace fui = freeink::ui;

constexpr fui::ActionId ACTION_EXIT = 1;
constexpr fui::ActionId ACTION_STAR = 2;
constexpr fui::ActionId ACTION_TAB = 3;  // the value is the Tab
constexpr fui::ActionId ACTION_PREV_CHAPTER = 4;
constexpr fui::ActionId ACTION_GO_TO = 5;
constexpr fui::ActionId ACTION_NEXT_CHAPTER = 6;

enum class Tab : uint8_t { Contents, Progress, More, Count };
constexpr uint8_t kTabCount = static_cast<uint8_t>(Tab::Count);

// What the keys' cursor rests on. None on a touch board until a key moves it: a cursor nobody asked for would only say
// which control the next Confirm hits, and a finger needs no such hint.
enum class Focus : uint8_t { None, Exit, Star, Tabs, PrevChapter, GoTo, NextChapter };

// What the bars show. The strings are the caller's, already translated and formatted, alive until build() returns.
struct Model {
  const char* title = nullptr;
  bool starred = false;
  bool progressOpen = false;
  Focus focus = Focus::None;
  Tab focusedTab = Tab::Contents;  // the tab under the cursor while focus is Tabs
  // The Progress rows. A row without a value is left out.
  const char* bookLabel = nullptr;
  const char* bookValue = nullptr;
  const char* chapterLabel = nullptr;
  const char* chapterValue = nullptr;
  const char* timeLabel = nullptr;
  const char* timeValue = nullptr;
  const char* goToLabel = nullptr;
};

struct Metrics {
  int16_t topBar = 0;     // the top bar's height, rule included
  int16_t tabBar = 0;     // the tab bar's height
  int16_t row = 0;        // one Progress row
  int16_t button = 0;     // the Progress buttons' height, and the chapter buttons' width
  int16_t sideInset = 0;  // between the screen's sides and the Progress rows and buttons
  int16_t gap = 0;        // above and below the Progress buttons
  int16_t rule = 2;       // the line that parts the bottom band from the page
};

// `tabBarHeight` is ListTabBar::HEIGHT, the reader menu's tab bar, so the two read alike. `touch` makes the Progress
// buttons finger-sized; with keys alone a row's height is enough.
inline Metrics metricsFor(const fui::DrawTarget& target, const fui::ThemeTokens& theme, const int16_t tabBarHeight,
                          const bool touch) {
  Metrics metrics;
  const int body = target.lineHeight(theme.bodyText.font);
  metrics.topBar = static_cast<int16_t>(std::max<int>(theme.headerHeight, 32 + theme.spaceMd * 2));
  metrics.tabBar = tabBarHeight;
  metrics.row = static_cast<int16_t>(body + theme.spaceSm * 2);
  metrics.button = static_cast<int16_t>(touch ? std::max<int>(theme.minTouchSize, body + theme.spaceMd * 2)
                                              : body + theme.spaceMd * 2);
  metrics.sideInset = theme.spaceLg;
  metrics.gap = theme.spaceMd;
  return metrics;
}

inline uint8_t rowCount(const Model& model) {
  return static_cast<uint8_t>((model.bookValue ? 1 : 0) + (model.chapterValue ? 1 : 0) + (model.timeValue ? 1 : 0));
}

inline int16_t progressHeight(const Metrics& metrics, const uint8_t rows) {
  return static_cast<int16_t>(rows * metrics.row + metrics.gap + metrics.button + metrics.gap);
}

// One row a value, or, where that leaves too little of the page between the bars (the Murphy held sideways), the
// values together on one line. The bars may take at most three fifths of the screen.
inline bool stacksRows(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  const int stacked = metrics.topBar + metrics.rule + metrics.tabBar + progressHeight(metrics, rowCount(model));
  return stacked * 5 <= screenHeight * 3;
}

inline int16_t bottomHeight(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  int height = metrics.rule + metrics.tabBar;
  if (model.progressOpen) {
    const uint8_t rows = stacksRows(model, metrics, screenHeight) ? rowCount(model) : uint8_t{1};
    height += progressHeight(metrics, rows);
  }
  return static_cast<int16_t>(height);
}

inline fui::Rect topRect(const fui::Rect& area, const Metrics& metrics) {
  return fui::Rect{area.x, area.y, area.width, metrics.topBar};
}

inline fui::Rect bottomRect(const fui::Rect& area, const Model& model, const Metrics& metrics) {
  const int16_t height = bottomHeight(model, metrics, area.height);
  return fui::Rect{area.x, static_cast<int16_t>(area.bottom() - height), area.width, height};
}

// True when (x, y) falls on a bar. A tap anywhere else lands on the page, and closes the bars.
inline bool onBars(const fui::Rect& area, const Model& model, const Metrics& metrics, const int x, const int y) {
  const auto px = static_cast<int16_t>(x);
  const auto py = static_cast<int16_t>(y);
  return topRect(area, metrics).contains(px, py) || bottomRect(area, model, metrics).contains(px, py);
}

// A square icon button, inverted while the keys' cursor is on it.
template <typename ScreenT>
void iconButton(ScreenT& screen, const fui::Rect rect, const freeink::Icon& icon, const fui::ActionId action,
                const bool focused) {
  fui::ButtonProps props;
  props.icon = fui::bitmapFromIcon(icon);
  props.action = action;
  props.state = focused ? fui::StateSelected : fui::StateNormal;
  props.styles.normal.background = fui::Paint::none();
  props.styles.selected.background = fui::Paint::solid(fui::Color::Black);
  props.styles.selected.foreground = fui::Paint::solid(fui::Color::White);
  props.styles.selected.radius = 8;
  props.styles.explicitlySet = true;
  props.borderEdges = fui::EdgesNone;
  screen.button(props, rect);
}

// A text button of the Progress row, framed, inverted while the keys' cursor is on it.
template <typename ScreenT>
void textButton(ScreenT& screen, const fui::Rect rect, const char* label, const fui::ActionId action,
                const bool focused) {
  fui::ButtonProps props;
  props.label = label;
  props.action = action;
  props.text = screen.theme().bodyText;
  props.state = focused ? fui::StateSelected : fui::StateNormal;
  props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
  props.styles.normal.borderWidth = 2;
  props.styles.normal.radius = 8;
  props.styles.selected = props.styles.normal;
  props.styles.selected.background = fui::Paint::solid(fui::Color::Black);
  props.styles.selected.foreground = fui::Paint::solid(fui::Color::White);
  props.styles.explicitlySet = true;
  screen.button(props, rect);
}

// Draws the bars over whatever the frame holds. `tabs` is the tab bar's props (ListTabBar::props, ACTION_TAB), its
// TabItem array alive until this returns.
template <typename ScreenT>
void build(ScreenT& screen, const Model& model, const Metrics& metrics, const fui::TabBarProps& tabs) {
  const fui::ThemeTokens& theme = screen.theme();
  const fui::Rect area = screen.contentRect();
  fui::DrawTarget& target = screen.target();
  const fui::Paint white = fui::Paint::solid(fui::Color::White);
  const fui::Paint black = fui::Paint::solid(fui::Color::Black);

  // The top bar: the title, kept clear of the two square buttons at its ends, and the rule under it. Drawn as text and
  // a fill rather than through fui::header, whose battery and clock chrome would come along unused (2.5 KB on a C3).
  const fui::Rect top = topRect(area, metrics);
  target.fill(top, white);
  target.fill(fui::Rect{top.x, static_cast<int16_t>(top.bottom() - metrics.rule), top.width, metrics.rule}, black);
  fui::TextStyle titleStyle = theme.bodyText;
  titleStyle.align = fui::TextAlign::Center;
  target.text(fui::Rect{static_cast<int16_t>(top.x + metrics.topBar), top.y,
                        static_cast<int16_t>(top.width - 2 * metrics.topBar),
                        static_cast<int16_t>(metrics.topBar - metrics.rule)},
              model.title ? model.title : "", titleStyle);
  const int16_t square = static_cast<int16_t>(metrics.topBar - metrics.rule - theme.spaceSm);
  const int16_t squareY = static_cast<int16_t>(top.y + (metrics.topBar - metrics.rule - square) / 2);
  iconButton(screen, fui::Rect{static_cast<int16_t>(top.x + theme.spaceSm), squareY, square, square}, icon_back_32,
             ACTION_EXIT, model.focus == Focus::Exit);
  iconButton(screen, fui::Rect{static_cast<int16_t>(top.right() - theme.spaceSm - square), squareY, square, square},
             model.starred ? icon_star_filled_32 : icon_star_32, ACTION_STAR, model.focus == Focus::Star);

  // The bottom band: the rule along its top edge, the tab bar along its bottom one.
  const fui::Rect bottom = bottomRect(area, model, metrics);
  target.fill(bottom, white);
  target.fill(fui::Rect{bottom.x, bottom.y, bottom.width, metrics.rule}, black);
  fui::tabBar(screen.frame(),
              fui::Rect{bottom.x, static_cast<int16_t>(bottom.bottom() - metrics.tabBar), bottom.width, metrics.tabBar},
              tabs);
  if (!model.progressOpen) return;

  const int16_t left = static_cast<int16_t>(bottom.x + metrics.sideInset);
  const int16_t width = static_cast<int16_t>(bottom.width - 2 * metrics.sideInset);
  int16_t y = static_cast<int16_t>(bottom.y + metrics.rule);
  // A label on the left, its value on the right. Plain text: the rows take no input.
  fui::TextStyle labelStyle = theme.bodyText;
  fui::TextStyle valueStyle = theme.bodyText;
  valueStyle.align = fui::TextAlign::Right;
  const auto addRow = [&](const char* label, const char* value) {
    if (!value) return;
    const fui::Rect rect{left, y, width, metrics.row};
    if (label) target.text(rect, label, labelStyle);
    target.text(rect, value, valueStyle);
    y = static_cast<int16_t>(y + metrics.row);
  };
  if (stacksRows(model, metrics, area.height)) {
    addRow(model.bookLabel, model.bookValue);
    addRow(model.chapterLabel, model.chapterValue);
    addRow(model.timeLabel, model.timeValue);
  } else {
    // One line, the values without their labels: the book, the chapter, the time.
    char line[96];
    snprintf(line, sizeof(line), "%s  ·  %s%s%s", model.bookValue ? model.bookValue : "",
             model.chapterValue ? model.chapterValue : "", model.timeValue ? "  ·  " : "",
             model.timeValue ? model.timeValue : "");
    valueStyle.align = fui::TextAlign::Center;
    addRow(nullptr, line);
  }

  y = static_cast<int16_t>(y + metrics.gap);
  const int16_t chapterWidth = static_cast<int16_t>(metrics.button + metrics.button / 2);
  textButton(screen, fui::Rect{left, y, chapterWidth, metrics.button}, "\xC2\xAB", ACTION_PREV_CHAPTER,
             model.focus == Focus::PrevChapter);
  textButton(screen,
             fui::Rect{static_cast<int16_t>(left + chapterWidth + theme.spaceMd), y,
                       static_cast<int16_t>(width - 2 * (chapterWidth + theme.spaceMd)), metrics.button},
             model.goToLabel, ACTION_GO_TO, model.focus == Focus::GoTo);
  textButton(screen, fui::Rect{static_cast<int16_t>(left + width - chapterWidth), y, chapterWidth, metrics.button},
             "\xC2\xBB", ACTION_NEXT_CHAPTER, model.focus == Focus::NextChapter);
}

}  // namespace ReaderBarsLayout
