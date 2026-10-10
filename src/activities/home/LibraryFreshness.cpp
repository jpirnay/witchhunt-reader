#include "LibraryFreshness.h"

#include <HalStorage.h>
#include <LibraryIndexReader.h>
#include <LibraryStaleness.h>

#include "CrossPointSettings.h"

namespace {
// The newest indexed book's date, read from the index once; -1 when not read yet.
int64_t newestIndexedDate = -1;
}  // namespace

namespace LibraryFreshness {

bool stale(const LibraryIndexReader& index) {
  LibraryStaleness::Facts facts;
  facts.indexValid = index.isOpen();
  facts.sameHiddenRule =
      facts.indexValid && index.header().acceptRules == library::acceptRules(SETTINGS.showHiddenFiles);
  facts.cardChanged = Storage.contentChangedSinceMark();
  return LibraryStaleness::rebuildNeeded(facts);
}

void built(const uint32_t generationAtStart) {
  newestIndexedDate = -1;  // the index was replaced
  // A change while the build ran may have come after the walk passed it: the marker stays for it.
  Storage.markContentSeen(generationAtStart);
}

void published() { newestIndexedDate = -1; }

void checkListedEntry(const uint32_t entryDate) {
  if (Storage.contentChangedSinceMark()) return;  // a rebuild is due already
  if (newestIndexedDate < 0) {
    LibraryIndexReader index;
    // A partial index (more books than it holds) leaves books out whatever their date: no comparison.
    const bool usable = index.open(library::INDEX_PATH) && (index.header().flags & library::FLAG_PARTIAL) == 0;
    newestIndexedDate = usable ? index.header().newestDate : 0;
  }
  if (LibraryStaleness::unseenChange(entryDate, static_cast<uint32_t>(newestIndexedDate))) Storage.noteFoundChange();
}

}  // namespace LibraryFreshness
