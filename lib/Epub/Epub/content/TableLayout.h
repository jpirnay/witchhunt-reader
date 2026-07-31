#pragma once
// Shared table grid-layout packing (docs/parser-stage1-step5-design.md).
//
// Turns a set of pre-wrapped table rows into a run of PageTableFragment elements, greedily packing
// rows and page-breaking between fragments. The per-cell text WRAPPING (layoutAndExtractLines) is
// settings-dependent and stays with the caller (LayoutSink, which owns a renderer + fontId); this
// helper takes the ALREADY-WRAPPED rows plus the row-height/fragment math and drives page emission
// through a small callback context. It is the table analogue of computeImageDisplaySize.
//
// Column-width / narrow-column fallback and the grid-vs-paragraph decision stay with the caller
// (they decide whether to call this at all); this helper assumes the grid path was chosen.

#include <cstdint>
#include <vector>

#include "Epub/Page.h"  // TableRow, PageTableFragment, table constants

class Page;

namespace compiled {

// One pre-wrapped grid row ready for packing: its cells (the GLOBAL Page.h ::TableCell = wrapped
// TextBlock lines + optional image + isHeader — NOT compiled::TableCell, which is the serialized
// words-only form), its pixel height (content + 2*TABLE_CELL_PADDING), header flag, and how many
// columns it renders as (1 for a full-width single-cell span, else the table's column count — a
// change flushes the current fragment).
struct TableLayoutRow {
  std::vector<::TableCell> cells;
  uint16_t height = 0;
  bool isHeaderRow = false;
  uint8_t renderCols = 0;
};

// The page state the packer drives. Callers implement it over their own currentPage /
// currentPageNextY / emitPage so the packing math is identical on both sides.
struct TablePageContext {
  virtual ~TablePageContext() = default;
  virtual int currentY() const = 0;      // currentPageNextY
  virtual void ensurePage() = 0;         // create currentPage if null (nextY=0)
  virtual void emitPageAndReset() = 0;   // emit currentPage, fresh page, nextY=0
  virtual void advanceY(int delta) = 0;  // currentPageNextY += delta
  virtual void pushFragment(uint8_t cols, uint16_t totalWidth, uint16_t totalHeight, std::vector<::TableRow>&& rows,
                            int16_t yPos, bool hasBorder) = 0;
  // An over-tall row (height > viewportHeight) can't go in a grid fragment: the caller flattens
  // it to paragraphs. The helper flushes the pending fragment before invoking this so document
  // order is preserved.
  virtual void onOversizeRow(const TableLayoutRow& row) = 0;
};

// Greedily pack `rows` into PageTableFragments, page-breaking between them: a fragment flushes on a
// renderCols change or when the next row would overflow the viewport; a bordered fragment/row adds
// +1px. Over-tall rows are routed to ctx.onOversizeRow after flushing the pending fragment.
void packTableFragments(const std::vector<TableLayoutRow>& rows, uint16_t totalWidth, uint16_t viewportHeight,
                        bool hasBorder, TablePageContext& ctx);

}  // namespace compiled
