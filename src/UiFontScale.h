#pragma once

#include <cstdint>
#include <iterator>

#include "components/themes/BaseTheme.h"

// The UI font ladder, and what a step up it does to layout.
//
// SMALL_FONT_ID, UI_10_FONT_ID and UI_12_FONT_ID are LOGICAL slots, not fixed faces: they are
// named after the family they carry at the DEFAULT step, and each step up SETTINGS.uiFontSize
// rebinds them one rung higher. Keeping them logical is what makes the setting a two-line
// change instead of a rename across ~600 call sites.
// Font IDs bound permanently to the DEFAULT step's faces, whatever SETTINGS.uiFontSize is.
//
// For chrome whose geometry is FIXED and cannot grow with its text: the button hint boxes, and a
// dashboard like the weather screen whose panels are sized in constants. Growing the text inside
// a box that cannot grow does not make it more readable, it makes it overlap the line below and
// the box beside it.
//
// The faces are compiled in either way -- the default step draws everything with them -- so this
// costs three entries in the font map and no flash. Not hashed names like the ids in fontIds.h
// (that file is generated from the font files, and these are second bindings of faces already in
// it), so they are small constants chosen to be nowhere near the hash space.
inline constexpr int FIT_SMALL_FONT_ID = -1001;
inline constexpr int FIT_BODY_FONT_ID = -1002;
inline constexpr int FIT_TITLE_FONT_ID = -1003;

namespace UiFontLadder {

// Line height (EpdFontData::advanceY) of each slot at each step, indexed by
// CrossPointSettings::UI_FONT_SIZE — kept as a plain index rather than that enum so this header
// needs nothing but ThemeMetrics. UITheme.cpp static_asserts that the two stay the same length.
//
// This table exists so the layout code and the binding code read the same numbers: the metrics
// grow by the DIFFERENCE between the active step and the default one, which is only correct if
// it matches the families applyUiFontScale() actually binds. applyUiFontScale() checks the live
// fonts against this table at every bind and logs if they ever disagree, so a regenerated font
// that changes advanceY cannot silently skew every menu.
struct Step {
  int small;  // SMALL_FONT_ID
  int body;   // UI_10_FONT_ID — list rows, values, button hints
  int title;  // UI_12_FONT_ID — headers, titles of rows that have a subtitle
};
inline constexpr Step STEPS[] = {
    {23, 25, 30},  // DEFAULT: notosans_8,  inter_ui_10, inter_ui_12
    {25, 30, 35},  // LARGE:   inter_ui_10, inter_ui_12, inter_ui_14
};
inline constexpr size_t STEP_COUNT = std::size(STEPS);

// Extra pixels each slot occupies at `step` relative to step 0. All three are zero at step 0, so
// every layout constant written against the default keeps its meaning. An out-of-range step (a
// settings file from a newer firmware, or a corrupt byte) reads as the default.
constexpr Step growthAt(const uint8_t step) {
  const Step& active = STEPS[step < STEP_COUNT ? step : 0];
  const Step& base = STEPS[0];
  return {active.small - base.small, active.body - base.body, active.title - base.title};
}

// Grows every metric that has to hold a line of UI text by the growth of the slot whose font
// that text is drawn in. Additive rather than a ratio on purpose: the fonts step by a fixed
// number of pixels, so a row needs exactly those pixels. Scaling by 1.17x instead would add
// 14 px of dead space to an 84 px header to fit 5 px of extra glyph.
//
// Deliberately NOT scaled: the reader status bar (progressBarHeight, statusBarVerticalMargin,
// batteryBarHeight) and the cover/home geometry. The status bar feeds the reader's text
// viewport, and moving the viewport invalidates every book's pagination cache — this setting is
// about hitting menu rows, not about re-laying-out books. The status bar's own text still grows
// with SMALL_FONT_ID and fits: getStatusBarItemsHeight() gives it a 31 px lane.
constexpr void applyTo(ThemeMetrics& metrics, const Step& growth) {
  // One line of body text.
  metrics.listRowHeight += growth.body;
  metrics.menuRowHeight += growth.body;
  metrics.tabBarHeight += growth.body;
  metrics.buttonHintsHeight += growth.body;
  metrics.keyboardKeyHeight += growth.body;

  // A title line plus up to two subtitle lines, the worst case drawList() paints.
  metrics.listWithSubtitleRowHeight += growth.body + growth.small * 2;

  // A title line plus an optional subtitle line.
  metrics.headerHeight += growth.title + growth.small;

  // The subtitle's own line, where the theme keeps one (0 stays 0: that theme draws it in the band).
  if (metrics.headerSubtitleLine > 0) metrics.headerSubtitleLine += growth.small;
}

}  // namespace UiFontLadder

// Binds the three logical UI font IDs to the ladder step SETTINGS.uiFontSize selects.
//
// Defined in main.cpp, next to the font family globals it binds — same arrangement as
// SilentRestart.h. Safe to call repeatedly; call it after changing SETTINGS.uiFontSize, and
// call UITheme::reload() alongside it so the metrics follow the fonts.
void applyUiFontScale();
