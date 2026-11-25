#pragma once

#include <I18n.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "components/themes/BaseTheme.h"

// The home screen's destinations, shared by HomeActivity (the rows on the home screen) and
// HomeMoreActivity (the ones the user moved off it). One table decides the order, the label, the
// icon and where each entry goes, so the two screens cannot disagree about any of them.

enum class HomeMenuAction : uint8_t {
  Library,
  ReadingStats,
  GlobalBookmarks,
  FileTransfer,
  Weather,
  More,
  Settings,
  // The Library on its Recent tab: a destination for Home's widgets only, not a menu entry (the
  // Library entry leads there too).
  RecentBooks,
};

struct HomeMenuEntry {
  HomeMenuAction action;
  StrId label;
  UIIcon icon;
};

// What exists right now. An entry with nothing behind it (no bookmarks) is left out of both
// lists: the per-item settings choose where an entry goes, not whether it exists.
struct HomeMenuAvailability {
  bool hasBookmarks = false;
};

enum class HomeMenuPlacement : uint8_t { Home, More };

// Appends, in display order, every available entry placed at `placement`. For Home, a "More"
// entry is inserted before Settings whenever anything available was moved off the home screen,
// so hiding an entry never makes it unreachable. Settings always stays on the home screen: it is
// where the choice is undone.
//
// `homeCapacity` is how many places besides Settings the theme has for entries (BaseTheme::
// homeMenuCapacity). Entries switched on past it go behind More as if they had been moved there. A
// theme with a fixed number of places (any capacity but kHomeMenuNoLimit) fills the places the
// switched-on entries leave free with switched-off ones, in table order and after them, so its
// grid keeps its shape. The two screens ask with the same capacity and so agree on where each is.
//
// `shownByTheme` is the destinations the theme draws on Home itself (BaseTheme::
// homeShownDestinations): they are left out of both lists, so More holds what the theme does not show.
inline constexpr int kHomeMenuNoLimit = std::numeric_limits<int>::max();
void collectHomeMenuEntries(HomeMenuPlacement placement, const HomeMenuAvailability& availability,
                            std::vector<HomeMenuEntry>& out, int homeCapacity = kHomeMenuNoLimit,
                            uint32_t shownByTheme = 0);

// The destinations with something behind them now, as a bit mask over HomeDestination.
[[nodiscard]] uint32_t availableDestinations(const HomeMenuAvailability& availability);
// The menu action that opens `destination`.
[[nodiscard]] HomeMenuAction actionFor(HomeDestination destination);

// How many entries at the end of `entries` are More and Settings, the two that are not destinations
// (Settings is always last, with More directly above it when present).
[[nodiscard]] int homeMenuTrailingCount(const std::vector<HomeMenuEntry>& entries);

// The Library entry's setting for a settings file written before it existed, when Browse Files and
// Recent Books had an entry each: shown on the home screen if either was.
[[nodiscard]] uint8_t libraryOnHomeFromLegacy(uint8_t browseFiles, uint8_t recentBooks);
