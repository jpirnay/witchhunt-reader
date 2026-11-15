#pragma once

#include <BuildArena.h>
#include <Epub.h>
#include <Epub/Section.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "BookmarkStore.h"

// Starred EPUB pages through a relayout, by the route KOReader sync takes. A star names its page's first text with the
// KOReader XPath, as a push does (ChapterXPathIndexer::findXPathForVisibleOffset), and finds that text again in
// another layout from the XPath, as a pull does (ChapterXPathIndexer::findProgressForXPath, then
// Section::getPageForVisibleTextOffset). No mapping of its own: both directions are the sync's mappers.
//
// Each mapping parses the chapter's XHTML once, which is idle-time work on the X3 (the reader's stepStarredPages()).
// What they turn into, the visible-text offset of the page's start, is kept with the star (StarAnchor::offset), so a
// relayout or a jump parses nothing while the section format it was counted under stands. `scratch`, where given, is
// a lent region the parse works in instead of the heap (ChapterXPathIndexer): the reader on the X3 has been measured
// with less contiguous heap than the inflate ring's 32 KB.
namespace StarAnchoring {

// The star's text offset while it can be trusted: counted under the section format this firmware has. Under another
// format the count may differ, and the XPath finds the text again (parseNext(), textOffset()), which refreshes it.
std::optional<uint32_t> knownOffset(const StarAnchor& anchor);

// The anchor a new star on `page` of `section` gets: the section's layout, its page count once the build is done (0
// while it runs), and where the page's text starts when the section's table says. The XPath is left to parseNext().
StarAnchor forPage(const Section& section, int page);

enum class Work : uint8_t {
  None,    // nothing owed
  Cheap,   // stars moved from offsets already known: no parse
  Parsed,  // one chapter parsed; more may be owed, for a later call
};

// The part that parses nothing: `section`'s spine's stars counted in another layout whose text offset is known (or,
// for a star made while that layout was being built, the table gives) move onto the page holding it, and bookmarks
// from older firmware are anchored where they are, their text to be named. True when anything changed. `section` must
// be complete: loaded, or built to the end.
bool settleKnown(BookmarkStore& store, const Section& section, int spineIndex);

// Whether parseNext() has a chapter to parse: a star of `spineIndex` counted in another layout than `section` (when
// given) with only its XPath to go by, or a new star's text to name. Looks at the store only, so a caller lends memory
// for a parse only when one is coming.
bool parseOwed(const BookmarkStore& store, const Section* section, int spineIndex);

// One chapter parse, the first owed: such a star's XPath resolved and the star moved, else a new star's text named.
// None when nothing is owed. A failed mapping is not tried again this session.
Work parseNext(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const Section* section, int spineIndex,
               BuildArena* scratch = nullptr);

// settleKnown() when `section` is given, and when it had nothing to do, parseNext(). For the tests, and for a caller
// that need not choose where a parse works.
Work step(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const Section* section, int spineIndex,
          BuildArena* scratch = nullptr);

// Where star `index`'s text starts, for a jump to it: what this session knows, else its XPath resolved (one chapter
// parse). nullopt: no XPath, or it did not resolve; the jump then goes by the page number.
std::optional<uint32_t> textOffset(BookmarkStore& store, size_t index, const std::shared_ptr<Epub>& epub,
                                   BuildArena* scratch = nullptr);

}  // namespace StarAnchoring
