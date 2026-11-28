// What a button or gesture action row offers on a board, and how the row stores an action rather than the position of
// its option (ButtonActionOptions.h, SettingInfo::withOptionValues).
#include <gtest/gtest.h>

#include <set>

#include "ButtonActionOptions.h"
#include "activities/settings/SettingInfo.h"

// SettingInfo's inline accessors reference SETTINGS and I18N: the real settings type, and the generated English blob
// (as test/setting_info_options does).
CrossPointSettings CrossPointSettings::instance;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(const StrId id) const { return _active.data + _active.offsets[static_cast<uint16_t>(id)]; }

namespace {

using S = CrossPointSettings;
namespace BAO = ButtonActionOptions;

constexpr BAO::Caps kUnlit{false, false, false};
constexpr BAO::Caps kLit{true, false, false};
constexpr BAO::Caps kWarm{true, true, false};
constexpr BAO::Caps kAll{true, true, true};
constexpr BAO::Caps kUnlitBle{false, false, true};

TEST(ButtonActionOptions, OptionZeroIsTheRowsDefault) {
  for (const BAO::Caps caps : {kUnlit, kLit, kWarm, kAll, kUnlitBle}) {
    EXPECT_EQ(S::BTN_DEFAULT, BAO::actionAt(0, caps));
    EXPECT_EQ(0, BAO::optionOf(S::BTN_DEFAULT, caps));
  }
}

TEST(ButtonActionOptions, EveryOptionStoresItsActionAndReadsBackAsItself) {
  for (const BAO::Caps caps : {kUnlit, kLit, kWarm, kAll, kUnlitBle}) {
    std::set<uint8_t> seen;
    const uint8_t count = BAO::optionCount(caps);
    for (uint8_t option = 0; option < count; ++option) {
      const uint8_t action = BAO::actionAt(option, caps);
      EXPECT_TRUE(seen.insert(action).second) << int(option);
      EXPECT_EQ(option, BAO::optionOf(action, caps)) << int(action);
      EXPECT_TRUE(BAO::offered(action, caps)) << int(action);
    }
  }
}

TEST(ButtonActionOptions, ABoardOffersOnlyWhatItCanDo) {
  EXPECT_EQ(S::BUTTON_ACTION_COUNT, BAO::optionCount(kAll));
  EXPECT_EQ(S::BUTTON_ACTION_COUNT - 1, BAO::optionCount(kWarm));
  EXPECT_EQ(S::BUTTON_ACTION_COUNT - 3, BAO::optionCount(kLit));
  EXPECT_EQ(S::BUTTON_ACTION_COUNT - 7, BAO::optionCount(kUnlit));
  EXPECT_EQ(S::BUTTON_ACTION_COUNT - 6, BAO::optionCount(kUnlitBle));
  // The case the positional lists could not take: an action at the end of the enum on a board without the light
  // block before it.
  EXPECT_EQ(-1, BAO::optionOf(S::BTN_TOGGLE_BLE_REMOTE, kWarm));
  EXPECT_GT(BAO::optionOf(S::BTN_TOGGLE_BLE_REMOTE, kUnlitBle), 0);
  EXPECT_EQ(S::BTN_TOGGLE_BLE_REMOTE,
            BAO::actionAt(static_cast<uint8_t>(BAO::optionOf(S::BTN_TOGGLE_BLE_REMOTE, kUnlitBle)), kUnlitBle));
  for (const uint8_t light : {S::BTN_LIGHT_TOGGLE, S::BTN_LIGHT_BRIGHTER, S::BTN_LIGHT_DIMMER, S::BTN_LIGHT_PANEL}) {
    EXPECT_EQ(-1, BAO::optionOf(light, kUnlit)) << int(light);
    EXPECT_GT(BAO::optionOf(light, kLit), 0) << int(light);
  }
  for (const uint8_t warm : {S::BTN_LIGHT_WARMER, S::BTN_LIGHT_COOLER}) {
    EXPECT_EQ(-1, BAO::optionOf(warm, kLit)) << int(warm);
    EXPECT_GT(BAO::optionOf(warm, kWarm), 0) << int(warm);
  }
}

TEST(ButtonActionOptions, AnOptionPastTheLastStoresTheDefault) {
  EXPECT_EQ(S::BTN_DEFAULT, BAO::actionAt(BAO::optionCount(kUnlit), kUnlit));
  EXPECT_EQ(S::BTN_DEFAULT, BAO::actionAt(255, kWarm));
}

// An unlit board: the options after Touch Lock are gone, so positions and values part ways only past the light block,
// which is where a row that stored positions would have shown the wrong action.
uint8_t unlitValueOf(const uint8_t option) { return BAO::actionAt(option, kUnlit); }
int unlitOptionOf(const uint8_t value) { return BAO::optionOf(value, kUnlit); }
uint8_t warmValueOf(const uint8_t option) { return BAO::actionAt(option, kWarm); }
int warmOptionOf(const uint8_t value) { return BAO::optionOf(value, kWarm); }

SettingInfo actionRow(const BAO::Caps caps, const SettingInfo::OptionValueFn valueOf,
                      const SettingInfo::ValueOptionFn optionOf) {
  std::vector<StrId> labels{StrId::STR_BTN_DEF_PREV_PAGE};
  for (const BAO::Option& option : BAO::ALL) {
    if (BAO::offered(option.needs, caps)) labels.push_back(option.label);
  }
  return SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortLeft, std::move(labels),
                           "btnShortLeft", StrId::STR_CAT_CONTROLS)
      .withOptionValues(valueOf, optionOf);
}

