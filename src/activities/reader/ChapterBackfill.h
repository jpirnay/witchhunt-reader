#pragma once

// The chapter page count's backfill. The count of a chapter split over several spine items is exact only once each of
// its files has been laid out under the current settings (ChapterPageSpan, SpinePageIndex), and Background-B lays out
// only the files after the one on screen. A reader who came into the chapter past its first file (a saved position
// after a font change, a bookmark, a jump, a chapter whose heading is a file of its own) kept the "~" to the chapter's
// last page. So once the look-ahead has nothing left to do, B lays out the chapter's earlier files too, the nearest
// first. Class 3 work (docs/future_work/reader-priority-scheme.md): under B's gates, and it never asks for a refresh
// (rule R6): the next draw that happens anyway shows the exact count.
namespace ChapterBackfill {

struct State {
  bool counterShown = false;      // the status bar shows the chapter's count (as text or a chapter progress bar)
  bool chapterBuilding = false;   // the chapter on screen is still being laid out (Background-C)
  bool pausedForChapter = false;  // B paused its look-ahead until the next chapter (memory ran short)
  bool doneForSpine = false;      // the chapter has no such file left, or its backfill gave up
  bool backfilling = false;       // B works on such a file now
};

// Whether B may look for an earlier file of the chapter to lay out.
constexpr bool mayLook(const State& s) {
  return s.counterShown && !s.chapterBuilding && !s.pausedForChapter && !s.doneForSpine && !s.backfilling;
}

// Whether B takes `target` (-1: the page index found none). A target laid out already for this spine and still
// without a count (a file of no pages, which records none) ends the backfill rather than be laid out again and again.
constexpr bool takes(const int target, const int lastTarget) { return target >= 0 && target != lastTarget; }

}  // namespace ChapterBackfill
