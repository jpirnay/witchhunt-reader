#pragma once

#include <TextScale.h>

#include <cstdint>

// The reader's line-spacing steps, in the order the setting stores them (CrossPointSettings::LINE_COMPRESSION asserts
// it matches). Each is named by its percentage of Pct100, Normal. FromBook is Book's style: each block's CSS
// line-height, never tighter than Pct95. Pct95 to Pct130 are the steps once called Tight, Normal, Wide, Extra Wide and
// Widest, at the factors they had; files written before Pct90 and Pct145 to Pct175 existed, or before Pct80 and Pct85,
// are converted as they load (lineSpacingStepFromStored).
enum class LineSpacingStep : uint8_t {
  Pct80 = 0,
  Pct85,
  Pct90,
  Pct95,
  Pct100,
  Pct110,
  Pct120,
  Pct130,
  Pct145,
  Pct160,
  Pct175,
  FromBook
};

// What a step means for a book.
//  - Font: a factor on the body font's own line height: the step's percentage.
//  - BuiltinNotoSans: the same, with lower factors: the built-in Noto Sans's own line height is taller.
//  - CjkPitch: Chinese, Japanese and Korean books, a line pitch in ems of the body text. JLREQ and CLREQ set the gap
//    between lines at half to one character (a pitch of 1.5 to 2 em); a CJK face's own line height is about 1.45 em,
//    below that. The steps past Pct120 go past the range, for readers who want more air, and those under Pct95 fall
//    short of it, for readers who want more lines on a page.
// The percentage a step shows is the Font scale's; the other two follow it as closely as their own steps allow.
enum class LineSpacingScale : uint8_t { Font, BuiltinNotoSans, CjkPitch };

struct LineSpacing {
  // Line pitch over the body font's line height, for every line the book's line-height does not set.
  TextScale compression;
  // Book's style: no block's compression below this. 0 when the book's line-height is not read.
  TextScale bookFloor = TextScale::fromRaw(0);
  // The body text's em in pixels, which a book's line-height multiplies.
  uint16_t emPx = 0;
};

// Every font is converted or rasterised at 150 dpi (fontconvert.py, ttf::ppemFromPoints), so a point is 150/72 px.
constexpr uint16_t lineSpacingEmPx(const uint8_t pointSize) {
  return static_cast<uint16_t>((pointSize * 150 + 36) / 72);
}

// The steps with a percentage, Pct80 to Pct175: every option but Book's style.
constexpr uint8_t kLineSpacingPercentSteps = static_cast<uint8_t>(LineSpacingStep::FromBook);

namespace line_spacing_detail {
constexpr uint8_t kSteps = kLineSpacingPercentSteps;
// The options' labels. A number, not a word: it reads the same in every language and needs no translation.
constexpr const char* const kLabels[kSteps] = {"80%",  "85%",  "90%",  "95%",  "100%", "110%",
                                               "120%", "130%", "145%", "160%", "175%"};
// In percent of the body font's line height.
constexpr uint8_t kFontFactorPct[kSteps] = {80, 85, 90, 95, 100, 110, 120, 130, 145, 160, 175};
// Up to Pct100 a twentieth under the Font factor, from Pct110 on a tenth, as Tight to Widest were set.
constexpr uint8_t kNotoSansFactorPct[kSteps] = {75, 80, 85, 90, 95, 100, 110, 120, 135, 150, 165};
// In hundredths of an em. From Pct110 on, near Normal's 1.7 em times the step's percentage: 1.87, 2.04 and 2.21 em were
// set as 1.85, 2 and 2.2, and 2.465, 2.72 and 2.975 are set as 2.45, 2.7 and 3. Below Pct95's 1.5 em, a tenth of an em
// a step: a gap of 0.4 character at Pct90, 0.3 at Pct85 and 0.2 at Pct80.
constexpr uint16_t kCjkPitchHundredthsEm[kSteps] = {120, 130, 140, 150, 170, 185, 200, 220, 245, 270, 300};

constexpr TextScale compressionAt(const uint8_t step, const LineSpacingScale scale, const uint16_t emPx,
                                  const int fontLineHeightPx) {
  switch (scale) {
    case LineSpacingScale::Font:
      return TextScale::percent(kFontFactorPct[step]);
    case LineSpacingScale::BuiltinNotoSans:
      return TextScale::percent(kNotoSansFactorPct[step]);
    case LineSpacingScale::CjkPitch:
      return fontLineHeightPx > 0
                 ? TextScale::ratio(int64_t{kCjkPitchHundredthsEm[step]} * emPx, int64_t{100} * fontLineHeightPx)
                 : TextScale();
  }
  return TextScale();
}
}  // namespace line_spacing_detail

