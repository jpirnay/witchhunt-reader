#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIIcon.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "ReaderLayoutSheet.h"
#include "components/icons/readerBarIcons.h"

// The reader bars' layout, kept out of ReaderBarsActivity so that it needs FreeInkUI and nothing else of the
// firmware's: the activity draws it, and test/reader_bars_layout lays it out at every board's size in both
// orientations.
//
// Two bands over the page, which stays standing between them:
//  - the top bar: back out of the book on the left, the book's title, the page's star on the right;
//  - the bottom band: the tab bar (Contents, Progress, Layout, More) and the open panel above it: Progress (where the
//    book and the chapter stand, the time spent in the book, previous chapter / Go to % / next chapter) or Layout
//    (ReaderLayoutSheet).
//
// A screen too short to leave a strip of page under the Layout sheet gives the sheet the whole height: the top bar
// goes (coversPage). One too short even for that has no Layout sheet (layoutFits), and its Layout tab does what More
// does.
//
// The arrangement follows the reader bars of the stock XT Licorice firmware (the idea only; nothing of its code).
namespace ReaderBarsLayout {
namespace fui = freeink::ui;

// Three actions for the bars and their panels (ReaderLayoutSheet::ACTION_SHEET the third), each value naming its
// target: FreeInkApp keeps 8 handlers a screen and drops any past that without a sign.
constexpr fui::ActionId ACTION_BAR = 1;  // the value is the Button
constexpr fui::ActionId ACTION_TAB = 3;  // the value is the Tab

// The bars' own buttons: the top bar's two and Progress's three.
enum class Button : uint8_t { Exit, Star, PrevChapter, GoTo, NextChapter };

enum class Tab : uint8_t { Contents, Progress, Layout, More, Count };
constexpr uint8_t kTabCount = static_cast<uint8_t>(Tab::Count);

// What the keys' cursor rests on. None on a touch board until a key moves it: a cursor nobody asked for would only say
// which control the next Confirm hits, and a finger needs no such hint.
enum class Focus : uint8_t { None, Exit, Star, Tabs, PrevChapter, GoTo, NextChapter, Layout };

// The panel open above the tab bar.
enum class Panel : uint8_t { None, Progress, Layout };

// What the bars show. The strings are the caller's, already translated and formatted, alive until build() returns.
struct Model {
  const char* title = nullptr;
  bool starred = false;
  Panel panel = Panel::None;
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
  // The Layout sheet, while it is the panel. Its focus is the target the cursor is on when focus is Layout.
  const ReaderLayoutSheet::Model* layout = nullptr;
};

struct Metrics {
  int16_t topBar = 0;     // the top bar's height, rule included
  int16_t tabBar = 0;     // the tab bar's height
  int16_t row = 0;        // one Progress row
  int16_t button = 0;     // the Progress buttons' height, and the chapter buttons' width
  int16_t sideInset = 0;  // between the screen's sides and the Progress rows and buttons
  int16_t gap = 0;        // above and below the Progress buttons
  int16_t rule = 2;       // the line that parts the bottom band from the page
  int16_t minPage = 44;   // the least strip of page worth leaving between the bars: a finger's width
  ReaderLayoutSheet::Metrics sheet{};
};

// `tabBarHeight` is ListTabBar::HEIGHT, the reader menu's tab bar, so the two read alike. `touch` makes the Progress
// buttons finger-sized; with keys alone a row's height is enough. `width` and `height` are the screen's, held as it
// is now.
inline Metrics metricsFor(const fui::DrawTarget& target, const fui::ThemeTokens& theme, const int16_t tabBarHeight,
                          const bool touch, const int16_t width, const int16_t height) {
  Metrics metrics;
  metrics.minPage = theme.minTouchSize;
  metrics.sheet = ReaderLayoutSheet::metricsFor(target, theme, width,
                                                static_cast<int16_t>(height - tabBarHeight - metrics.rule), touch);
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

// The Layout sheet and the tab bar under it fit the screen at all, the top bar given up if need be.
inline bool layoutFits(const Metrics& metrics, const int16_t screenHeight) {
  return metrics.rule + metrics.tabBar + ReaderLayoutSheet::height(metrics.sheet) <= screenHeight;
}

// The Layout sheet takes the whole height: with the top bar there it would leave less than a finger's width of page.
inline bool coversPage(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  if (model.panel != Panel::Layout) return false;
  const int withTopBar =
      metrics.topBar + metrics.rule + metrics.tabBar + ReaderLayoutSheet::height(metrics.sheet) + metrics.minPage;
  return withTopBar > screenHeight;
}

inline int16_t bottomHeight(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  if (coversPage(model, metrics, screenHeight)) return screenHeight;
  int height = metrics.rule + metrics.tabBar;
  switch (model.panel) {
    case Panel::None:
      break;
    case Panel::Progress: {
      const uint8_t rows = stacksRows(model, metrics, screenHeight) ? rowCount(model) : uint8_t{1};
      height += progressHeight(metrics, rows);
      break;
    }
    case Panel::Layout:
      height += ReaderLayoutSheet::height(metrics.sheet);
      break;
  }
  return static_cast<int16_t>(height);
}

// The top bar, or nothing while the Layout sheet covers the page.
inline fui::Rect topRect(const fui::Rect& area, const Model& model, const Metrics& metrics) {
  if (coversPage(model, metrics, area.height)) return fui::Rect{area.x, area.y, area.width, 0};
  return fui::Rect{area.x, area.y, area.width, metrics.topBar};
}

inline fui::Rect bottomRect(const fui::Rect& area, const Model& model, const Metrics& metrics) {
  const int16_t height = bottomHeight(model, metrics, area.height);
  return fui::Rect{area.x, static_cast<int16_t>(area.bottom() - height), area.width, height};
}

// The Layout sheet's rect: the bottom band above the tab bar, below the rule.
inline fui::Rect sheetRect(const fui::Rect& area, const Model& model, const Metrics& metrics) {
  const fui::Rect bottom = bottomRect(area, model, metrics);
  const int16_t height = ReaderLayoutSheet::height(metrics.sheet);
  return fui::Rect{bottom.x, static_cast<int16_t>(bottom.bottom() - metrics.tabBar - height), bottom.width, height};
}

// True when (x, y) falls on a bar. A tap anywhere else lands on the page, and closes the bars.
inline bool onBars(const fui::Rect& area, const Model& model, const Metrics& metrics, const int x, const int y) {
  const auto px = static_cast<int16_t>(x);
  const auto py = static_cast<int16_t>(y);
  return topRect(area, model, metrics).contains(px, py) || bottomRect(area, model, metrics).contains(px, py);
}

// A square icon button, inverted while the keys' cursor is on it.
template <typename ScreenT>
void iconButton(ScreenT& screen, const fui::Rect rect, const freeink::Icon& icon, const Button button,
                const bool focused) {
  fui::ButtonProps props;
  props.icon = fui::bitmapFromIcon(icon);
  props.action = ACTION_BAR;
  props.value = static_cast<int16_t>(button);
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
void textButton(ScreenT& screen, const fui::Rect rect, const char* label, const Button button, const bool focused) {
  fui::ButtonProps props;
  props.label = label;
  props.action = ACTION_BAR;
  props.value = static_cast<int16_t>(button);
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
  const fui::Rect top = topRect(area, model, metrics);
  if (top.height > 0) {
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
               Button::Exit, model.focus == Focus::Exit);
    iconButton(screen, fui::Rect{static_cast<int16_t>(top.right() - theme.spaceSm - square), squareY, square, square},
               model.starred ? icon_star_filled_32 : icon_star_32, Button::Star, model.focus == Focus::Star);
  }

  // The bottom band: the rule along its top edge, the tab bar along its bottom one.
  const fui::Rect bottom = bottomRect(area, model, metrics);
  target.fill(bottom, white);
  target.fill(fui::Rect{bottom.x, bottom.y, bottom.width, metrics.rule}, black);
  fui::tabBar(screen.frame(),
              fui::Rect{bottom.x, static_cast<int16_t>(bottom.bottom() - metrics.tabBar), bottom.width, metrics.tabBar},
              tabs);
  if (model.panel == Panel::Layout && model.layout) {
    ReaderLayoutSheet::build(screen, sheetRect(area, model, metrics), *model.layout, metrics.sheet);
    return;
  }
  if (model.panel != Panel::Progress) return;

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
  textButton(screen, fui::Rect{left, y, chapterWidth, metrics.button}, "\xC2\xAB", Button::PrevChapter,
             model.focus == Focus::PrevChapter);
  textButton(screen,
             fui::Rect{static_cast<int16_t>(left + chapterWidth + theme.spaceMd), y,
                       static_cast<int16_t>(width - 2 * (chapterWidth + theme.spaceMd)), metrics.button},
             model.goToLabel, Button::GoTo, model.focus == Focus::GoTo);
  textButton(screen, fui::Rect{static_cast<int16_t>(left + width - chapterWidth), y, chapterWidth, metrics.button},
             "\xC2\xBB", Button::NextChapter, model.focus == Focus::NextChapter);
}

}  // namespace ReaderBarsLayout
