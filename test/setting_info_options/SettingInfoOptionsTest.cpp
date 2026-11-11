// A settings row whose options are read when asked rather than fixed when the row is built. The
// font-size rows need it: their options are the sizes of the family selected right now, and that
// changes while the screen holding the row is open.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "activities/settings/SettingInfo.h"

// SettingInfo's inline accessors reference SETTINGS and I18N. Same definitions as
// test/timezone/TimezoneTest.cpp: the real settings type, and the generated English blob.
CrossPointSettings CrossPointSettings::instance;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(const StrId id) const { return _active.data + _active.offsets[static_cast<uint16_t>(id)]; }

namespace {

struct Options {
  uint8_t count = 3;
  uint8_t selected = 0;
};

uint8_t optionCount(const void* ctx) { return static_cast<const Options*>(ctx)->count; }
std::string optionLabel(const void* ctx, const uint8_t index) {
  return "opt" + std::to_string(index) + "/" + std::to_string(static_cast<const Options*>(ctx)->count);
}
uint8_t getSelected(const void* ctx) { return static_cast<const Options*>(ctx)->selected; }
void setSelected(void* ctx, const uint8_t v) { static_cast<Options*>(ctx)->selected = v; }

SettingInfo makeRow(Options& options) {
  return SettingInfo::DynamicEnumCtx(StrId::STR_FONT_SIZE, {}, &options, getSelected, setSelected)
      .withDynamicOptions(optionCount, optionLabel);
}

TEST(SettingInfoDynamicOptions, CountAndLabelsAreReadWhenAsked) {
  Options options;
  const SettingInfo row = makeRow(options);
  EXPECT_EQ(3, int{row.getEnumOptionCount()});
  options.count = 5;  // the family changed under an open screen
  EXPECT_EQ(5, int{row.getEnumOptionCount()});
  EXPECT_EQ("opt4/5", row.getEnumOptionLabel(4));
}

TEST(SettingInfoDynamicOptions, ALabelPastTheCountIsEmpty) {
  Options options;
  EXPECT_EQ("", makeRow(options).getEnumOptionLabel(3));
}

// The web settings API takes the borrowed pointer when there is one and copies otherwise; a
// dynamic label exists only as a std::string, so there must be no pointer.
TEST(SettingInfoDynamicOptions, HasNoFlashLabel) {
  Options options;
  EXPECT_EQ(nullptr, makeRow(options).getEnumOptionFlashLabel(0));
}

TEST(SettingInfoDynamicOptions, WinsOverLabelsFilledInAtBuildTime) {
  Options options;
  SettingInfo row = makeRow(options);
  row.enumLabels = {"stale"};
  EXPECT_EQ(3, int{row.getEnumOptionCount()});
  EXPECT_EQ("opt0/3", row.getEnumOptionLabel(0));
}

TEST(SettingInfoDynamicOptions, SelectionIsBoundedByTheCountNow) {
  Options options;
  const SettingInfo row = makeRow(options);
  row.setEnumSelectedIndex(2);
  EXPECT_EQ(2, int{options.selected});
  row.setEnumSelectedIndex(3);
  EXPECT_EQ(2, int{options.selected}) << "past the count: ignored";
  options.count = 5;
  row.setEnumSelectedIndex(4);
  EXPECT_EQ(4, int{options.selected});
  EXPECT_EQ(4, int{row.getEnumSelectedIndex()});
}

TEST(SettingInfoDynamicOptions, ARowWithoutThemIsUnchanged) {
  Options options;
  const SettingInfo row = SettingInfo::DynamicEnumCtx(StrId::STR_FONT_SIZE, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
                                                      &options, getSelected, setSelected);
  EXPECT_EQ(2, int{row.getEnumOptionCount()});
  EXPECT_NE(nullptr, row.getEnumOptionFlashLabel(1));
}

// Literal options: flash labels beside the translated ones, after them by default or from an index on.
const char* literalLabel(const uint8_t index) {
  static const char* const kLabels[] = {"L0", "L1", "L2"};
  return kLabels[index];
}

SettingInfo literalRow(std::vector<StrId> values) {
  return SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing, std::move(values));
}

TEST(SettingInfoLiteralOptions, FollowTheTranslatedOnesByDefault) {
  const SettingInfo row = literalRow({StrId::STR_STATE_OFF, StrId::STR_STATE_ON}).withLiteralOptions(literalLabel, 3);
  ASSERT_EQ(5, int{row.getEnumOptionCount()});
  EXPECT_EQ(I18N.get(StrId::STR_STATE_OFF), row.getEnumOptionLabel(0));
  EXPECT_EQ(I18N.get(StrId::STR_STATE_ON), row.getEnumOptionLabel(1));
  EXPECT_EQ("L0", row.getEnumOptionLabel(2));
  EXPECT_EQ("L2", row.getEnumOptionLabel(4));
  EXPECT_EQ("", row.getEnumOptionLabel(5));
}

TEST(SettingInfoLiteralOptions, SitBetweenTheTranslatedOnesFromTheirIndex) {
  const SettingInfo row =
      literalRow({StrId::STR_DEFAULT_VALUE, StrId::STR_BOOK_S_STYLE}).withLiteralOptions(literalLabel, 3, 1);
  ASSERT_EQ(5, int{row.getEnumOptionCount()});
  EXPECT_EQ(I18N.get(StrId::STR_DEFAULT_VALUE), row.getEnumOptionLabel(0));
  EXPECT_EQ("L0", row.getEnumOptionLabel(1));
  EXPECT_EQ("L1", row.getEnumOptionLabel(2));
  EXPECT_EQ("L2", row.getEnumOptionLabel(3));
  EXPECT_EQ(I18N.get(StrId::STR_BOOK_S_STYLE), row.getEnumOptionLabel(4));
  EXPECT_EQ(nullptr, row.getEnumOptionFlashLabel(5));
}

TEST(SettingInfoLiteralOptions, CanComeFirst) {
  const SettingInfo row = literalRow({StrId::STR_BOOK_S_STYLE}).withLiteralOptions(literalLabel, 3, 0);
  ASSERT_EQ(4, int{row.getEnumOptionCount()});
  EXPECT_EQ("L0", row.getEnumOptionLabel(0));
  EXPECT_EQ("L2", row.getEnumOptionLabel(2));
  EXPECT_EQ(I18N.get(StrId::STR_BOOK_S_STYLE), row.getEnumOptionLabel(3));
}

TEST(SettingInfoLiteralOptions, AnIndexPastTheTranslatedOnesAppends) {
  const SettingInfo row = literalRow({StrId::STR_STATE_OFF}).withLiteralOptions(literalLabel, 3, 7);
  ASSERT_EQ(4, int{row.getEnumOptionCount()});
  EXPECT_EQ(I18N.get(StrId::STR_STATE_OFF), row.getEnumOptionLabel(0));
  EXPECT_EQ("L0", row.getEnumOptionLabel(1));
  EXPECT_EQ("L2", row.getEnumOptionLabel(3));
}

}  // namespace
