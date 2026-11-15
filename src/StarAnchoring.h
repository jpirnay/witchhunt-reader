#pragma once

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
// Each mapping parses the chapter's XHTML once, which is idle-time work on the X3 (step()). What they turn into, the
// visible-text offset of the page's start, is kept for the session (StarAnchor::offset), so a second relayout in the
// same session, or a jump to a star already settled, parses nothing.
namespace StarAnchoring {

// The anchor a new star on `page` of `section` gets: the section's layout, its page count once the build is done (0
// while it runs), and where the page's text starts when the section's table says. The XPath is left to step().
StarAnchor forPage(const Section& section, int page);

enum class Work : uint8_t {
  None,    // nothing owed
  Cheap,   // stars moved from offsets already known: no parse
  Parsed,  // one chapter parsed; more may be owed, for a later call
};

// Does what `store` owes: when `section` is given (complete, no build running, of spine `spineIndex`), moves that
// spine's stars counted in another layout onto the page holding their text, resolving at most one XPath; then, if
// nothing was parsed yet, names one new star's text. At most one chapter parse a call. A star without an anchor (TXT,
// older firmware) is left as it is.
Work step(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const Section* section, int spineIndex);

// Where star `index`'s text starts, for a jump to it: what this session knows, else its XPath resolved (one chapter
// parse). nullopt: no XPath, or it did not resolve; the jump then goes by the page number.
std::optional<uint32_t> textOffset(BookmarkStore& store, size_t index, const std::shared_ptr<Epub>& epub);

}  // namespace StarAnchoring