// The label of a step with a percentage ("110%"); nullptr for Book's style and past the steps. A plain function, so a
// settings row can take it as its literal options (SettingInfo::withLiteralOptions).
inline const char* lineSpacingStepLabel(const uint8_t step) {
  return step < line_spacing_detail::kSteps ? line_spacing_detail::kLabels[step] : nullptr;
}

// The spacing for a stored step. An unknown step reads as Normal.
constexpr LineSpacing lineSpacingFor(const uint8_t step, const LineSpacingScale scale, const uint8_t pointSize,
                                     const int fontLineHeightPx) {
  using namespace line_spacing_detail;
  constexpr auto normal = static_cast<uint8_t>(LineSpacingStep::Pct100);
  constexpr auto floor = static_cast<uint8_t>(LineSpacingStep::Pct95);
  LineSpacing ls;
  ls.emPx = lineSpacingEmPx(pointSize);
  if (step == static_cast<uint8_t>(LineSpacingStep::FromBook)) {
    ls.compression = compressionAt(normal, scale, ls.emPx, fontLineHeightPx);
    ls.bookFloor = compressionAt(floor, scale, ls.emPx, fontLineHeightPx);
  } else {
    ls.compression = compressionAt(step < kSteps ? step : normal, scale, ls.emPx, fontLineHeightPx);
  }
  return ls;
}

// Bumped when the meaning of a stored step changes. A settings or recent-books file stamped lower ("lineSpacingOrderV",
// absent in every file from before it) holds steps in an older meaning and is converted as it loads.
//
// 1 = Pct90 put under the old Tight, and Pct145 to Pct175 above the old Widest: the steps were renumbered so the list
//     runs in order, with Book's style last.
// 2 = Pct80 and Pct85 put under Pct90: every step of version 1 moves up two.
constexpr uint8_t LINE_SPACING_ORDER_VERSION = 2;
// What lineSpacingStepFromStored returns for a value it cannot use.
constexpr uint8_t kNoLineSpacingStep = 0xFF;

namespace line_spacing_detail {
// The steps as an older numbering stored them, in its order.
// Before version 1: Tight 0, Normal 1, Wide 2, Extra Wide 3, Widest 4, Book's style 5.
constexpr LineSpacingStep kOrderV0[] = {LineSpacingStep::Pct95,  LineSpacingStep::Pct100, LineSpacingStep::Pct110,
                                        LineSpacingStep::Pct120, LineSpacingStep::Pct130, LineSpacingStep::FromBook};
// Version 1: 90% 0 to 175% 8, Book's style 9.
constexpr LineSpacingStep kOrderV1[] = {LineSpacingStep::Pct90,   LineSpacingStep::Pct95,  LineSpacingStep::Pct100,
                                        LineSpacingStep::Pct110,  LineSpacingStep::Pct120, LineSpacingStep::Pct130,
                                        LineSpacingStep::Pct145,  LineSpacingStep::Pct160, LineSpacingStep::Pct175,
                                        LineSpacingStep::FromBook};

template <int N>
constexpr uint8_t stepInOrder(const LineSpacingStep (&order)[N], const int stored) {
  return stored >= 0 && stored < N ? static_cast<uint8_t>(order[stored]) : kNoLineSpacingStep;
}
}  // namespace line_spacing_detail

// A step read from a file stamped `fileVersion`, in today's numbering; kNoLineSpacingStep when the stored value is
// unusable: absent (pass -1) or out of range. The caller supplies the fallback, because it differs: a setting falls
// back to Pct100, a book's override to "follow the default".
constexpr uint8_t lineSpacingStepFromStored(const int stored, const uint8_t fileVersion) {
  constexpr int kFromBook = static_cast<int>(LineSpacingStep::FromBook);
  if (fileVersion >= LINE_SPACING_ORDER_VERSION) {
    return stored >= 0 && stored <= kFromBook ? static_cast<uint8_t>(stored) : kNoLineSpacingStep;
  }
  return fileVersion == 1 ? line_spacing_detail::stepInOrder(line_spacing_detail::kOrderV1, stored)
                          : line_spacing_detail::stepInOrder(line_spacing_detail::kOrderV0, stored);
}
