// The reader's line-spacing steps as a compression on the body font's line height: factors for most books, an em pitch
// for Chinese, Japanese and Korean books (JLREQ and CLREQ: a line gap of half to one character), and Book's style.
// The CJK cases use the X3 study's metrics: Noto Sans CJK at 14 pt, em 29 px, line height 42 px.
#include <gtest/gtest.h>

#include "Epub/LineSpacing.h"

namespace {

constexpr uint8_t kSteps = 5;  // Tight to Widest; Book's style is tested on its own

uint8_t step(const LineSpacingStep s) { return static_cast<uint8_t>(s); }

// The line pitch the parser lays out for a compression (ChapterHtmlSlimParser::effectiveLineHeight).
int pitch(const int lineHeight, const float compression) { return static_cast<int>(lineHeight * compression + 0.5f); }

TEST(LineSpacing, FontFactorsPerStep) {
  const float expected[kSteps] = {0.95f, 1.0f, 1.1f, 1.2f, 1.3f};
  for (uint8_t s = 0; s < kSteps; ++s) {
    const LineSpacing ls = lineSpacingFor(s, LineSpacingScale::Font, 14, 30);
    EXPECT_FLOAT_EQ(ls.compression, expected[s]) << "step " << int{s};
    EXPECT_EQ(ls.bookFloor, 0.0f) << "step " << int{s};
  }
}

TEST(LineSpacing, BuiltinNotoSansFactorsPerStep) {
  const float expected[kSteps] = {0.90f, 0.95f, 1.0f, 1.1f, 1.2f};
  for (uint8_t s = 0; s < kSteps; ++s) {
    EXPECT_FLOAT_EQ(lineSpacingFor(s, LineSpacingScale::BuiltinNotoSans, 14, 30).compression, expected[s])
        << "step " << int{s};
  }
}

TEST(LineSpacing, CjkPitchPerStep) {
  const float pitchEm[kSteps] = {1.5f, 1.7f, 1.85f, 2.0f, 2.2f};
  for (uint8_t s = 0; s < kSteps; ++s) {
    const LineSpacing ls = lineSpacingFor(s, LineSpacingScale::CjkPitch, 14, 42);
    EXPECT_NEAR(ls.compression, pitchEm[s] * 29.0f / 42.0f, 1e-5f) << "step " << int{s};
    EXPECT_EQ(ls.bookFloor, 0.0f) << "step " << int{s};
  }
  // 1.7 em of 29 px is 49.3, 1.85 em 53.65, 2 em 58, 2.2 em 63.8. Tight is 43.5 px, a half pixel the float product may
  // put on either side, so only its compression is asserted above.
  EXPECT_EQ(pitch(42, lineSpacingFor(step(LineSpacingStep::Normal), LineSpacingScale::CjkPitch, 14, 42).compression),
            49);
  EXPECT_EQ(pitch(42, lineSpacingFor(step(LineSpacingStep::Wide), LineSpacingScale::CjkPitch, 14, 42).compression), 54);
  EXPECT_EQ(pitch(42, lineSpacingFor(step(LineSpacingStep::ExtraWide), LineSpacingScale::CjkPitch, 14, 42).compression),
            58);
  EXPECT_EQ(pitch(42, lineSpacingFor(step(LineSpacingStep::Widest), LineSpacingScale::CjkPitch, 14, 42).compression),
            64);
}

TEST(LineSpacing, CjkPitchIsTheSameEmWhateverTheFont) {
  // A CJK face (line height 42) and a Latin face drawing the book through CJK fallback (35): both 1.7 em of 29 px.
  const uint8_t normal = step(LineSpacingStep::Normal);
  EXPECT_EQ(pitch(42, lineSpacingFor(normal, LineSpacingScale::CjkPitch, 14, 42).compression), 49);
  EXPECT_EQ(pitch(35, lineSpacingFor(normal, LineSpacingScale::CjkPitch, 14, 35).compression), 49);
}

TEST(LineSpacing, EmIsPointsAt150Dpi) {
  // 12 pt = 25 px, 14 pt = 29.17 px, 18 pt = 37.5 px, rounded as ttf::ppemFromPoints rounds.
  EXPECT_EQ(lineSpacingEmPx(12), 25);
  EXPECT_EQ(lineSpacingEmPx(14), 29);
  EXPECT_EQ(lineSpacingEmPx(18), 38);
  EXPECT_EQ(lineSpacingFor(step(LineSpacingStep::Normal), LineSpacingScale::Font, 14, 30).emPx, 29);
}

TEST(LineSpacing, BookStyleIsNormalFlooredAtTight) {
  const uint8_t book = step(LineSpacingStep::FromBook);
  const uint8_t normal = step(LineSpacingStep::Normal);
  const uint8_t tight = step(LineSpacingStep::Tight);
  for (const LineSpacingScale scale :
       {LineSpacingScale::Font, LineSpacingScale::BuiltinNotoSans, LineSpacingScale::CjkPitch}) {
    const LineSpacing ls = lineSpacingFor(book, scale, 14, 42);
    EXPECT_FLOAT_EQ(ls.compression, lineSpacingFor(normal, scale, 14, 42).compression);
    EXPECT_FLOAT_EQ(ls.bookFloor, lineSpacingFor(tight, scale, 14, 42).compression);
    EXPECT_EQ(ls.emPx, 29);
  }
}

TEST(LineSpacing, OutOfRangeStepIsNormal) {
  const LineSpacing ls = lineSpacingFor(9, LineSpacingScale::Font, 14, 30);
  EXPECT_FLOAT_EQ(ls.compression, 1.0f);
  EXPECT_EQ(ls.bookFloor, 0.0f);
}

TEST(LineSpacing, NoFontLineHeightFallsBack) {
  EXPECT_FLOAT_EQ(lineSpacingFor(step(LineSpacingStep::Wide), LineSpacingScale::CjkPitch, 14, 0).compression, 1.0f);
}

}  // namespace
