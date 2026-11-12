// Dark Mode's decisions (DarkMode.h, ReaderImageMode::forOutput) and the settings row that persists it
// (DarkModeOption.h).
#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "DarkMode.h"
#include "ReaderImageMode.h"
#include "activities/settings/DarkModeOption.h"

// SettingInfo's inline accessors reference SETTINGS and I18N. Same definitions as
// test/setting_info_options: the real settings type, and the generated English blob.
CrossPointSettings CrossPointSettings::instance;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(const StrId id) const { return _active.data + _active.offsets[static_cast<uint16_t>(id)]; }

namespace {

using S = CrossPointSettings;

// Every screen inverts with the setting on, except one that shows a picture of its own; Quick Resume leaves the frame
// on screen in the polarity it went out in, whatever the setting says now.
TEST(DarkMode, OutputPolarityPerScreen) {
  using P = DarkMode::ScreenPolarity;
  for (const bool now : {false, true}) {
    EXPECT_FALSE(DarkMode::invertsOutput(false, P::FollowDarkMode, now));
    EXPECT_TRUE(DarkMode::invertsOutput(true, P::FollowDarkMode, now));
    EXPECT_FALSE(DarkMode::invertsOutput(true, P::Normal, now)) << "a sleep screen or the image viewer";
    EXPECT_FALSE(DarkMode::invertsOutput(false, P::Normal, now));
    EXPECT_EQ(DarkMode::invertsOutput(true, P::AsOnScreen, now), now) << "Quick Resume over the reader or a picture";
    EXPECT_EQ(DarkMode::invertsOutput(false, P::AsOnScreen, now), now);
  }
}

// Quick Resume's moon: a FAST on a dark page (a HALF would flash it white), the HALF it always had on a light one.
TEST(DarkMode, QuickResumeMoonIsFastOnlyOnADarkPage) {
  EXPECT_TRUE(DarkMode::moonGoesOutFast(true));
  EXPECT_FALSE(DarkMode::moonGoesOutFast(false));
}

// Quick Resume's frame, saved at sleep (main.cpp saveSleepFrameBuffer) and restored at wake (loadSleepFrameBuffer) in
// whatever polarity the wake has, must reach the glass as it was when the device went to sleep: from the reader or Home
// in either mode, from the image viewer (normal polarity under Dark Mode), and across a start or end of the night hours
// while asleep.
TEST(DarkMode, QuickResumeFrameComesBackAsItWasOnTheGlass) {
  const uint8_t frame[4] = {0x00, 0xFF, 0x0F, 0xA5};
  const auto glass = [](const uint8_t byte, const bool inverted) { return inverted ? uint8_t(~byte) : byte; };
  const auto convert = [](const uint8_t byte, const bool inverted) {
    return DarkMode::glassDiffersFromBuffer(inverted) ? uint8_t(~byte) : byte;
  };
  for (const bool shownInverted : {false, true}) {
    for (const bool invertedAtWake : {false, true}) {
      for (const uint8_t byte : frame) {
        const uint8_t saved = convert(byte, shownInverted);
        const uint8_t restored = convert(saved, invertedAtWake);
        EXPECT_EQ(glass(restored, invertedAtWake), glass(byte, shownInverted))
            << "shownInverted=" << shownInverted << " invertedAtWake=" << invertedAtWake;
      }
    }
  }
}

// The plane pair does not survive inversion: anti-aliasing is off on every panel that cannot mirror its single push's
// greys (the X3, X4, X4 Pro, Murphy), and stays on the LilyGo T5S3, which can.
TEST(DarkMode, TextAntiAliasingIsOffWhereTheGraysDoNotSurviveInversion) {
  EXPECT_TRUE(DarkMode::showsTextAntiAliasing(true, false, false)) << "light mode is unchanged";
  EXPECT_FALSE(DarkMode::showsTextAntiAliasing(true, true, false)) << "dual-plane panel, dark";
  EXPECT_TRUE(DarkMode::showsTextAntiAliasing(true, true, true)) << "T5S3 single push, dark";
  EXPECT_FALSE(DarkMode::showsTextAntiAliasing(false, true, true)) << "never on when the reader turned it off";
  EXPECT_FALSE(DarkMode::showsTextAntiAliasing(false, false, true));
}

// Ghost-clearing runs twice as often in Dark Mode; Never stays Never and every page stays every page. Every refresh
// cadence reads it through CrossPointSettings::getRefreshFrequency(), which passes darkMode in.
TEST(DarkMode, RefreshFrequencyHalvesInDarkMode) {
  EXPECT_EQ(DarkMode::refreshFrequency(15, false), 15);
  EXPECT_EQ(DarkMode::refreshFrequency(15, true), 8);
  EXPECT_EQ(DarkMode::refreshFrequency(5, true), 3);
  EXPECT_EQ(DarkMode::refreshFrequency(2, true), 1);
  EXPECT_EQ(DarkMode::refreshFrequency(1, true), 1);
  EXPECT_EQ(DarkMode::refreshFrequency(0, true), 0) << "Never";
  EXPECT_EQ(DarkMode::refreshFrequency(60, true), 30);
}

// Display's pictures go out as Black & White does while the output is inverted; the other modes are untouched, and
// "Refresh after image pages" still answers for the setting the reader chose (EpubReaderActivity reads it from there).
TEST(DarkMode, DisplayImagesAreDrawnBlackAndWhite) {
  EXPECT_EQ(ReaderImageMode::forOutput(S::IMAGES_DISPLAY, true), S::IMAGES_DISPLAY_BW);
  EXPECT_EQ(ReaderImageMode::forOutput(S::IMAGES_DISPLAY, false), S::IMAGES_DISPLAY);
  for (const uint8_t mode : {S::IMAGES_DISPLAY_BW, S::IMAGES_PLACEHOLDER, S::IMAGES_SUPPRESS}) {
    EXPECT_EQ(ReaderImageMode::forOutput(mode, true), mode);
    EXPECT_EQ(ReaderImageMode::forOutput(mode, false), mode);
  }
  EXPECT_FALSE(ReaderImageMode::imagesInGray(ReaderImageMode::forOutput(S::IMAGES_DISPLAY, true)));
  EXPECT_EQ(ReaderImageMode::forLayout(ReaderImageMode::forOutput(S::IMAGES_DISPLAY, true)), S::IMAGES_DISPLAY)
      << "same layout, same section cache";
  EXPECT_TRUE(ReaderImageMode::imagePageOwesRefresh(S::IMAGES_DISPLAY));
}

// What persists the setting: JsonSettingsIO saves and loads every keyed row of the settings list by its key, bounded
// by its type (a toggle loads 0 or 1, anything else as the default). The row must carry the key, the toggle type and
// the field; off is the default, so no existing install changes.
TEST(DarkMode, TheSettingsRowPersistsTheField) {
  const SettingInfo row = DarkModeOption::make(/*scheduleOffered=*/false);
  ASSERT_NE(row.key, nullptr);
  EXPECT_STREQ(row.key, "darkMode");
  EXPECT_EQ(row.type, SettingType::TOGGLE) << "no reading light or clock: the switch it was";
  EXPECT_EQ(row.valuePtr, &CrossPointSettings::darkMode);
  EXPECT_EQ(row.category, StrId::STR_CAT_DISPLAY);
  EXPECT_EQ(row.nameId, StrId::STR_DARK_MODE);
  EXPECT_EQ(S::getInstance().darkMode, 0) << "off by default";
}

// With a reading light and a clock the row offers Scheduled as its third value, keeping 0 and 1 as the toggle stored
// them, so an install upgrading keeps its choice. JsonSettingsIO bounds an enum by its option count.
TEST(DarkMode, TheSettingsRowOffersScheduledWhereTheBoardCanKeepIt) {
  const SettingInfo row = DarkModeOption::make(/*scheduleOffered=*/true);
  EXPECT_STREQ(row.key, "darkMode");
  EXPECT_EQ(row.type, SettingType::ENUM);
  EXPECT_EQ(row.valuePtr, &CrossPointSettings::darkMode);
  ASSERT_EQ(row.getEnumOptionCount(), DarkMode::SETTING_COUNT);
  EXPECT_EQ(row.enumValues[DarkMode::SETTING_OFF], StrId::STR_DARK_MODE_OFF);
  EXPECT_EQ(row.enumValues[DarkMode::SETTING_ON], StrId::STR_DARK_MODE_ON);
  EXPECT_EQ(row.enumValues[DarkMode::SETTING_SCHEDULED], StrId::STR_DARK_MODE_SCHEDULED);
  EXPECT_EQ(row.requiredCapability, SettingRequires::Nothing) << "Off and On everywhere";
}

// The hours: two rows of half-hour times, saved by key, shown only under Scheduled and only where the board has the
// reading light and the clock; labelled in the clock's format.
TEST(DarkMode, TheHoursRowsShowUnderScheduledOnly) {
  auto& settings = S::getInstance();
  const SettingInfo from = DarkModeOption::fromRow();
  const SettingInfo until = DarkModeOption::untilRow();
  EXPECT_STREQ(from.key, "darkModeFrom");
  EXPECT_STREQ(until.key, "darkModeUntil");
  EXPECT_EQ(from.valuePtr, &CrossPointSettings::darkModeFrom);
  EXPECT_EQ(until.valuePtr, &CrossPointSettings::darkModeUntil);
  for (const SettingInfo* row : {&from, &until}) {
    EXPECT_EQ(row->type, SettingType::ENUM);
    EXPECT_EQ(row->getEnumOptionCount(), 48);
    EXPECT_TRUE(row->usesSelectorActivity);
    EXPECT_EQ(row->requiredCapability, SettingRequires::ReadingLightAndClock);
    ASSERT_NE(row->shownWhen, nullptr);
    EXPECT_EQ(row->category, StrId::STR_CAT_DISPLAY);
  }
  EXPECT_EQ(settings.darkModeFrom, 44) << "22:00";
  EXPECT_EQ(settings.darkModeUntil, 14) << "07:00";

  const uint8_t saved = settings.darkMode;
  for (const uint8_t mode : {DarkMode::SETTING_OFF, DarkMode::SETTING_ON, DarkMode::SETTING_SCHEDULED}) {
    settings.darkMode = mode;
    EXPECT_EQ(from.shownWhen(), mode == DarkMode::SETTING_SCHEDULED) << int(mode);
  }
  settings.darkMode = saved;

  const uint8_t savedFormat = settings.clockFormat12h;
  settings.clockFormat12h = 0;
  EXPECT_EQ(from.getEnumOptionLabel(44), "22:00");
  EXPECT_EQ(from.getEnumOptionLabel(1), "00:30");
  settings.clockFormat12h = 1;
  EXPECT_EQ(from.getEnumOptionLabel(44), "10:00PM");
  EXPECT_EQ(from.getEnumOptionLabel(0), "12:00AM");
  EXPECT_EQ(from.getEnumOptionLabel(24), "12:00PM");
  settings.clockFormat12h = savedFormat;
}

DarkMode::ClockReading at(const int hour, const int minute, const int second = 0, const uint32_t epochDay = 1000000) {
  const int secondOfDay = hour * 3600 + minute * 60 + second;
  return {true, epochDay + static_cast<uint32_t>(secondOfDay), secondOfDay};
}
constexpr DarkMode::NightFlip NO_FLIP{0, false};
constexpr uint8_t T22_00 = 44;
constexpr uint8_t T07_00 = 14;

// The night hours run [from, until), across midnight when from is the later time; the same slot twice is none.
TEST(DarkMode, NightHoursMayRunPastMidnight) {
  EXPECT_TRUE(DarkMode::inNightHours(22 * 60, T22_00, T07_00));
  EXPECT_TRUE(DarkMode::inNightHours(23 * 60 + 59, T22_00, T07_00));
  EXPECT_TRUE(DarkMode::inNightHours(0, T22_00, T07_00));
  EXPECT_TRUE(DarkMode::inNightHours(6 * 60 + 59, T22_00, T07_00));
  EXPECT_FALSE(DarkMode::inNightHours(7 * 60, T22_00, T07_00)) << "until is the first light minute";
  EXPECT_FALSE(DarkMode::inNightHours(21 * 60 + 59, T22_00, T07_00));
  EXPECT_FALSE(DarkMode::inNightHours(12 * 60, T22_00, T07_00));
  // Within one day: 13:00 to 15:30.
  EXPECT_TRUE(DarkMode::inNightHours(13 * 60, 26, 31));
  EXPECT_TRUE(DarkMode::inNightHours(15 * 60 + 29, 26, 31));
  EXPECT_FALSE(DarkMode::inNightHours(15 * 60 + 30, 26, 31));
  EXPECT_FALSE(DarkMode::inNightHours(12 * 60 + 59, 26, 31));
  for (const int minute : {0, 600, 1439}) EXPECT_FALSE(DarkMode::inNightHours(minute, 20, 20));
}

TEST(DarkMode, MinutesToTheNextChangeOfTheNightHours) {
  EXPECT_EQ(DarkMode::minutesToNextChange(21 * 60, T22_00, T07_00), 60);
  EXPECT_EQ(DarkMode::minutesToNextChange(23 * 60, T22_00, T07_00), 8 * 60);
  EXPECT_EQ(DarkMode::minutesToNextChange(6 * 60 + 59, T22_00, T07_00), 1);
  EXPECT_EQ(DarkMode::minutesToNextChange(7 * 60, T22_00, T07_00), 15 * 60) << "at a boundary: the next one";
  EXPECT_EQ(DarkMode::minutesToNextChange(12 * 60, 20, 20), 22 * 60) << "no night hours: a day's worth at most";
}

// Off and On ignore the clock; Scheduled follows it, and stays light while the time of day is not known.
TEST(DarkMode, ScheduledFollowsTheClock) {
  using DarkMode::isDark;
  const DarkMode::ClockReading unknown{false, 0, 0};
  for (const auto& clock : {at(23, 0), at(12, 0), unknown}) {
    EXPECT_FALSE(isDark(DarkMode::SETTING_OFF, T22_00, T07_00, clock, NO_FLIP));
    EXPECT_TRUE(isDark(DarkMode::SETTING_ON, T22_00, T07_00, clock, NO_FLIP));
  }
  EXPECT_TRUE(isDark(DarkMode::SETTING_SCHEDULED, T22_00, T07_00, at(23, 0), NO_FLIP));
  EXPECT_TRUE(isDark(DarkMode::SETTING_SCHEDULED, T22_00, T07_00, at(3, 0), NO_FLIP));
  EXPECT_FALSE(isDark(DarkMode::SETTING_SCHEDULED, T22_00, T07_00, at(12, 0), NO_FLIP));
  EXPECT_FALSE(isDark(DarkMode::SETTING_SCHEDULED, T22_00, T07_00, unknown, NO_FLIP)) << "no clock: light";
  EXPECT_FALSE(isDark(7, T22_00, T07_00, at(23, 0), NO_FLIP)) << "a value this build does not know";
}

// The drawer's switch: Off and On swap; under Scheduled the other polarity holds until the hours next change, then the
// hours take over again; flipped back before then, the flip goes.
TEST(DarkMode, TheDrawerFlipHoldsUntilTheHoursNextChange) {
  using DarkMode::flipped;
  using DarkMode::isDark;
  EXPECT_EQ(flipped(DarkMode::SETTING_OFF, T22_00, T07_00, at(12, 0), NO_FLIP).setting, DarkMode::SETTING_ON);
  EXPECT_EQ(flipped(DarkMode::SETTING_ON, T22_00, T07_00, at(12, 0), NO_FLIP).setting, DarkMode::SETTING_OFF);
  EXPECT_EQ(flipped(DarkMode::SETTING_ON, T22_00, T07_00, at(12, 0), NO_FLIP).flip.until, 0u);

  // 23:00:20 in the night hours: light until 07:00:00.
  const auto night = at(23, 0, 20);
  const auto lit = flipped(DarkMode::SETTING_SCHEDULED, T22_00, T07_00, night, NO_FLIP);
  EXPECT_EQ(lit.setting, DarkMode::SETTING_SCHEDULED) << "the schedule stays";
  EXPECT_FALSE(lit.flip.dark);
  EXPECT_EQ(lit.flip.until, night.epoch + 8 * 3600 - 20);
  EXPECT_FALSE(isDark(lit.setting, T22_00, T07_00, at(23, 30), lit.flip));
  EXPECT_FALSE(isDark(lit.setting, T22_00, T07_00, at(6, 59, 59, 1000000 + 86400), lit.flip)) << "next morning";
  EXPECT_FALSE(isDark(lit.setting, T22_00, T07_00, at(7, 0, 0, 1000000 + 86400), lit.flip)) << "day by the hours";
  EXPECT_TRUE(isDark(lit.setting, T22_00, T07_00, at(22, 0, 0, 1000000 + 86400), lit.flip))
      << "the next night is dark again";
  EXPECT_TRUE(isDark(lit.setting, T22_00, T07_00, at(23, 0, 0, 1000000 + 2 * 86400), lit.flip))
      << "asleep through a whole day: the flip ran out at 07:00";

  // Flipped back the same night: the hours again, and no flip left.
  const auto back = flipped(lit.setting, T22_00, T07_00, at(23, 10), lit.flip);
  EXPECT_EQ(back.flip.until, 0u);
  EXPECT_TRUE(isDark(back.setting, T22_00, T07_00, at(23, 10), back.flip));

  // In the day, dark until 22:00.
  const auto noon = at(12, 0);
  const auto dark = flipped(DarkMode::SETTING_SCHEDULED, T22_00, T07_00, noon, NO_FLIP);
  EXPECT_TRUE(dark.flip.dark);
  EXPECT_EQ(dark.flip.until, noon.epoch + 10 * 3600);
  EXPECT_TRUE(isDark(dark.setting, T22_00, T07_00, at(21, 59), dark.flip));
  EXPECT_TRUE(isDark(dark.setting, T22_00, T07_00, at(23, 0), dark.flip)) << "the night hours from there";
  EXPECT_FALSE(isDark(dark.setting, T22_00, T07_00, at(7, 0, 0, 1000000 + 86400), dark.flip));

  // No clock: no "until" to hold to, so the switch sets the setting like Off and On.
  const auto noClock = flipped(DarkMode::SETTING_SCHEDULED, T22_00, T07_00, {false, 0, 0}, NO_FLIP);
  EXPECT_EQ(noClock.setting, DarkMode::SETTING_ON) << "light without a clock, so the flip turns it on";
}

}  // namespace
