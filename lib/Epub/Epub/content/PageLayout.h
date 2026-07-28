#pragma once
// PageLayout — the microreader-style PULL layout core: compute ONE page from a cursor, live, over a
// content.bin spine, instead of paginating a whole spine to completion. This is the read-side engine
// for the single-conclusive-source design (docs/stage1-single-source-live-pagination-2026-07-27.md):
// the reader holds a PagePosition cursor and asks PageLayout for the page at that cursor; a page turn
// re-lays-out from the next/previous cursor. Nothing paginated is persisted.
//
// G2a (this file) is the SCAFFOLD increment: layout() seekToBlock()s the cursor's block and drives
// the existing, byte-identical LayoutSink for exactly ONE page (stopping at the first completed page).
// Because a valid cursor is a page boundary, LayoutSink's first emitPage() is precisely the page we
// want, and its cross-block accumulators start zeroed there (see the restart-boundary analysis in the
// design doc). This reuses the whole trusted layout engine with zero pagination-behavior risk and
// establishes the cursor + oracle-test machinery. Later increments (G2b/G3) replace the innards with a
// pure collect()-until-full core + a per-paragraph arena cache + native backward layout, keeping this
// same interface and the same position-identical gate against the whole-spine engine.

#include <cstdint>
#include <memory>

#include "BlockStreamReader.h"
#include "LayoutSink.h"

class GfxRenderer;
class Page;

namespace compiled {

// A reading cursor into a spine. `offset` is polymorphic by the block's type — line index for a text
// block, pixel row for a page-split image, row index for a table (mirrors microreader's overloaded
// PagePosition::offset). `auxFontId`/`imageCounter` are the two spine-scoped scalars that are NOT
// reconstructible from a local window (the heading aux-font latch and the image-cache-path counter),
// so they are carried in the cursor. `charOffset` is the stable re-sync key across a settings change.
struct PagePosition {
  uint16_t spineIndex = 0;
  uint16_t blockIndex = 0;  // logical block within the spine (G1 seekToBlock target)
  uint16_t offset = 0;      // line | pixel-row | table-row, by block type (0 at a block start)
  uint32_t charOffset = 0;  // absolute char offset of this position (reading progress / re-sync)
  int32_t auxFontId = 0;    // carried spine-scoped heading aux-font latch (see restart-boundary analysis)
  int imageCounter = 0;     // carried spine-scoped image-cache-path counter

  bool atSpineStart() const { return blockIndex == 0 && offset == 0; }
  // Positional equality (the layout-relevant fields — the carried scalars/charOffset are derived).
  bool samePosition(const PagePosition& o) const {
    return spineIndex == o.spineIndex && blockIndex == o.blockIndex && offset == o.offset;
  }
};

// The laid-out page plus the cursors that bound it. `page` is the standard Page the renderer already
// consumes (unchanged). `end` is the start of the NEXT page (one-past-end); `atSpineEnd` is true when
// this page is the last of the spine (next turn crosses to spine+1).
struct LaidOutPage {
  std::unique_ptr<Page> page;
  PagePosition start;
  PagePosition end;
  bool atSpineEnd = false;
  bool ok = false;
};

// Lay out exactly ONE page starting at `start`, which must be a page-boundary cursor (block start;
// G2a does not yet resume mid-block/mid-table). Drives `reader` (already open()'d, matching
// fingerprint) and a fresh internal LayoutSink built from `params`. Returns LaidOutPage{ok=false} on
// any read/layout error or if `start.spineIndex` is not available. The reader is repositioned via
// seekToBlock; the caller owns the reader and must keep it alive.
LaidOutPage layoutPage(BlockStreamReader& reader, GfxRenderer& renderer, const LayoutParams& params,
                       const PagePosition& start);

// Lay out the page that ENDS at `endCursor` (its one-past-end == endCursor), for prev-page
// navigation. `endCursor` is a page-boundary cursor within `endCursor.spineIndex` (typically the
// `start` of the page the reader is currently on). Returns that previous page + its own start cursor
// in `LaidOutPage.start` (what the reader navigates to). Guaranteed byte-identical to the forward
// page with the same boundaries: it finds the start by a bounded backward scan and then FORWARD-
// renders, reusing layoutPage. Returns ok=false at spine start (endCursor.atSpineStart(), no prior
// page in this spine — the caller crosses to the previous spine) or on error.
LaidOutPage layoutPageBackward(BlockStreamReader& reader, GfxRenderer& renderer, const LayoutParams& params,
                               const PagePosition& endCursor);

}  // namespace compiled
