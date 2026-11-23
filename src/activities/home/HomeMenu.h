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
// `homeCapacity` is how many entries besides Settings the theme has room for (BaseTheme::
// homeMenuCapacity). Entries placed on Home past it go behind More as if they had been moved there,
// so the two screens, which ask with the same capacity, agree on where each one is.
void collectHomeMenuEntries(HomeMenuPlacement placement, const HomeMenuAvailability& availability,
                            std::vector<HomeMenuEntry>& out, int homeCapacity = std::numeric_limits<int>::max());

// How many entries at the end of `entries` are More and Settings, the two that are not destinations
// (Settings is always last, with More directly above it when present).
[[nodiscard]] int homeMenuTrailingCount(const std::vector<HomeMenuEntry>& entries);

// The Library entry's setting for a settings file written before it existed, when Browse Files and
// Recent Books had an entry each: shown on the home screen if either was.
[[nodiscard]] uint8_t libraryOnHomeFromLegacy(uint8_t browseFiles, uint8_t recentBooks);
