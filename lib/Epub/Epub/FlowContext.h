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
  static bool pageProgressionRtlFor(VerticalText request, bool vertical, PageProgression ppd);
  static FlowContext of(VerticalText request, bool vertical, PageProgression ppd);
};
