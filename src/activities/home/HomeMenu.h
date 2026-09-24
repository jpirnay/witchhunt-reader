#pragma once

#include <I18n.h>

#include <cstdint>
#include <vector>

#include "components/themes/BaseTheme.h"

// The home screen's destinations, shared by HomeActivity (the rows on the home screen) and
// HomeMoreActivity (the ones the user moved off it). One table decides the order, the label, the
// icon and where each entry goes, so the two screens cannot disagree about any of them.

enum class HomeMenuAction : uint8_t {
  FileBrowser,
  Recents,
  ReadingStats,
  GlobalBookmarks,
  OpdsBrowser,
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

// What exists right now. An entry with nothing behind it (no bookmarks, no OPDS server) is left
// out of both lists: the per-item settings choose where an entry goes, not whether it exists.
struct HomeMenuAvailability {
  bool hasBookmarks = false;
  bool hasOpdsServers = false;
};

enum class HomeMenuPlacement : uint8_t { Home, More };

// Appends, in display order, every available entry placed at `placement`. For Home, a "More"
// entry is inserted before Settings whenever anything available was moved off the home screen,
// so hiding an entry never makes it unreachable. Settings always stays on the home screen: it is
// where the choice is undone.
void collectHomeMenuEntries(HomeMenuPlacement placement, const HomeMenuAvailability& availability,
                            std::vector<HomeMenuEntry>& out);
