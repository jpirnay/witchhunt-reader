#pragma once

// What a reader render pass records once its page is on the panel (the tail of EpubReaderActivity::renderNormalPass),
// decided from the page the pass drew, never from where the reader is by then.
//
// renderContents() gives the render lock up for the waveform so the loop task can take input while the panel
// refreshes, and the loop task uses it: a page turn landing in that window moves the position on, and one that crosses
// into another spine resets the section (stepPageStateLocked). The pass relocks to a position that is no longer its
// page. It used to record that live position as the progress, a page not drawn yet (in a burst of turns, one or two
// ahead of the screen). When the turn had crossed a spine there was no section to read: it logged "section became null
// after display completion" as an error and dropped the rest of its bookkeeping, so the page it drew was never
// recorded. X3, 2026-10-20: 42 of 45 spine crossings in 30-press bursts (the T5S3 the same). A turn in that window is
// the reader working as designed: the navigation has asked for the pass that draws, and records, its new position.
namespace DisplayedPage {

// The page a pass drew, pinned while the pass still held the lock (lastRendered* in the reader).
struct Identity {
  int spine = -1;
  int page = 0;
  int pageCount = 0;
};

// Where the reader is once the pass has the lock back. `hasSection` false: a navigation reset the section (a turn
// across a spine boundary, a jump), and the pass that loads the next one has not run yet.
struct LivePosition {
  bool hasSection = false;
  int spine = -1;
  int page = 0;
};

struct Settlement {
  // The position moved on while the page was going out to the panel.
  bool superseded = false;
  // Publish `progress` as the reading position. The drawn page is what the reader last saw; the pass the navigation
  // asked for records the newer position after it, through the same slot, so the order holds. Not when the
  // navigation left the book's pages: the finished-book pass writes its 100% record directly, and a page record
  // published here would be saved after it.
  bool recordProgress = false;
  Identity progress;
};

constexpr Settlement settle(const Identity& drawn, const LivePosition& live, const int spineCount) {
  Settlement s;
  s.superseded = !live.hasSection || live.spine != drawn.spine || live.page != drawn.page;
  s.recordProgress = live.spine < spineCount;
  s.progress = drawn;
  return s;
}

// The page the status bar on the panel was drawn for (renderStatusBar()), by every pass that shows one: a fresh render,
// a pre-rendered page, a page shown mid-build.
struct StatusBarPage {
  int spine = -1;
  int page = -1;  // 1-based, as the status bar counts; -1 before the first status bar
};

// A render of the page the status bar already shows is a clock or battery tick, not a page turn: it refreshes FAST and
// spends nothing from the refresh budget, which counts pages. Page k of one spine and page k of the next are two
// pages; told apart by the number alone, a turn between them was billed as a tick, and a run of one-page spines
// never reached the periodic HALF.
constexpr bool redrawsStatusBarPage(const StatusBarPage& shown, const int spine, const int page) {
  return shown.page >= 0 && shown.spine == spine && shown.page == page;
}

}  // namespace DisplayedPage
