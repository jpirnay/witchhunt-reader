#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIIcon.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "ReaderLayoutSheet.h"
#include "components/icons/readerBarIcons.h"

// The reader bars' layout, kept out of ReaderBarsController so that it needs FreeInkUI and nothing else of the
// firmware's: the controller draws it, and test/reader_bars_layout lays it out at every board's size in both
// orientations.
//
// Two bands over the page, which stays standing between them:
//  - the top bar: back out of the book on the left, the book's title, the page's star on the right;
//  - the bottom band: the tab bar (Navigation, Progress, Layout, Orientation, More: an icon each) and the open panel
//    above it: Navigation (the chapter list, the starred pages, a printed page, leaving the book), Progress (where the
//    book and the chapter stand, the time spent in the book, previous chapter / Go to % / next chapter, and KOReader
//    sync's pull and push where it is set up), Layout (ReaderLayoutSheet) or Orientation (the four ways to hold it).
//
// A screen too short to leave a strip of page under a panel gives the panel the whole height: the top bar goes
// (coversPage). A list too long for one column takes two (listColumns). A screen too short even for that has no such
// panel (panelFits): its tab does what the reader menu does.
//
// The arrangement follows the reader bars of the stock XT Licorice firmware (the idea only; nothing of its code).
namespace ReaderBarsLayout {
namespace fui = freeink::ui;

// Three actions for the bars and their panels (ReaderLayoutSheet::ACTION_SHEET the third), each value naming its
// target: FreeInkApp keeps 8 handlers a screen and drops any past that without a sign.
constexpr fui::ActionId ACTION_BAR = 1;  // the value is the Button
constexpr fui::ActionId ACTION_TAB = 3;  // the value is the Tab

// The bars' own buttons: the top bar's three (the title opens the book's details), Progress's chapter row and sync row,
// and the rows of the Navigation and Orientation lists. The four orientations in CrossPointSettings::ORIENTATION's
// order, from Portrait.
enum class Button : uint8_t {
  Exit,
  Star,
  PrevChapter,
  GoTo,
  NextChapter,
  Info,
  SyncPull,
  SyncPush,
  Chapters,
  StarredPages,
  PrintedPage,
  Search,
  GoHome,
  Portrait,
  LandscapeCw,
  Inverted,
  LandscapeCcw,
};

enum class Tab : uint8_t { Navigation, Progress, Layout, Orientation, More, Count };
constexpr uint8_t kTabCount = static_cast<uint8_t>(Tab::Count);

// What the keys' cursor rests on. None on a touch board until a key moves it: a cursor nobody asked for would only say
// which control the next Confirm hits, and a finger needs no such hint. List: a row of the Navigation or Orientation
// list (Model::listFocus).
enum class Focus : uint8_t {
  None,
  Exit,
  Star,
  Tabs,
  PrevChapter,
  GoTo,
  NextChapter,
  Layout,
  Title,
  SyncPull,
  SyncPush,
  List
};

// The panel open above the tab bar.
enum class Panel : uint8_t { None, Progress, Layout, Navigation, Orientation };

// One row of the Navigation or Orientation list. `marked`: the orientation the book is read in.
struct ListRow {
  const char* label = nullptr;
  Button button = Button::Chapters;
  bool marked = false;
};
constexpr uint8_t kMaxListRows = 5;

// What the bars show. The strings are the caller's, already translated and formatted, alive until build() returns.
struct Model {
  const char* title = nullptr;
  bool starred = false;
  Panel panel = Panel::None;
  Focus focus = Focus::None;
  Tab focusedTab = Tab::Navigation;  // the tab under the cursor while focus is Tabs
  // The Progress rows. A row without a value is left out.
  const char* bookLabel = nullptr;
  const char* bookValue = nullptr;
  const char* chapterLabel = nullptr;
  const char* chapterValue = nullptr;
  const char* timeLabel = nullptr;
  const char* timeValue = nullptr;
  const char* goToLabel = nullptr;
  // Progress's sync row, where KOReader sync is set up; null leaves it out.
  const char* syncPullLabel = nullptr;
  const char* syncPushLabel = nullptr;
  // The Layout sheet, while it is the panel. Its focus is the target the cursor is on when focus is Layout.
  const ReaderLayoutSheet::Model* layout = nullptr;
  // The Navigation or Orientation list, while it is the panel, and the row under the cursor while focus is List.
  ListRow list[kMaxListRows]{};
  uint8_t listCount = 0;
  uint8_t listFocus = 0;
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

// The value rows, then the chapter buttons, then the sync row where there is one.
inline int16_t progressHeight(const Metrics& metrics, const uint8_t rows, const bool syncRow) {
  return static_cast<int16_t>(rows * metrics.row + metrics.gap + metrics.button + metrics.gap +
                              (syncRow ? metrics.button + metrics.gap : 0));
}

// One row a value, or, where that leaves too little of the page between the bars (the Murphy held sideways), the
// values together on one line. The bars may take at most three fifths of the screen.
inline bool stacksRows(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  const int stacked = metrics.topBar + metrics.rule + metrics.tabBar + progressHeight(metrics, rowCount(model), false);
  return stacked * 5 <= screenHeight * 3;
}

// The sync row, where KOReader sync is set up and the screen still has a finger's width of page with it. The Murphy
// held sideways has not: there sync stays in the reader menu, one step past More.
inline bool showsSyncRow(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  if (!model.syncPullLabel || !model.syncPushLabel) return false;
  const uint8_t rows = stacksRows(model, metrics, screenHeight) ? rowCount(model) : uint8_t{1};
  return metrics.topBar + metrics.rule + metrics.tabBar + progressHeight(metrics, rows, true) + metrics.minPage <=
         screenHeight;
}

// A list of `rows` rows of buttons, a gap above, between and below them.
inline int16_t listHeight(const Metrics& metrics, const uint8_t rows) {
  return static_cast<int16_t>(rows * metrics.button + (rows + 1) * metrics.gap);
}

inline uint8_t listRows(const uint8_t count, const uint8_t columns) {
  return static_cast<uint8_t>((count + columns - 1) / columns);
}

// One column, or two where one would leave less than a finger's width of page with the top bar there.
inline uint8_t listColumns(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  const int oneColumn =
      metrics.topBar + metrics.rule + metrics.tabBar + listHeight(metrics, model.listCount) + metrics.minPage;
  return oneColumn > screenHeight ? uint8_t{2} : uint8_t{1};
}

// The open panel's height above the tab bar.
inline int16_t panelHeight(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  switch (model.panel) {
    case Panel::None:
      return 0;
    case Panel::Progress: {
      const uint8_t rows = stacksRows(model, metrics, screenHeight) ? rowCount(model) : uint8_t{1};
      return progressHeight(metrics, rows, showsSyncRow(model, metrics, screenHeight));
    }
    case Panel::Layout:
      return ReaderLayoutSheet::height(metrics.sheet);
    case Panel::Navigation:
    case Panel::Orientation:
      return listHeight(metrics, listRows(model.listCount, listColumns(model, metrics, screenHeight)));
  }
  return 0;
}

// The panel and the tab bar under it fit the screen at all, the top bar given up if need be. A screen they do not fit
// has no such panel: its tab hands over to the reader.
inline bool panelFits(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  return metrics.rule + metrics.tabBar + panelHeight(model, metrics, screenHeight) <= screenHeight;
}

// The Layout sheet as the panel, for the tab's check before it opens.
inline bool layoutFits(const Metrics& metrics, const int16_t screenHeight) {
  return metrics.rule + metrics.tabBar + ReaderLayoutSheet::height(metrics.sheet) <= screenHeight;
}

// The panel takes the whole height: with the top bar there it would leave less than a finger's width of page.
// Progress never does: its rows go onto one line instead (stacksRows).
inline bool coversPage(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  if (model.panel == Panel::None || model.panel == Panel::Progress) return false;
  const int withTopBar =
      metrics.topBar + metrics.rule + metrics.tabBar + panelHeight(model, metrics, screenHeight) + metrics.minPage;
  return withTopBar > screenHeight;
}

inline int16_t bottomHeight(const Model& model, const Metrics& metrics, const int16_t screenHeight) {
  if (coversPage(model, metrics, screenHeight)) return screenHeight;
  return static_cast<int16_t>(metrics.rule + metrics.tabBar + panelHeight(model, metrics, screenHeight));
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

// The list's rect, as the sheet's: above the tab bar, its rows from the top down.
inline fui::Rect listRect(const fui::Rect& area, const Model& model, const Metrics& metrics) {
  const fui::Rect bottom = bottomRect(area, model, metrics);
  const int16_t height = panelHeight(model, metrics, area.height);
  return fui::Rect{bottom.x, static_cast<int16_t>(bottom.bottom() - metrics.tabBar - height), bottom.width, height};
}

// Row `index` of the list in `rect`, laid in `columns` columns left to right, then down.
inline fui::Rect listCell(const fui::Rect& rect, const Metrics& metrics, const fui::ThemeTokens& theme,
                          const uint8_t columns, const uint8_t index) {
  const int16_t inner = static_cast<int16_t>(rect.width - 2 * metrics.sideInset);
  const int16_t width = static_cast<int16_t>((inner - (columns - 1) * theme.spaceMd) / columns);
  const uint8_t column = static_cast<uint8_t>(index % columns);
  const uint8_t row = static_cast<uint8_t>(index / columns);
  return fui::Rect{static_cast<int16_t>(rect.x + metrics.sideInset + column * (width + theme.spaceMd)),
                   static_cast<int16_t>(rect.y + metrics.gap + row * (metrics.button + metrics.gap)), width,
                   metrics.button};
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
    // The title between the two buttons: a tap there opens the book's details. Inverted under the keys' cursor.
    const fui::Rect title{static_cast<int16_t>(top.x + metrics.topBar), top.y,
                          static_cast<int16_t>(top.width - 2 * metrics.topBar),
                          static_cast<int16_t>(metrics.topBar - metrics.rule)};
    const bool titleFocused = model.focus == Focus::Title;
    if (titleFocused) target.fill(title.inset(fui::Insets{4, 0, 4, 0}), black, 8);
    fui::TextStyle titleStyle = theme.bodyText;
    titleStyle.align = fui::TextAlign::Center;
    titleStyle.color = titleFocused ? fui::Color::White : fui::Color::Black;
    target.text(title, model.title ? model.title : "", titleStyle);
    screen.frame().hit(title, ACTION_BAR, static_cast<int16_t>(Button::Info));
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
  if (model.panel == Panel::Navigation || model.panel == Panel::Orientation) {
    const fui::Rect rect = listRect(area, model, metrics);
    const uint8_t columns = listColumns(model, metrics, area.height);
    for (uint8_t i = 0; i < model.listCount && i < kMaxListRows; ++i) {
      const ListRow& row = model.list[i];
      const fui::Rect cell = listCell(rect, metrics, theme, columns, i);
      const bool focused = model.focus == Focus::List && model.listFocus == i;
      textButton(screen, cell, row.label, row.button, focused);
      // The orientation the book is read in: a dot at the row's start, in the button's own ink.
      if (row.marked) {
        const int16_t dot = static_cast<int16_t>(std::max<int>(8, metrics.button / 5));
        target.fill(fui::Rect{static_cast<int16_t>(cell.x + theme.spaceMd),
                              static_cast<int16_t>(cell.y + (cell.height - dot) / 2), dot, dot},
                    focused ? white : black, static_cast<int16_t>(dot / 2));
      }
    }
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
  if (!showsSyncRow(model, metrics, area.height)) return;
  // KOReader sync: pull, push, half the width each.
  y = static_cast<int16_t>(y + metrics.button + metrics.gap);
  const int16_t half = static_cast<int16_t>((width - theme.spaceMd) / 2);
  textButton(screen, fui::Rect{left, y, half, metrics.button}, model.syncPullLabel, Button::SyncPull,
             model.focus == Focus::SyncPull);
  textButton(screen, fui::Rect{static_cast<int16_t>(left + width - half), y, half, metrics.button}, model.syncPushLabel,
             Button::SyncPush, model.focus == Focus::SyncPush);
}

}  // namespace ReaderBarsLayout
