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

}  // namespace CoverGridLayout
