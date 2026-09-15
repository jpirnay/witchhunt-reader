#pragma once

#include <string>

#include "GfxRenderer.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"  // Rect

// Reading-progress presentation for a book: where the percentage comes from, how the
// pace-based estimate is worded, and what the two cover overlays look like.
//
// Split out of UITheme, which selects and owns the look-and-feel and should not also know
// about document IDs, the on-SD progress cache or the reading-stats layer. The themes call
// in here directly, so nothing in the theme stack has to reach back up into UITheme for a
// number that has nothing to do with theming.
//
// Free functions, no state: there is nothing to keep between calls, and a class here would
// cost an object for the sake of a namespace.
namespace BookProgressPresentation {

// Overall progress percent (0..100) for a recent book, read from its cached progress.bin.
// Cheap: derives the cache path from the book path, no book parsing. Returns -1 when the
// book was never opened or the percent byte isn't written yet.
int readPercent(const RecentBook& book);

// Draws a reading-progress overlay directly on a cover thumbnail: a thin bar along the
// bottom edge while in progress (1..99%), a folded top-right corner when finished (100%),
// and nothing for unread books (0% / <0). Drawn with a white halo so it stays legible over
// any cover art on the 1-bit display. `coverRect` is the cover frame including its 1px border.
void drawIndicator(const GfxRenderer& renderer, Rect coverRect, int progressPercent);

// Compact progress/ETA status string for a recent book, e.g. "62% · ~45m" — the percentage
// plus a pace-based time-to-finish estimate from the reading-stats layer. The ETA suffix is
// dropped when the book is finished or when there's too little history to project a
// meaningful pace. The "~<time>" form is intentionally language-neutral (no translation
// string needed). Returns "" when the book has no progress data (progressPercent < 0).
std::string formatStatus(const RecentBook& book, int progressPercent);

// Draws formatStatus()'s content as a filled pill badge inset into the top-right corner of a
// cover. Used where the cover is large enough to carry an overlaid label (e.g. the carousel
// centre cover). Draws nothing when the book has no progress data.
void drawBadge(const GfxRenderer& renderer, Rect coverRect, const RecentBook& book, int progressPercent);

}  // namespace BookProgressPresentation
