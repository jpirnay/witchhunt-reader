// The reader's line-spacing steps as a compression on the body font's line height: factors for most books, an em pitch
// for Chinese, Japanese and Korean books (JLREQ and CLREQ: a line gap of half to one character), and Book's style.
// The CJK cases use the X3 study's metrics: Noto Sans CJK at 14 pt, em 29 px, line height 42 px.
#include <gtest/gtest.h>

#include <cstdlib>

#include "Epub/LineSpacing.h"

namespace {

constexpr uint8_t kSteps = kLineSpacingPercentSteps;  // 90% to 175%; Book's style is tested on its own

uint8_t step(const LineSpacingStep s) { return static_cast<uint8_t>(s); }

// The line pitch the parser lays out for a compression (ChapterHtmlSlimParser::effectiveLineHeight).
int pitch(const int lineHeight, const float compression) { return static_cast<int>(lineHeight * compression + 0.5f); }

TEST(LineSpacing, FontFactorsAreTheStepsPercentages) {
  const float expected[kSteps] = {0.90f, 0.95f, 1.0f, 1.1f, 1.2f, 1.3f, 1.45f, 1.6f, 1.75f};
  for (uint8_t s = 0; s < kSteps; ++s) {
    const LineSpacing ls = lineSpacingFor(s, LineSpacingScale::Font, 14, 30);
    EXPECT_FLOAT_EQ(ls.compression, expected[s]) << "step " << int{s};
    EXPECT_EQ(ls.bookFloor, 0.0f) << "step " << int{s};
  }
}

TEST(LineSpacing, BuiltinNotoSansFactorsPerStep) {
  const float expected[kSteps] = {0.85f, 0.90f, 0.95f, 1.0f, 1.1f, 1.2f, 1.35f, 1.5f, 1.65f};
  for (uint8_t s = 0; s < kSteps; ++s) {
    EXPECT_FLOAT_EQ(lineSpacingFor(s, LineSpacingScale::BuiltinNotoSans, 14, 30).compression, expected[s])
        << "step " << int{s};
  }
}

TEST(LineSpacing, CjkPitchPerStep) {
  const float pitchEm[kSteps] = {1.4f, 1.5f, 1.7f, 1.85f, 2.0f, 2.2f, 2.45f, 2.7f, 3.0f};
  for (uint8_t s = 0; s < kSteps; ++s) {
    const LineSpacing ls = lineSpacingFor(s, LineSpacingScale::CjkPitch, 14, 42);
    EXPECT_NEAR(ls.compression, pitchEm[s] * 29.0f / 42.0f, 1e-5f) << "step " << int{s};
    EXPECT_EQ(ls.bookFloor, 0.0f) << "step " << int{s};
  }
  // 1.7 em of 29 px is 49.3, 1.85 em 53.65, 2 em 58, 2.2 em 63.8, 2.45 em 71.05, 2.7 em 78.3, 3 em 87. 1.4 em is 40.6
  // and 1.5 em 43.5 px, a half pixel the float product may put on either side, so only their compressions are asserted.
  const auto cjkPitch = [](const LineSpacingStep s) {
    return pitch(42, lineSpacingFor(step(s), LineSpacingScale::CjkPitch, 14, 42).compression);
  };
  EXPECT_EQ(cjkPitch(LineSpacingStep::Pct100), 49);
  EXPECT_EQ(cjkPitch(LineSpacingStep::Pct110), 54);
  EXPECT_EQ(cjkPitch(LineSpacingStep::Pct120), 58);
  EXPECT_EQ(cjkPitch(LineSpacingStep::Pct130), 64);
  EXPECT_EQ(cjkPitch(LineSpacingStep::Pct145), 71);
  EXPECT_EQ(cjkPitch(LineSpacingStep::Pct160), 78);
  EXPECT_EQ(cjkPitch(LineSpacingStep::Pct175), 87);
}

// The steps that existed before 90% and 145% to 175%: Tight, Normal, Wide, Extra Wide and Widest, at the factors they
// had, so a page laid out at one of them is laid out the same after the upgrade.
TEST(LineSpacing, TheFormerFiveStepsKeepTheirFactors) {
  const LineSpacingStep former[] = {LineSpacingStep::Pct95, LineSpacingStep::Pct100, LineSpacingStep::Pct110,
                                    LineSpacingStep::Pct120, LineSpacingStep::Pct130};
  const float font[] = {0.95f, 1.0f, 1.1f, 1.2f, 1.3f};
  const float noto[] = {0.90f, 0.95f, 1.0f, 1.1f, 1.2f};
  const float cjkEm[] = {1.5f, 1.7f, 1.85f, 2.0f, 2.2f};
  for (int i = 0; i < 5; ++i) {
    const uint8_t s = step(former[i]);
    EXPECT_FLOAT_EQ(lineSpacingFor(s, LineSpacingScale::Font, 14, 30).compression, font[i]) << i;
    EXPECT_FLOAT_EQ(lineSpacingFor(s, LineSpacingScale::BuiltinNotoSans, 14, 30).compression, noto[i]) << i;
    EXPECT_NEAR(lineSpacingFor(s, LineSpacingScale::CjkPitch, 14, 42).compression, cjkEm[i] * 29.0f / 42.0f, 1e-5f)
        << i;
  }
}

TEST(LineSpacing, EveryScaleWidensStepByStep) {
  for (const LineSpacingScale scale :
       {LineSpacingScale::Font, LineSpacingScale::BuiltinNotoSans, LineSpacingScale::CjkPitch}) {
    for (uint8_t s = 1; s < kSteps; ++s) {
      EXPECT_LT(lineSpacingFor(s - 1, scale, 14, 42).compression, lineSpacingFor(s, scale, 14, 42).compression)
          << "scale " << static_cast<int>(scale) << " step " << int{s};
    }
  }
}

TEST(LineSpacing, LabelsArePercentagesOfNormal) {
  const char* const expected[kSteps] = {"90%", "95%", "100%", "110%", "120%", "130%", "145%", "160%", "175%"};
  for (uint8_t s = 0; s < kSteps; ++s) {
    ASSERT_NE(lineSpacingStepLabel(s), nullptr) << "step " << int{s};
    EXPECT_STREQ(lineSpacingStepLabel(s), expected[s]);
    // The label says the Font factor.
    EXPECT_NEAR(lineSpacingFor(s, LineSpacingScale::Font, 14, 30).compression * 100.0f, std::atoi(expected[s]), 1e-3f);
  }
  EXPECT_EQ(lineSpacingStepLabel(step(LineSpacingStep::FromBook)), nullptr);
}

TEST(LineSpacing, CjkPitchIsTheSameEmWhateverTheFont) {
  // A CJK face (line height 42) and a Latin face drawing the book through CJK fallback (35): both 1.7 em of 29 px.
  const uint8_t normal = step(LineSpacingStep::Pct100);
  EXPECT_EQ(pitch(42, lineSpacingFor(normal, LineSpacingScale::CjkPitch, 14, 42).compression), 49);
  EXPECT_EQ(pitch(35, lineSpacingFor(normal, LineSpacingScale::CjkPitch, 14, 35).compression), 49);
}

TEST(LineSpacing, EmIsPointsAt150Dpi) {
  // 12 pt = 25 px, 14 pt = 29.17 px, 18 pt = 37.5 px, rounded as ttf::ppemFromPoints rounds.
  EXPECT_EQ(lineSpacingEmPx(12), 25);
  EXPECT_EQ(lineSpacingEmPx(14), 29);
  EXPECT_EQ(lineSpacingEmPx(18), 38);
  EXPECT_EQ(lineSpacingFor(step(LineSpacingStep::Pct100), LineSpacingScale::Font, 14, 30).emPx, 29);
}

TEST(LineSpacing, BookStyleIsNormalFlooredAtNinetyFivePercent) {
  const uint8_t book = step(LineSpacingStep::FromBook);
  const uint8_t normal = step(LineSpacingStep::Pct100);
  const uint8_t floor = step(LineSpacingStep::Pct95);
  for (const LineSpacingScale scale :
       {LineSpacingScale::Font, LineSpacingScale::BuiltinNotoSans, LineSpacingScale::CjkPitch}) {
    const LineSpacing ls = lineSpacingFor(book, scale, 14, 42);
    EXPECT_FLOAT_EQ(ls.compression, lineSpacingFor(normal, scale, 14, 42).compression);
    EXPECT_FLOAT_EQ(ls.bookFloor, lineSpacingFor(floor, scale, 14, 42).compression);
    EXPECT_EQ(ls.emPx, 29);
  }
}

TEST(LineSpacing, OutOfRangeStepIsNormal) {
  const LineSpacing ls = lineSpacingFor(12, LineSpacingScale::Font, 14, 30);
  EXPECT_FLOAT_EQ(ls.compression, 1.0f);
  EXPECT_EQ(ls.bookFloor, 0.0f);
}

TEST(LineSpacing, NoFontLineHeightFallsBack) {
  EXPECT_FLOAT_EQ(lineSpacingFor(step(LineSpacingStep::Pct110), LineSpacingScale::CjkPitch, 14, 0).compression, 1.0f);
}

// --- stored values (LINE_SPACING_ORDER_VERSION)
// ------------------------------------------------------------------------

// A file from before the renumbering (no stamp: version 0) holds Tight 0 .. Widest 4 and Book's style 5. Each comes
// back as the step with the same spacing, and Book's style as Book's style.
TEST(LineSpacingStored, AnUnstampedValueMeansWhatItMeantThen) {
  EXPECT_EQ(lineSpacingStepFromStored(0, 0), step(LineSpacingStep::Pct95));
  EXPECT_EQ(lineSpacingStepFromStored(1, 0), step(LineSpacingStep::Pct100));
  EXPECT_EQ(lineSpacingStepFromStored(2, 0), step(LineSpacingStep::Pct110));
  EXPECT_EQ(lineSpacingStepFromStored(3, 0), step(LineSpacingStep::Pct120));
  EXPECT_EQ(lineSpacingStepFromStored(4, 0), step(LineSpacingStep::Pct130));
  EXPECT_EQ(lineSpacingStepFromStored(5, 0), step(LineSpacingStep::FromBook));
}

TEST(LineSpacingStored, AnUnstampedValueKeepsItsSpacing) {
  const float oldFont[] = {0.95f, 1.0f, 1.1f, 1.2f, 1.3f};
  for (int old = 0; old < 5; ++old) {
    const uint8_t now = lineSpacingStepFromStored(old, 0);
    EXPECT_FLOAT_EQ(lineSpacingFor(now, LineSpacingScale::Font, 14, 30).compression, oldFont[old]) << old;
  }
}

TEST(LineSpacingStored, UnusableValuesAreRefused) {
  EXPECT_EQ(lineSpacingStepFromStored(-1, 0), kNoLineSpacingStep) << "absent";
  EXPECT_EQ(lineSpacingStepFromStored(6, 0), kNoLineSpacingStep) << "past the old list";
  EXPECT_EQ(lineSpacingStepFromStored(-1, LINE_SPACING_ORDER_VERSION), kNoLineSpacingStep) << "absent";
  EXPECT_EQ(lineSpacingStepFromStored(10, LINE_SPACING_ORDER_VERSION), kNoLineSpacingStep) << "past the list";
  EXPECT_EQ(lineSpacingStepFromStored(300, LINE_SPACING_ORDER_VERSION), kNoLineSpacingStep);
}

TEST(LineSpacingStored, AStampedValueIsTodaysStep) {
  for (int s = 0; s <= static_cast<int>(LineSpacingStep::FromBook); ++s) {
    EXPECT_EQ(lineSpacingStepFromStored(s, LINE_SPACING_ORDER_VERSION), s);
  }
}

}  // namespace
