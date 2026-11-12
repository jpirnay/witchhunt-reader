#pragma once

#include <Epub/LineSpacing.h>

#include <cstdint>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "ReaderLayoutSheet.h"
#include "ReaderLayoutValues.h"

// What a press on one of the Layout tab's controls (ReaderLayout::Control, ReaderLayoutSheet.h) does to the values
// behind it (ReaderLayoutValues).
namespace ReaderLayout {

// Text darkness from lightest to darkest. The stored numbers are not in that order: Light came last
// (CrossPointSettings::TEXT_DARKNESS).
constexpr uint8_t kDarknessOrder[] = {CrossPointSettings::DARKNESS_LIGHT, CrossPointSettings::DARKNESS_NORMAL,
                                      CrossPointSettings::DARKNESS_DARK, CrossPointSettings::DARKNESS_EXTRA_DARK,
                                      CrossPointSettings::DARKNESS_MAXIMUM};
constexpr uint8_t kDarknessSteps = sizeof(kDarknessOrder);
static_assert(kDarknessSteps == CrossPointSettings::TEXT_DARKNESS_COUNT, "every darkness has its place in the order");

inline uint8_t darknessRank(const uint8_t darkness) {
  for (uint8_t i = 0; i < kDarknessSteps; ++i) {
    if (kDarknessOrder[i] == darkness) return i;
  }
  return 2;  // DARKNESS_DARK, the default, for a value no setting holds
}

// Changes `values` for one press on `control`: `delta` -1 or +1 on a stepper (stopping at either end), ignored on the
// others. `sizes` are the sizes the book's font offers (sdFontSystem.sizeListFor), which the size steps through as the
// reader's own size keys do. True when anything changed.
inline bool press(ReaderLayoutValues& values, const Control control, const int delta, const ReaderSizeList& sizes) {
  const ReaderLayoutValues before = values;
  switch (control) {
    case Control::Size:
      values.fontSize = sizes.step(sizes.snap(values.fontSize), delta);
      break;
    case Control::Line: {
      constexpr int kLast = static_cast<int>(LineSpacingStep::FromBook);
      const int next = static_cast<int>(values.lineSpacing) + delta;
      if (next >= 0 && next <= kLast) values.lineSpacing = static_cast<uint8_t>(next);
      break;
    }
    case Control::Margin: {
      const int next = static_cast<int>(values.marginHorizontal) + delta * CrossPointSettings::SCREEN_MARGIN_STEP;
      if (next >= CrossPointSettings::SCREEN_MARGIN_MIN && next <= CrossPointSettings::SCREEN_MARGIN_MAX) {
        values.marginHorizontal = static_cast<uint8_t>(next);
      }
      break;
    }
    case Control::Darkness: {
      const int next = static_cast<int>(darknessRank(values.textDarkness)) + delta;
      if (next >= 0 && next < kDarknessSteps) values.textDarkness = kDarknessOrder[next];
      break;
    }
    case Control::Paragraph:
      values.extraParagraphSpacing = !values.extraParagraphSpacing;
      break;
    case Control::Align:
      values.paragraphAlignment =
          static_cast<uint8_t>((values.paragraphAlignment + 1) % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
      break;
    case Control::Hyphenation:
      values.hyphenation = !values.hyphenation;
      break;
    case Control::BookStyle:
      values.embeddedStyle = !values.embeddedStyle;
      break;
    case Control::Count:
      break;
  }
  return !(values == before);
}

// True when `a` and `b` lay a book out differently. Darkness only changes how the page is drawn.
inline bool laysOutDifferently(const ReaderLayoutValues& a, const ReaderLayoutValues& b) {
  ReaderLayoutValues drawnAlike = b;
  drawnAlike.textDarkness = a.textDarkness;
  return !(a == drawnAlike);
}

}  // namespace ReaderLayout
