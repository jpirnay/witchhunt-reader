#pragma once

#include <FreeInkApp.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

// The reader bars' Layout tab, laid out: FreeInkUI only, like ReaderBarsLayout, which draws it above its tab bar, and
// laid out by test/reader_bars_layout at every board's size.
//
// A grid of cells, each a label over its value:
//  - first a row of two: the font, which hands over to the font list, and "This book only";
//  - then the four steppers (size, line spacing, margin, darkness), each [-] label/value [+];
//  - then the four that change with one press (paragraph spacing, alignment, hyphenation, the book's style).
// As many cells a row as the width takes: two steppers and four of the others on a portrait X4, one stepper a row on
// the Murphy, four of each on a landscape X4.

namespace ReaderLayout {

// The controls, steppers first. The values behind them are ReaderLayoutValues (ReaderLayoutValues.h).
enum class Control : uint8_t { Size, Line, Margin, Darkness, Paragraph, Align, Hyphenation, BookStyle, Count };
constexpr uint8_t kControlCount = static_cast<uint8_t>(Control::Count);
constexpr uint8_t kStepperCount = 4;
constexpr bool isStepper(const Control control) { return static_cast<uint8_t>(control) < kStepperCount; }

}  // namespace ReaderLayout

namespace ReaderLayoutSheet {
namespace fui = freeink::ui;
using ReaderLayout::Control;
using ReaderLayout::kControlCount;
using ReaderLayout::kStepperCount;

// One action for the whole sheet, its value naming the target: FreeInkApp keeps 8 handlers a screen and drops any past
// that without a sign, so the bars and the sheet share three (ReaderBarsLayout).
constexpr fui::ActionId ACTION_SHEET = 4;
// The font cell (the next family) and "This book only".
constexpr int16_t kFontValue = 0x40;
constexpr int16_t kThisBookValue = 0x41;
// A control: its number, with kUpFlag for a step up. A one-press control fires its step up.
constexpr int16_t kUpFlag = 0x20;
constexpr int16_t stepValue(const Control control, const bool up) {
  return static_cast<int16_t>(static_cast<uint8_t>(control) | (up ? kUpFlag : 0));
}

struct Cell {
  const char* label = nullptr;
  const char* value = nullptr;
};

// What the sheet shows. The strings are the caller's, translated and formatted, alive until build() returns.
struct Model {
  Cell font;
  Cell thisBook;
  Cell controls[kControlCount];
  // The key target (targets()) the cursor is on, -1 for none.
  int16_t focus = -1;
};

struct Metrics {
  int16_t row = 0;    // a row of cells
  int16_t step = 0;   // the width of a stepper's [-] and [+]
  int16_t gap = 0;    // between rows, and above and below them
  int16_t inset = 0;  // between the screen's sides and the cells
  uint8_t stepperColumns = 2;
  uint8_t pressColumns = 4;
  // A cell's label and value on one line, label left and value right, where a label over its value leaves the sheet
  // too tall for the screen (the Murphy at the large UI font).
  bool singleLine = false;
};

inline uint8_t stepperRows(const Metrics& m);
inline uint8_t pressRows(const Metrics& m);
inline int16_t height(const Metrics& m);

// `available` is the height the sheet may take: the screen's, less the tab bar and its rule.
inline Metrics metricsFor(const fui::DrawTarget& target, const fui::ThemeTokens& theme, const int16_t width,
                          const int16_t available, const bool touch) {
  Metrics metrics;
  const int small = target.lineHeight(theme.smallText.font);
  const int body = target.lineHeight(theme.bodyText.font);
  const int minRow = touch ? static_cast<int>(theme.minTouchSize) : 0;
  metrics.row = static_cast<int16_t>(std::max(small + body + theme.spaceSm * 3, minRow));
  metrics.step = static_cast<int16_t>(std::max(metrics.row * 3 / 4, touch ? theme.minTouchSize * 3 / 4 : 0));
  const bool narrow = width < 400;
  metrics.gap = static_cast<int16_t>(narrow ? theme.spaceSm : theme.spaceMd);
  metrics.inset = static_cast<int16_t>(narrow ? theme.spaceSm : theme.spaceLg);
  const int room = width - 2 * metrics.inset;
  // A stepper wants its two buttons and about four characters of value between them; a one-press cell its label
  // ("Paragraph gap" was cut short in a quarter of a portrait X4).
  const int stepperCell = 2 * metrics.step + 4 * body;
  const int pressCell = body * 11 / 2;
  // As many a row as fit, then spread evenly: four cells with room for three a row make two rows of two, not three and
  // a lone one with the rest of its row empty.
  const auto balanced = [](const int count, const int fit) {
    const int rows = (count + fit - 1) / fit;
    return static_cast<uint8_t>((count + rows - 1) / rows);
  };
  metrics.stepperColumns = balanced(kStepperCount, std::clamp(room / std::max(stepperCell, 1), 1, 4));
  metrics.pressColumns = balanced(kControlCount - kStepperCount, std::clamp(room / std::max(pressCell, 1), 2, 4));
  if (height(metrics) > available) {
    metrics.singleLine = true;
    metrics.row = static_cast<int16_t>(std::max(body + theme.spaceSm * 2, minRow));
    metrics.step = static_cast<int16_t>(std::max(metrics.row * 3 / 4, touch ? theme.minTouchSize * 3 / 4 : 0));
  }
  return metrics;
}

inline uint8_t stepperRows(const Metrics& m) {
  return static_cast<uint8_t>((kStepperCount + m.stepperColumns - 1) / m.stepperColumns);
}
inline uint8_t pressRows(const Metrics& m) {
  constexpr uint8_t kPressCount = kControlCount - kStepperCount;
  return static_cast<uint8_t>((kPressCount + m.pressColumns - 1) / m.pressColumns);
}
inline uint8_t rowCount(const Metrics& m) { return static_cast<uint8_t>(1 + stepperRows(m) + pressRows(m)); }

// The height the sheet takes above the tab bar: its rows, the gaps between them and one gap above and below.
inline int16_t height(const Metrics& m) {
  const int rows = rowCount(m);
  return static_cast<int16_t>(rows * m.row + (rows + 1) * m.gap);
}

// The cells: 0 the font, 1 "This book only", then 2 + each Control.
constexpr uint8_t kCellCount = 2 + kControlCount;

inline fui::Rect cellRect(const fui::Rect& sheet, const Metrics& m, const uint8_t cell) {
  uint8_t row = 0;
  uint8_t column = 0;
  uint8_t columns = 2;
  if (cell >= 2) {
    const uint8_t control = static_cast<uint8_t>(cell - 2);
    if (control < kStepperCount) {
      columns = m.stepperColumns;
      row = static_cast<uint8_t>(1 + control / columns);
      column = static_cast<uint8_t>(control % columns);
    } else {
      columns = m.pressColumns;
      const uint8_t index = static_cast<uint8_t>(control - kStepperCount);
      row = static_cast<uint8_t>(1 + stepperRows(m) + index / columns);
      column = static_cast<uint8_t>(index % columns);
    }
  } else {
    column = cell;
  }
  const int room = sheet.width - 2 * m.inset;
  const int width = (room - (columns - 1) * m.gap) / columns;
  return fui::Rect{static_cast<int16_t>(sheet.x + m.inset + column * (width + m.gap)),
                   static_cast<int16_t>(sheet.y + m.gap + row * (m.row + m.gap)), static_cast<int16_t>(width), m.row};
}

// What a key press can land on, in reading order: what Confirm there fires, the row it sits in, and its centre across
// the screen in permille, by which Up and Down pick the nearest in the next row.
struct Target {
  fui::ActionId action = fui::NO_ACTION;
  int16_t value = 0;
  uint8_t row = 0;
  int16_t centre = 0;
  fui::Rect rect{};
};
constexpr uint8_t kMaxTargets = 2 + 2 * kStepperCount + (kControlCount - kStepperCount);

inline uint8_t targets(const fui::Rect& sheet, const Metrics& m, Target (&out)[kMaxTargets]) {
  uint8_t count = 0;
  const auto add = [&](const fui::ActionId action, const int16_t value, const fui::Rect& rect) {
    Target& target = out[count++];
    target.action = action;
    target.value = value;
    target.rect = rect;
    target.row = static_cast<uint8_t>((rect.y - sheet.y - m.gap) / (m.row + m.gap));
    target.centre = static_cast<int16_t>((rect.x + rect.width / 2 - sheet.x) * 1000 / std::max<int>(sheet.width, 1));
  };
  add(ACTION_SHEET, kFontValue, cellRect(sheet, m, 0));
  add(ACTION_SHEET, kThisBookValue, cellRect(sheet, m, 1));
  for (uint8_t control = 0; control < kControlCount; ++control) {
    const fui::Rect cell = cellRect(sheet, m, static_cast<uint8_t>(2 + control));
    const auto c = static_cast<Control>(control);
    if (ReaderLayout::isStepper(c)) {
      add(ACTION_SHEET, stepValue(c, false), fui::Rect{cell.x, cell.y, m.step, cell.height});
      add(ACTION_SHEET, stepValue(c, true),
          fui::Rect{static_cast<int16_t>(cell.right() - m.step), cell.y, m.step, cell.height});
    } else {
      add(ACTION_SHEET, stepValue(c, true), cell);
    }
  }
  return count;
}

// The target to go to from `from` on a press of Up (-1) or Down (+1): the one nearest across in the row above or
// below. -1 when there is no such row: the cursor leaves the sheet, for the top bar or the tab bar.
inline int16_t targetAcross(const Target (&all)[kMaxTargets], const uint8_t count, const int16_t from,
                            const int direction) {
  if (from < 0 || from >= count) return -1;
  const int row = all[from].row + direction;
  int16_t best = -1;
  int bestDistance = 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (all[i].row != row) continue;
    const int distance = std::abs(all[i].centre - all[from].centre);
    if (best < 0 || distance < bestDistance) {
      best = static_cast<int16_t>(i);
      bestDistance = distance;
    }
  }
  return best;
}