TEST(SettingInfoOptionValues, ThePickerShowsTheOptionThatStoresTheValue) {
  const SettingInfo row = actionRow(kWarm, warmValueOf, warmOptionOf);
  SETTINGS.btnShortLeft = S::BTN_LIGHT_COOLER;
  const int option = BAO::optionOf(S::BTN_LIGHT_COOLER, kWarm);
  EXPECT_EQ(option, row.getEnumSelectedIndex());
  EXPECT_EQ(row.getEnumOptionLabel(static_cast<uint8_t>(option)), I18N.get(StrId::STR_BTN_ACT_LIGHT_COOLER));
}

TEST(SettingInfoOptionValues, ChoosingAnOptionStoresItsAction) {
  const SettingInfo row = actionRow(kWarm, warmValueOf, warmOptionOf);
  row.setEnumSelectedIndex(static_cast<uint8_t>(BAO::optionOf(S::BTN_SCREENSHOT, kWarm)));
  EXPECT_EQ(S::BTN_SCREENSHOT, SETTINGS.btnShortLeft);
  row.setEnumSelectedIndex(0);
  EXPECT_EQ(S::BTN_DEFAULT, SETTINGS.btnShortLeft);
}

TEST(SettingInfoOptionValues, AValueTheBoardDoesNotOfferReadsAsTheDefault) {
  const SettingInfo row = actionRow(kUnlit, unlitValueOf, unlitOptionOf);
  SETTINGS.btnShortLeft = S::BTN_LIGHT_TOGGLE;
  EXPECT_EQ(-1, row.enumOptionOf(S::BTN_LIGHT_TOGGLE));
  EXPECT_EQ(0, row.getEnumSelectedIndex());
}

TEST(SettingInfoOptionValues, OptionsAndLabelsLineUpOnEveryBoard) {
  for (const BAO::Caps caps : {kUnlit, kLit, kWarm, kAll, kUnlitBle}) {
    std::vector<StrId> labels{StrId::STR_BTN_DEF_PREV_PAGE};
    for (const BAO::Option& option : BAO::ALL) {
      if (BAO::offered(option.needs, caps)) labels.push_back(option.label);
    }
    ASSERT_EQ(labels.size(), BAO::optionCount(caps));
    for (uint8_t option = 1; option < labels.size(); ++option) {
      const uint8_t action = BAO::actionAt(option, caps);
      bool found = false;
      for (const BAO::Option& entry : BAO::ALL) found |= entry.action == action && entry.label == labels[option];
      EXPECT_TRUE(found) << int(option);
    }
  }
}

TEST(SettingInfoOptionValues, ARowWithoutThemStoresPositions) {
  const SettingInfo row = SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                                            {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode");
  row.setEnumSelectedIndex(1);
  EXPECT_EQ(1, SETTINGS.sleepScreenCoverMode);
  EXPECT_EQ(1, row.getEnumSelectedIndex());
  EXPECT_EQ(1, row.enumOptionOf(1));
  EXPECT_EQ(-1, row.enumOptionOf(2));
}

}  // namespace
