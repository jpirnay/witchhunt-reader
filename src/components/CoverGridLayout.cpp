#include "CoverGridLayout.h"

#include <algorithm>

namespace CoverGridLayout {

Layout compute(const Input& in) {
  Layout l;

  // Widest grid whose cells still clear kMinCellWidth: cols cells plus (cols + 1) margins have to
  // fit. A panel too narrow for even one full-size cell still gets one column (a squeezed cover
  // beats an empty screen).
  const int usableWidth = std::max(0, in.contentWidth);
  l.cols = std::max(1, (usableWidth - kMargin) / (kMinCellWidth + kMargin));
  l.cellWidth = std::max(1, (usableWidth - (l.cols + 1) * kMargin) / l.cols);
  l.labelWidth = std::max(0, l.cellWidth - 4);

  const int maxCellHeight = std::max(kMinCellHeight, in.maxCellHeight);
  const int usableHeight = std::max(0, in.contentHeight - std::max(0, in.bottomReserve));

  // Rows are counted at full cell size, so a page never trades cover size for density.
  const int fullStride = maxCellHeight + kLabelHeight + kMargin;
  l.rows = std::max(1, usableHeight / fullStride);

  // Whatever height is left over goes into taller cells, up to the ceiling. The lower clamp only
  // bites on a panel too short to hold even one full-size row.
  const int fitted = usableHeight / l.rows - kLabelHeight - kMargin;
  l.cellHeight = std::max(kMinCellHeight, std::min(maxCellHeight, fitted));
  l.rowStride = l.cellHeight + kLabelHeight + kMargin;
  return l;
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
  if (dy - row * l.rowStride >= l.cellHeight + kLabelHeight) return -1;

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
