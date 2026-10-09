#include "CoverGridLayout.h"

#include <algorithm>

namespace CoverGridLayout {

Layout compute(const Input& in) {
  Layout l;

  const int maxCellHeight = std::max(kMinCellHeight, in.maxCellHeight);
  const int usableHeight = std::max(0, in.contentHeight - std::max(0, in.bottomReserve));

  // Rows are counted at full cell size -- unless that leaves at least half a row empty: then one
  // more row, and every cell shrinks to fit. At the full-size 242 px cell that shrink is bounded:
  // to no less than 170 px when one row becomes two, 194 px for two becoming three, closer after.
  const int fullStride = maxCellHeight + kLabelHeight + kMargin;
  l.rows = usableHeight / fullStride;
  if (usableHeight - l.rows * fullStride >= fullStride / 2) ++l.rows;
  l.rows = std::max(1, l.rows);

  // The height left over goes into taller cells, up to the ceiling. The lower clamp only bites on
  // a panel too short to hold even one shrunk row.
  const int fitted = usableHeight / l.rows - kLabelHeight - kMargin;
  l.cellHeight = std::max(kMinCellHeight, std::min(maxCellHeight, fitted));

  // Height the capped cells leave over goes to the rows equally, rather than as a blank band under
  // the last one; where each row's share holds another label line, the label takes a third line.
  const int baseStride = l.cellHeight + kLabelHeight + kMargin;
  const int spare = std::max(0, usableHeight / l.rows - baseStride);
  l.labelLines = spare >= kLabelLineHeight ? 3 : 2;
  l.labelHeight = kLabelHeight + (l.labelLines - 2) * kLabelLineHeight;
  l.rowStride = baseStride + spare;

  // As many columns as hold a 2:3 cover at that height -- the usual shape, and what a fitted cover
  // of it fills -- each with its margin. A panel too narrow for even one still gets one column (a
  // squeezed cover beats an empty screen).
  const int coverWidth = (l.cellHeight - 2) * 2 / 3 + 2;
  const int usableWidth = std::max(0, in.contentWidth);
  l.cols = std::max(1, (usableWidth - kMargin) / (coverWidth + kMargin));
  l.cellWidth = std::max(1, (usableWidth - (l.cols + 1) * kMargin) / l.cols);
  l.labelWidth = std::max(0, l.cellWidth - 4);

  const int thumbCellWidth = in.maxCellWidth > 0 ? std::min(in.maxCellWidth, l.cellWidth) : l.cellWidth;
  l.thumbWidth = std::max(1, thumbCellWidth - 2);
  l.thumbHeight = std::max(1, l.cellHeight - 2);
  return l;
}

Placement place(const Screen& screen) {
  Placement p;
  const int gap = screen.tabBarHeight > 0 ? kTabBarGap : screen.verticalSpacing;
  p.top = screen.topPadding + screen.headerHeight + screen.tabBarHeight + gap;
  p.cells = compute({.contentWidth = screen.contentWidth,
                     .contentHeight = screen.contentBottom - p.top - screen.verticalSpacing,
                     .bottomReserve = 0,
                     .maxCellHeight = kMaxCellHeight,
                     .maxCellWidth = kMaxCellWidth});
  return p;
}

int rowBelow(const int index, const int count, const int cols) {
  if (count <= 0 || cols <= 0) return 0;
  if (index + cols < count) return index + cols;
  const int lastRowStart = (count - 1) / cols * cols;
  if (index < lastRowStart) return count - 1;  // the partial last row has no cell in this column
  return index % cols;                         // from the last row: back to the first
}

int rowAbove(const int index, const int count, const int cols) {
  if (count <= 0 || cols <= 0) return 0;
  if (index - cols >= 0) return index - cols;
  const int col = index % cols;
  return col + (count - 1 - col) / cols * cols;  // the same column, on the last row that has it
}

int hitTest(const Layout& l, const int originX, const int originY, const int pageStartRow, const int itemCount,
            const int px, const int py) {
  if (l.cols <= 0 || l.rows <= 0 || l.cellWidth <= 0 || l.rowStride <= 0) return -1;

  // Rows first: the stride includes the trailing margin, so the hit band is the cell's own
  // height and the remainder of the stride is gutter.
  const int dy = py - originY;
  if (dy < 0) return -1;
  const int row = dy / l.rowStride;
  if (row >= l.rows) return -1;
  if (dy - row * l.rowStride >= l.cellHeight + l.labelHeight) return -1;

  // Columns: same shape, with the leading margin taken off first.
  const int dx = px - originX - kMargin;
  if (dx < 0) return -1;
  const int colStride = l.cellWidth + kMargin;
  const int col = dx / colStride;
  if (col >= l.cols) return -1;
  if (dx - col * colStride >= l.cellWidth) return -1;

  const int index = (pageStartRow + row) * l.cols + col;
  if (index < 0 || index >= itemCount) return -1;  // the last row is usually partial
  return index;
}

}  // namespace CoverGridLayout
