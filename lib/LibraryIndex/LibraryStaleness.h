#pragma once

#include <cstdint>

// When the book index is reused and when it is built again (docs/design/library-index.md, "When it
// rebuilds"). The index lives on the card it describes, so it is kept across boots and wakes -- a
// different card brings its own -- and built again only for a reason.
namespace LibraryStaleness {

struct Facts {
  bool indexValid = false;      // there, with the right magic and version, its sections in the file
  bool sameHiddenRule = false;  // built under the Show Hidden Files setting now in force
  // The firmware changed the card since the last finished build, in this boot or an earlier one
  // (HalStorage::contentChangedSinceMark): an upload, a download, a move or removal on the device,
  // a USB Drive session -- or the Books tab found a change it did not make (unseenChange).
  bool cardChanged = false;
};

constexpr bool rebuildNeeded(const Facts& facts) {
  return !facts.indexValid || !facts.sameHiddenRule || facts.cardChanged;
}

// The Books tab listed a book or folder dated `entryDate` (FAT, the later of modified and created)
// while the newest indexed book is dated `newestIndexed`: anything later is not in the index, so it
// was put on the card where the firmware did not see it. An index without books compares with
// nothing.
constexpr bool unseenChange(const uint32_t entryDate, const uint32_t newestIndexed) {
  return newestIndexed != 0 && entryDate > newestIndexed;
}

}  // namespace LibraryStaleness