// The target along the row from `from`, `step` -1 or +1, stopping at the row's ends.
inline int16_t targetAlong(const Target (&all)[kMaxTargets], const uint8_t count, const int16_t from, const int step) {
  if (from < 0 || from >= count) return -1;
  const int next = from + step;
  if (next < 0 || next >= count || all[next].row != all[from].row) return from;
  return static_cast<int16_t>(next);
}

namespace detail {

inline void drawText(fui::DrawTarget& target, const fui::Rect& rect, const char* text, fui::TextStyle style,
                     const bool inverted) {
  if (!text) return;
  style.align = fui::TextAlign::Center;
  style.color = inverted ? fui::Color::White : fui::Color::Black;
  target.text(rect, text, style);
}

// A label over its value, centred in `rect`; or on one line, the label left and the value right.
inline void drawLabelAndValue(fui::DrawTarget& target, const fui::ThemeTokens& theme, const fui::Rect& rect,
                              const Cell& cell, const bool inverted, const bool singleLine) {
  fui::TextStyle label = theme.smallText;
  fui::TextStyle value = theme.bodyText;
  label.color = value.color = inverted ? fui::Color::White : fui::Color::Black;
  fui::Rect labelRect = rect;
  fui::Rect valueRect = rect;
  if (singleLine) {
    const fui::Rect inner = rect.inset(fui::Insets{0, theme.spaceSm, 0, theme.spaceSm});
    labelRect = valueRect = inner;
    labelRect.width = static_cast<int16_t>(inner.width / 2);
    valueRect.x = static_cast<int16_t>(inner.x + labelRect.width);
    valueRect.width = static_cast<int16_t>(inner.width - labelRect.width);
    value.align = fui::TextAlign::Right;
  } else {
    labelRect.height = target.lineHeight(label.font);
    valueRect.height = target.lineHeight(value.font);
    labelRect.y = static_cast<int16_t>(rect.y + (rect.height - labelRect.height - valueRect.height) / 2);
    valueRect.y = static_cast<int16_t>(labelRect.y + labelRect.height);
    label.align = value.align = fui::TextAlign::Center;
  }
  if (cell.label) target.text(labelRect, cell.label, label);
  if (cell.value) target.text(valueRect, cell.value, value);
}

}  // namespace detail

