#include <gtest/gtest.h>

#include "BookReaderOverrides.h"
#include "CrossPointSettings.h"
#include "Epub/LineSpacing.h"

using Field = BookReaderOverrides::Field;

namespace {
const BookReaderOverrides::Numbering kCurrent = BookReaderOverrides::current();
}

TEST(BookReaderOverrides, ANewBookFollowsEverySetting) {
  const BookReaderOverrides overrides;
  EXPECT_FALSE(overrides.any());
  for (uint8_t i = 0; i < BookReaderOverrides::kFieldCount; ++i) {
    EXPECT_EQ(overrides.get(static_cast<Field>(i)), -1) << "field " << int{i};
  }
}

TEST(BookReaderOverrides, EveryFieldReadsBackWhatWasStored) {
  for (uint8_t i = 0; i < BookReaderOverrides::kFieldCount; ++i) {
    const auto field = static_cast<Field>(i);
    // 1 is a valid value of every field but the size, a point size, and the margin, in pixels.
    int value = 1;
    if (field == Field::FontSize) value = CrossPointSettings::DEFAULT_FONT_POINT_SIZE;
    if (field == Field::MarginHorizontal) value = CrossPointSettings::SCREEN_MARGIN_MIN + 5;
    BookReaderOverrides overrides;
    overrides.setFromStored(field, value, kCurrent);
    EXPECT_EQ(overrides.get(field), value) << "field " << int{i};
    EXPECT_TRUE(overrides.any());
  }
}

TEST(BookReaderOverrides, AMarginOutsideTheSettingsRangeFollowsTheSetting) {
  BookReaderOverrides overrides;
  overrides.setFromStored(Field::MarginHorizontal, CrossPointSettings::SCREEN_MARGIN_MIN - 1, kCurrent);
  EXPECT_EQ(overrides.get(Field::MarginHorizontal), -1);
  overrides.setFromStored(Field::MarginHorizontal, CrossPointSettings::SCREEN_MARGIN_MAX + 1, kCurrent);
  EXPECT_EQ(overrides.get(Field::MarginHorizontal), -1);
  overrides.setFromStored(Field::MarginHorizontal, CrossPointSettings::SCREEN_MARGIN_MAX, kCurrent);
  EXPECT_EQ(overrides.get(Field::MarginHorizontal), CrossPointSettings::SCREEN_MARGIN_MAX);
}

TEST(BookReaderOverrides, ValuesAFieldCannotHaveFollowTheSetting) {
  BookReaderOverrides overrides;
  overrides.setFromStored(Field::Hyphenation, 2, kCurrent);
  overrides.setFromStored(Field::ImageRendering, CrossPointSettings::IMAGE_RENDERING_COUNT, kCurrent);
  overrides.setFromStored(Field::PublisherFonts, -7, kCurrent);
  overrides.setFromStored(Field::FontSize, 0, kCurrent);
  overrides.setFromStored(Field::LineSpacing, 200, kCurrent);
  EXPECT_FALSE(overrides.any());
}

TEST(BookReaderOverrides, AFontSizeIsReadInTheNumberingItWasStoredIn) {
  BookReaderOverrides overrides;
  overrides.setFromStored(Field::FontSize, CrossPointSettings::DEFAULT_FONT_POINT_SIZE, kCurrent);
  EXPECT_EQ(overrides.get(Field::FontSize), CrossPointSettings::DEFAULT_FONT_POINT_SIZE);

  // A file from before point sizes stored a FONT_SIZE rung; it is converted as recent.json converts it.
  const BookReaderOverrides::Numbering legacy{0, kCurrent.lineSpacingOrder};
  overrides.setFromStored(Field::FontSize, 1, legacy);
  EXPECT_EQ(overrides.get(Field::FontSize), static_cast<int8_t>(CrossPointSettings::fontPointSizeFromStored(1, 0)));
  EXPECT_GT(overrides.get(Field::FontSize), 0);
}

TEST(BookReaderOverrides, ALineSpacingStepIsReadInTheNumberingItWasStoredIn) {
  BookReaderOverrides overrides;
  // Before LINE_SPACING_ORDER_VERSION 1, 1 was Normal.
  overrides.setFromStored(Field::LineSpacing, 1, BookReaderOverrides::Numbering{kCurrent.fontSizeOrder, 0});
  EXPECT_EQ(overrides.get(Field::LineSpacing), static_cast<int8_t>(LineSpacingStep::Pct100));
  overrides.setFromStored(Field::LineSpacing, 1, kCurrent);
  EXPECT_EQ(overrides.get(Field::LineSpacing), static_cast<int8_t>(LineSpacingStep::Pct95));
}

TEST(BookReaderOverrides, AFamilyByNameWinsOverABuiltInOneWhicheverComesFirst) {
  BookReaderOverrides nameFirst;
  nameFirst.setSdFontFamilyFromStored("Literata");
  nameFirst.setFromStored(Field::FontFamily, 1, kCurrent);
  EXPECT_EQ(nameFirst.get(Field::FontFamily), -1);
  EXPECT_EQ(nameFirst.sdFontFamilyOverride, "Literata");

  BookReaderOverrides builtInFirst;
  builtInFirst.setFromStored(Field::FontFamily, 1, kCurrent);
  builtInFirst.setSdFontFamilyFromStored("Literata");
  EXPECT_EQ(builtInFirst, nameFirst);
}

TEST(BookReaderOverrides, AFamilyByNameAloneIsASettingOfTheBooksOwn) {
  BookReaderOverrides overrides;
  overrides.setSdFontFamilyFromStored("Literata");
  EXPECT_TRUE(overrides.any());
  overrides.setSdFontFamilyFromStored("");
  EXPECT_FALSE(overrides.any());
}
