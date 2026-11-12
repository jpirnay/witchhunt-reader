// What a press on each of the reader bars' Layout controls does to the values behind it (ReaderLayout::press).
#include <gtest/gtest.h>

#include "ReaderLayoutControls.h"

using ReaderLayout::Control;

namespace {

// The built-in families' sizes: enough for the controls that do not step through them.
ReaderSizeList sizes() { return ReaderSizeList::builtin(); }

ReaderLayoutValues someValues() {
  ReaderLayoutValues values;
  values.fontSize = 14;
  values.lineSpacing = static_cast<uint8_t>(LineSpacingStep::Pct100);
  values.marginHorizontal = 10;
  values.paragraphAlignment = CrossPointSettings::JUSTIFIED;
  values.textDarkness = CrossPointSettings::DARKNESS_DARK;
  values.embeddedStyle = true;
  return values;
}

}  // namespace

TEST(ReaderLayoutControls, TheSizeStepsThroughTheFontsSizesAndStopsAtItsEnds) {
  const ReaderSizeList list = sizes();
  ASSERT_GE(list.count, 2);
  ReaderLayoutValues values = someValues();
  values.fontSize = list.points[0];
  EXPECT_FALSE(ReaderLayout::press(values, Control::Size, -1, list));
  EXPECT_TRUE(ReaderLayout::press(values, Control::Size, +1, list));
  EXPECT_EQ(values.fontSize, list.points[1]);
  values.fontSize = list.points[list.count - 1];
  EXPECT_FALSE(ReaderLayout::press(values, Control::Size, +1, list));
  EXPECT_TRUE(ReaderLayout::press(values, Control::Size, -1, list));
  EXPECT_EQ(values.fontSize, list.points[list.count - 2]);
}

TEST(ReaderLayoutControls, LineSpacingRunsFromTheTightestStepToTheBooksOwn) {
  ReaderLayoutValues values = someValues();
  values.lineSpacing = 0;
  EXPECT_FALSE(ReaderLayout::press(values, Control::Line, -1, sizes()));
  values.lineSpacing = static_cast<uint8_t>(LineSpacingStep::FromBook);
  EXPECT_FALSE(ReaderLayout::press(values, Control::Line, +1, sizes()));
  EXPECT_TRUE(ReaderLayout::press(values, Control::Line, -1, sizes()));
  EXPECT_EQ(values.lineSpacing, static_cast<uint8_t>(LineSpacingStep::Pct175));
}

TEST(ReaderLayoutControls, TheMarginStepsAsTheSettingDoesWithinItsRange) {
  ReaderLayoutValues values = someValues();
  values.marginHorizontal = CrossPointSettings::SCREEN_MARGIN_MIN;
  EXPECT_FALSE(ReaderLayout::press(values, Control::Margin, -1, sizes()));
  EXPECT_TRUE(ReaderLayout::press(values, Control::Margin, +1, sizes()));
  EXPECT_EQ(values.marginHorizontal, CrossPointSettings::SCREEN_MARGIN_MIN + CrossPointSettings::SCREEN_MARGIN_STEP);
  values.marginHorizontal = CrossPointSettings::SCREEN_MARGIN_MAX;
  EXPECT_FALSE(ReaderLayout::press(values, Control::Margin, +1, sizes()));
}

TEST(ReaderLayoutControls, DarknessGoesFromLightToMaximumThoughLightIsStoredLast) {
  ReaderLayoutValues values = someValues();
  values.textDarkness = CrossPointSettings::DARKNESS_LIGHT;
  EXPECT_FALSE(ReaderLayout::press(values, Control::Darkness, -1, sizes()));
  const uint8_t expected[] = {CrossPointSettings::DARKNESS_NORMAL, CrossPointSettings::DARKNESS_DARK,
                              CrossPointSettings::DARKNESS_EXTRA_DARK, CrossPointSettings::DARKNESS_MAXIMUM};
  for (const uint8_t darkness : expected) {
    EXPECT_TRUE(ReaderLayout::press(values, Control::Darkness, +1, sizes()));
    EXPECT_EQ(values.textDarkness, darkness);
  }
  EXPECT_FALSE(ReaderLayout::press(values, Control::Darkness, +1, sizes()));
}

TEST(ReaderLayoutControls, SwitchesFlipAndAlignmentGoesRound) {
  ReaderLayoutValues values = someValues();
  ReaderLayout::press(values, Control::Paragraph, 0, sizes());
  ReaderLayout::press(values, Control::Hyphenation, 0, sizes());
  ReaderLayout::press(values, Control::BookStyle, 0, sizes());
  EXPECT_TRUE(values.extraParagraphSpacing);
  EXPECT_TRUE(values.hyphenation);
  EXPECT_FALSE(values.embeddedStyle);
  for (uint8_t i = 1; i <= CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT; ++i) {
    ReaderLayout::press(values, Control::Align, 0, sizes());
    EXPECT_EQ(values.paragraphAlignment, i % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
  }
}

TEST(ReaderLayoutControls, DarknessAloneLaysNothingOutAgain) {
  const ReaderLayoutValues before = someValues();
  ReaderLayoutValues after = before;
  after.textDarkness = CrossPointSettings::DARKNESS_MAXIMUM;
  EXPECT_FALSE(ReaderLayout::laysOutDifferently(before, after));
  after.marginHorizontal = 20;
  EXPECT_TRUE(ReaderLayout::laysOutDifferently(before, after));
}
