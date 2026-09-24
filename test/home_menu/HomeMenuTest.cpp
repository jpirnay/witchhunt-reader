// Cover for where each home screen entry ends up: on the home screen, behind its "More" entry,
// or nowhere. The one promise the "More" entry makes is that hiding an entry never makes it
// unreachable -- and, the other way round, that a More row never opens onto an empty list.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/home/HomeMenu.h"

CrossPointSettings CrossPointSettings::instance;

namespace {

using A = HomeMenuAction;

std::vector<A> actionsAt(const HomeMenuPlacement placement, const HomeMenuAvailability& availability) {
  std::vector<HomeMenuEntry> entries;
  collectHomeMenuEntries(placement, availability, entries);
  std::vector<A> actions;
  for (const HomeMenuEntry& entry : entries) actions.push_back(entry.action);
  return actions;
}

class HomeMenuTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SETTINGS.showBrowseFilesOnHome = 1;
    SETTINGS.showRecentBooksOnHome = 1;
    SETTINGS.showReadingStatsOnHome = 1;
    SETTINGS.showBookmarksOnHome = 1;
    SETTINGS.showOpdsBrowserOnHome = 1;
    SETTINGS.showFileTransferOnHome = 1;
    SETTINGS.showWeatherOnHome = 1;
    SETTINGS.useWeather = 1;
  }

  HomeMenuAvailability everything{.hasBookmarks = true, .hasOpdsServers = true};
  HomeMenuAvailability nothingOptional{.hasBookmarks = false, .hasOpdsServers = false};
};

TEST_F(HomeMenuTest, DefaultsShowEverythingAvailableAndNoMore) {
  EXPECT_EQ(actionsAt(HomeMenuPlacement::Home, everything),
            (std::vector<A>{A::FileBrowser, A::Recents, A::ReadingStats, A::GlobalBookmarks, A::OpdsBrowser,
                            A::FileTransfer, A::Weather, A::Settings}));
  EXPECT_TRUE(actionsAt(HomeMenuPlacement::More, everything).empty());
}

TEST_F(HomeMenuTest, EntriesWithNothingBehindThemAppearNowhere) {
  EXPECT_EQ(actionsAt(HomeMenuPlacement::Home, nothingOptional),
            (std::vector<A>{A::FileBrowser, A::Recents, A::ReadingStats, A::FileTransfer, A::Weather, A::Settings}));
}

TEST_F(HomeMenuTest, AHiddenEntryMovesBehindMoreWhichSitsAboveSettings) {
  SETTINGS.showFileTransferOnHome = 0;

  EXPECT_EQ(actionsAt(HomeMenuPlacement::Home, everything),
            (std::vector<A>{A::FileBrowser, A::Recents, A::ReadingStats, A::GlobalBookmarks, A::OpdsBrowser, A::Weather,
                            A::More, A::Settings}));
  EXPECT_EQ(actionsAt(HomeMenuPlacement::More, everything), (std::vector<A>{A::FileTransfer}));
}

TEST_F(HomeMenuTest, MoreKeepsTheHomeScreenOrder) {
  SETTINGS.showWeatherOnHome = 0;
  SETTINGS.showBrowseFilesOnHome = 0;
  SETTINGS.showOpdsBrowserOnHome = 0;

  EXPECT_EQ(actionsAt(HomeMenuPlacement::More, everything),
            (std::vector<A>{A::FileBrowser, A::OpdsBrowser, A::Weather}));
}

// Hiding OPDS with no server configured hides nothing the user could open, so a More row here
// would lead to an empty list.
TEST_F(HomeMenuTest, HidingAnUnavailableEntryAddsNoMore) {
  SETTINGS.showOpdsBrowserOnHome = 0;
  SETTINGS.showBookmarksOnHome = 0;

  const auto home = actionsAt(HomeMenuPlacement::Home, nothingOptional);
  EXPECT_EQ(std::count(home.begin(), home.end(), A::More), 0);
  EXPECT_TRUE(actionsAt(HomeMenuPlacement::More, nothingOptional).empty());
}

TEST_F(HomeMenuTest, WeatherTurnedOffIsInNeitherPlace) {
  SETTINGS.useWeather = 0;
  SETTINGS.showWeatherOnHome = 0;

  const auto home = actionsAt(HomeMenuPlacement::Home, everything);
  EXPECT_EQ(std::count(home.begin(), home.end(), A::Weather), 0);
  EXPECT_EQ(std::count(home.begin(), home.end(), A::More), 0);
  EXPECT_TRUE(actionsAt(HomeMenuPlacement::More, everything).empty());
}

TEST_F(HomeMenuTest, SettingsStaysWhenEverythingElseIsHidden) {
  SETTINGS.showBrowseFilesOnHome = 0;
  SETTINGS.showRecentBooksOnHome = 0;
  SETTINGS.showReadingStatsOnHome = 0;
  SETTINGS.showBookmarksOnHome = 0;
  SETTINGS.showOpdsBrowserOnHome = 0;
  SETTINGS.showFileTransferOnHome = 0;
  SETTINGS.showWeatherOnHome = 0;

  EXPECT_EQ(actionsAt(HomeMenuPlacement::Home, everything), (std::vector<A>{A::More, A::Settings}));
  EXPECT_EQ(actionsAt(HomeMenuPlacement::More, everything),
            (std::vector<A>{A::FileBrowser, A::Recents, A::ReadingStats, A::GlobalBookmarks, A::OpdsBrowser,
                            A::FileTransfer, A::Weather}));
}

// Every entry is reachable from exactly one place, whatever the settings say.
TEST_F(HomeMenuTest, NoEntryIsInBothPlaces) {
  SETTINGS.showRecentBooksOnHome = 0;
  SETTINGS.showWeatherOnHome = 0;

  const auto home = actionsAt(HomeMenuPlacement::Home, everything);
  for (const A action : actionsAt(HomeMenuPlacement::More, everything)) {
    EXPECT_EQ(std::count(home.begin(), home.end(), action), 0) << static_cast<int>(action);
  }
}

}  // namespace