// Draws the sheet into `sheet` (its background is the caller's) and registers its targets.
template <typename ScreenT>
void build(ScreenT& screen, const fui::Rect& sheet, const Model& model, const Metrics& m) {
  const fui::ThemeTokens& theme = screen.theme();
  fui::DrawTarget& target = screen.target();
  const fui::Paint black = fui::Paint::solid(fui::Color::Black);
  constexpr uint8_t kRadius = 8;
  Target all[kMaxTargets];
  const uint8_t count = targets(sheet, m, all);
  const auto focused = [&](const uint8_t index) { return model.focus == static_cast<int16_t>(index); };

  uint8_t index = 0;
  for (uint8_t cell = 0; cell < kCellCount; ++cell) {
    const fui::Rect rect = cellRect(sheet, m, cell);
    const Cell& content = cell == 0 ? model.font : cell == 1 ? model.thisBook : model.controls[cell - 2];
    const bool stepper = cell >= 2 && ReaderLayout::isStepper(static_cast<Control>(cell - 2));
    if (!stepper) {
      const bool inverted = focused(index);
      if (inverted) target.fill(rect, black, kRadius);
      target.stroke(rect, black, 2, kRadius);
      detail::drawLabelAndValue(target, theme, rect, content, inverted, m.singleLine);
      screen.frame().hit(rect, all[index].action, all[index].value);
      ++index;
      continue;
    }
    target.stroke(rect, black, 2, kRadius);
    for (const char* sign : {"-", "+"}) {
      const Target& button = all[index];
      const bool inverted = focused(index);
      if (inverted) target.fill(button.rect.inset(fui::Insets{3, 3, 3, 3}), black, kRadius - 2);
      detail::drawText(target, button.rect, sign, theme.titleText, inverted);
      screen.frame().hit(button.rect, button.action, button.value);
      ++index;
    }
    const fui::Rect middle{static_cast<int16_t>(rect.x + m.step), rect.y, static_cast<int16_t>(rect.width - 2 * m.step),
                           rect.height};
    detail::drawLabelAndValue(target, theme, middle, content, false, m.singleLine);
  }
  (void)count;
}

}  // namespace ReaderLayoutSheet
