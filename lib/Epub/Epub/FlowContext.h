#pragma once

#include <cstdint>

#include "PageProgression.h"

enum class CssWritingMode : uint8_t;

// The reader's choice for a book, in CrossPointSettings::VERTICAL_TEXT's order (stored by number).
enum class VerticalText : uint8_t { Automatic = 0, On = 1, Off = 2 };

// How a section runs and turns (spec 5.1, 5.5): resolved once per book and section, kept in BuildParams, the property
// hash and the section header. Step 1 adds a right-to-left base direction for horizontal text.
struct FlowContext {
  bool vertical = false;            // set vertical-rl through the turned frame
  bool pageProgressionRtl = false;  // its pages turn right to left

  // The request a book's sections are built with: the book's override (-1 none, else a VerticalText), else the
  // setting; Off for a language that is never set vertically (TypographyProfile::verticalAllowed).
  static VerticalText requestFor(VerticalText setting, int8_t bookOverride, bool verticalAllowed);
  // Whether a section is set vertically, from the writing mode in effect at its <body> and the book's ppd.
  static bool verticalFor(VerticalText request, CssWritingMode rootMode, PageProgression ppd);
  // Whether a section's pages turn right to left. A vertical-rl section's do. A horizontal section of a right-to-left
  // book (`bookRtl`: see bookIsRtl) does unless the OPF says ltr. Otherwise a book under Vertical Text Off turns left
  // to right whatever its ppd (a CJK book read horizontally), and one that may be set vertically follows its ppd (a
  // horizontal colophon in a vertical book keeps the book's direction).
  static bool pageProgressionRtlFor(VerticalText request, bool vertical, PageProgression ppd, bool bookRtl = false);
  static FlowContext of(VerticalText request, bool vertical, PageProgression ppd, bool bookRtl = false);
  // Whether a book reads right to left: its language runs right to left (TypographyProfile::direction: Hebrew, Yiddish,
  // the Arabic-script languages), or its OPF says rtl and its language is never set vertically (so the ppd cannot
  // mean the Japanese vertical convention).
  static bool bookIsRtl(bool languageRtl, bool verticalAllowed, PageProgression ppd);
};
