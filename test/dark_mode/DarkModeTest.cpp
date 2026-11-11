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

// Quick Resume's frame, saved at sleep and restored at wake in the polarity boot sets (Dark Mode's), must reach the
// glass as it was when the device went to sleep: from the reader or Home in either mode, and from the image viewer,
// which shows in normal polarity while Dark Mode is on.
TEST(DarkMode, QuickResumeFrameComesBackAsItWasOnTheGlass) {
  const uint8_t frame[4] = {0x00, 0xFF, 0x0F, 0xA5};
  const auto glass = [](const uint8_t byte, const bool inverted) { return inverted ? uint8_t(~byte) : byte; };
  for (const bool darkMode : {false, true}) {
    for (const bool shownInverted : {false, true}) {
      const bool invert = DarkMode::savedFrameInverts(shownInverted, darkMode);
      for (const uint8_t byte : frame) {
        const uint8_t saved = invert ? uint8_t(~byte) : byte;
        EXPECT_EQ(glass(saved, /*inverted at wake=*/darkMode), glass(byte, shownInverted))
            << "darkMode=" << darkMode << " shownInverted=" << shownInverted;
      }
    }
  }
  EXPECT_FALSE(DarkMode::savedFrameInverts(true, true)) << "the dark reader page is saved as it is";
  EXPECT_TRUE(DarkMode::savedFrameInverts(false, true)) << "the image viewer under Dark Mode";
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
  const SettingInfo row = DarkModeOption::make();
  ASSERT_NE(row.key, nullptr);
  EXPECT_STREQ(row.key, "darkMode");
  EXPECT_EQ(row.type, SettingType::TOGGLE);
  EXPECT_EQ(row.valuePtr, &CrossPointSettings::darkMode);
  EXPECT_EQ(row.category, StrId::STR_CAT_DISPLAY);
  EXPECT_EQ(row.nameId, StrId::STR_DARK_MODE);
  EXPECT_EQ(S::getInstance().darkMode, 0) << "off by default";
}

}  // namespace
