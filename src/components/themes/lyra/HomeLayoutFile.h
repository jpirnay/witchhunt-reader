#pragma once

#include <cstddef>
#include <string>

#include "components/themes/lyra/HomeTilesLayout.h"

// Lyra Tiles' own layout, the file the web editor writes (Layout "Custom"):
//
//   {"version": 1,
//    "rows": [[{"widget": "book"}],
//             [{"widget": "clock"}, {"widget": "picture", "path": "/home"}],
//             [{"widget": "shortcut", "to": "library"}, {"widget": "shortcut", "to": "recent"}]]}
//
// A row holds one widget across or two side by side. Widgets: book ("size": "compact" for the small
// card), also, shortcut ("to": library, stats, bookmarks, transfer, weather, recent), clock,
// calendar, week, weather, picture ("path": a JPEG, PNG or BMP, or a folder of them; /home when
// left out). What the parser does not know it leaves out, rather than refusing the file: a later
// editor may know more widgets than this firmware. Only the first book and the first also are kept.
namespace HomeLayoutFile {

inline constexpr const char* kPath = "/.crosspoint/home.json";
// Six rows of two widgets with long picture paths stay well under this.
inline constexpr size_t kMaxBytes = 4096;

struct Parsed {
  bool ok = false;  // a version-1 file with at least one widget
  HomeTilesLayout::Config config;
  // The pictures the layout names, by Cell::option; empty past the last.
  std::string pictures[HomeTilesLayout::kMaxPictures];
};

Parsed parse(const std::string& json);

// A shortcut's destination (HomeTilesLayout::destination) as the file names it, "" for none.
const char* destinationName(uint8_t destination);

}  // namespace HomeLayoutFile
