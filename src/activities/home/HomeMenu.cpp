#include "HomeMenu.h"

#include <array>
#include <iterator>

#include "CrossPointSettings.h"

namespace {

// What an entry needs before it appears anywhere.
enum class Requires : uint8_t { Nothing, Bookmarks, WeatherEnabled };

struct HomeMenuRow {
  HomeMenuEntry entry;
  Requires requirement;
  // The "show on home screen" setting; nullptr pins the entry to the home screen.
  uint8_t CrossPointSettings::* shownOnHome;
};

// Display order, top to bottom (left to right on the carousel's icon row).
//
// Reading Stats requires nothing on purpose. Gating it on there being history would mean asking
// the stats store, which is deliberately not resident -- loaded on demand and released to give
// the heap back -- and the menu is rebuilt both inside and outside that window. Outside it every
// accessor reads zero, so the row would come and go depending on which rebuild ran last. The
// screen already says so plainly when there is nothing to show.
constexpr HomeMenuRow kRows[] = {
    {{HomeMenuAction::Library, StrId::STR_LIBRARY, Folder}, Requires::Nothing, &CrossPointSettings::showLibraryOnHome},
    {{HomeMenuAction::ReadingStats, StrId::STR_READING_STATS, Stats},
     Requires::Nothing,
     &CrossPointSettings::showReadingStatsOnHome},
    {{HomeMenuAction::GlobalBookmarks, StrId::STR_GLOBAL_BOOKMARKS, Book},
     Requires::Bookmarks,
     &CrossPointSettings::showBookmarksOnHome},
    {{HomeMenuAction::FileTransfer, StrId::STR_FILE_TRANSFER, Transfer},
     Requires::Nothing,
     &CrossPointSettings::showFileTransferOnHome},
    {{HomeMenuAction::Weather, StrId::STR_WEATHER, Weather},
     Requires::WeatherEnabled,
     &CrossPointSettings::showWeatherOnHome},
    {{HomeMenuAction::Settings, StrId::STR_SETTINGS_TITLE, Settings}, Requires::Nothing, nullptr},
};

constexpr HomeMenuEntry kMoreEntry{HomeMenuAction::More, StrId::STR_MORE, Ellipsis};

bool isAvailable(const Requires requirement, const HomeMenuAvailability& availability) {
  switch (requirement) {
    case Requires::Nothing:
      return true;
    case Requires::Bookmarks:
      return availability.hasBookmarks;
    case Requires::WeatherEnabled:
      return SETTINGS.useWeather != 0;
  }
  return false;
}

HomeMenuPlacement placementOf(const HomeMenuRow& row) {
  if (row.shownOnHome == nullptr || SETTINGS.*row.shownOnHome) return HomeMenuPlacement::Home;
  return HomeMenuPlacement::More;
}

}  // namespace

void collectHomeMenuEntries(const HomeMenuPlacement placement, const HomeMenuAvailability& availability,
                            std::vector<HomeMenuEntry>& out, const int homeCapacity) {
  constexpr size_t kCount = std::size(kRows);
  const auto available = [&](const size_t i) { return isAvailable(kRows[i].requirement, availability); };
  const auto pinned = [](const size_t i) { return kRows[i].shownOnHome == nullptr; };

  // Where each available row ends up. First its switch, in table order, as far as the theme has
  // places; the pinned row needs no place and never moves.
  std::array<HomeMenuPlacement, kCount> placed{};
  std::array<bool, kCount> filled{};
  int onHome = 0;
  for (size_t i = 0; i < kCount; ++i) {
    if (!available(i)) continue;
    placed[i] = placementOf(kRows[i]);
    if (pinned(i) || placed[i] != HomeMenuPlacement::Home) continue;
    if (onHome < homeCapacity) {
      ++onHome;
    } else {
      placed[i] = HomeMenuPlacement::More;
    }
  }
  // A theme with a fixed number of places fills the ones the switched-on rows leave free with
  // switched-off rows, in table order, so its grid keeps its shape; More keeps what is left.
  if (homeCapacity != kHomeMenuNoLimit) {
    for (size_t i = 0; i < kCount && onHome < homeCapacity; ++i) {
      if (!available(i) || pinned(i) || placed[i] != HomeMenuPlacement::More) continue;
      placed[i] = HomeMenuPlacement::Home;
      filled[i] = true;
      ++onHome;
    }
  }

  bool anyOnMore = false;
  for (size_t i = 0; i < kCount; ++i) {
    if (available(i) && placed[i] == HomeMenuPlacement::More) anyOnMore = true;
  }

  if (placement == HomeMenuPlacement::More) {
    for (size_t i = 0; i < kCount; ++i) {
      if (available(i) && placed[i] == HomeMenuPlacement::More) out.push_back(kRows[i].entry);
    }
    return;
  }
  // Home: the rows switched on, then those that took a free place, then More when anything is
  // behind it, then the pinned row (Settings), which More sits directly above.
  for (size_t i = 0; i < kCount; ++i) {
    if (available(i) && !pinned(i) && placed[i] == HomeMenuPlacement::Home && !filled[i]) out.push_back(kRows[i].entry);
  }
  for (size_t i = 0; i < kCount; ++i) {
    if (available(i) && filled[i]) out.push_back(kRows[i].entry);
  }
  if (anyOnMore) out.push_back(kMoreEntry);
  for (size_t i = 0; i < kCount; ++i) {
    if (available(i) && pinned(i)) out.push_back(kRows[i].entry);
  }
}

int homeMenuTrailingCount(const std::vector<HomeMenuEntry>& entries) {
  int trailing = 0;
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (it->action != HomeMenuAction::More && it->action != HomeMenuAction::Settings) break;
    ++trailing;
  }
  return trailing;
}

uint8_t libraryOnHomeFromLegacy(const uint8_t browseFiles, const uint8_t recentBooks) {
  return browseFiles != 0 || recentBooks != 0 ? 1 : 0;
}
