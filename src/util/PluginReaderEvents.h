#pragma once

#include <cstdint>
#include <string>

// The reader's side of the plugin events (PluginEvents.h): reader.open, reader.session, reader.exit, and the open book
// sleep.enter reports. The readers call these where the reading statistics hook in: opened() beside
// ReadingSessionTracker::begin(), shown() where a landed page's progress is saved, closed() beside end(). A user's page
// turn reaches turned() through ReadingSessionTracker::onPageTurn(), which jumps (a chapter, a link, a search hit) do
// not call; the next shown() tells forward from back by the page's position. Nothing is sent, or computed, for an
// event no plugin subscribes to.
//
// reader.session is upstream CrossPoint's (PR #3114, Justin Mitchell): the time a page stayed on screen before an
// ordinary forward turn, 2 s to 30 min a page (ReaderSession.h).
namespace plugin_events::reader {

void opened(const std::string& bookPath);
// True while a book is open and a plugin subscribes: the readers compute shown()'s progress only then.
bool tracking();
// A user's page turn, either way.
void turned();
// The visible page landed. `position` orders the book's pages (a later page, a larger number); `progressBp` is the
// whole-book progress at the page's top, 0..10000; `endPercent` at its bottom, 100 on the last page.
void shown(uint32_t position, int progressBp, int endPercent);
// reader.session for what was read, then reader.exit.
void closed();
// reader.session for what was read so far (before sleep.enter); the book stays open.
void flushSession();
// The open book and its last landed page's percents (sleep.enter's variables). False with no book open.
bool current(std::string& bookPath, int& percent, int& endPercent);

}  // namespace plugin_events::reader
