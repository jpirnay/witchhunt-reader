#pragma once

#include <cstdint>

// Home as rows of selector positions, in the order they appear on the screen, for a theme that
// lays Home out in two dimensions (Lyra Tiles). With a key for each direction
// (HalCapabilities::hasFourDirectionKeys), Up and Down go to the row above or below and Left and
// Right along the row; on the book card, Left and Right bring the previous or next recent book.
// With Up and Down only, Home steps through the cells in this order (next()), so the selection
// follows the screen even where the selector's positions do not: a widget between the card and the
// tiles opens something that comes after the menu in Home's numbering.
//
// Pure integer arithmetic, run on the host (test/home_tiles_layout).
namespace HomeGridNav {

struct Grid {
  static constexpr int kMaxRows = 12;
  static constexpr int kMaxCells = 32;
  int rowCount = 0;  // 0: no grid, Home steps through its items as a list
  // Small types: Home keeps a grid for as long as it is open, on the X3's heap.
  uint8_t first[kMaxRows]{};  // a row's first cell in `cells`
  uint8_t count[kMaxRows]{};
  int16_t cells[kMaxCells]{};  // selector positions, row by row
  int cellCount = 0;
  // The row of the book card, where Left and Right cycle the recent books; -1 for none.
  int cycleRow = -1;

  // A row of `items` consecutive selector positions from `firstIndex`.
  constexpr void addRow(const int firstIndex, const int items) {
    if (items <= 0 || rowCount >= kMaxRows || cellCount + items > kMaxCells) return;
    first[rowCount] = static_cast<uint8_t>(cellCount);
    count[rowCount] = static_cast<uint8_t>(items);
    for (int i = 0; i < items; ++i) cells[cellCount++] = static_cast<int16_t>(firstIndex + i);
    ++rowCount;
  }
  // A row of the `items` selector positions in `indices`, left to right.
  constexpr void addCells(const int* indices, const int items) {
    if (items <= 0 || rowCount >= kMaxRows || cellCount + items > kMaxCells) return;
    first[rowCount] = static_cast<uint8_t>(cellCount);
    count[rowCount] = static_cast<uint8_t>(items);
    for (int i = 0; i < items; ++i) cells[cellCount++] = static_cast<int16_t>(indices[i]);
    ++rowCount;
  }
  // The row just added is the book card.
  constexpr void markCycleRow() {
    if (rowCount > 0) cycleRow = rowCount - 1;
  }
};

enum class Move : uint8_t { Up, Down, Left, Right };

struct Step {
  int index;  // the selector position to go to
  int cycle;  // -1 / 1: bring the previous / next book onto the card instead; 0 otherwise
};

// Where `current` sits: its row and its column in that row; row -1 when it is in no row.
struct Where {
  int row = -1;
  int column = 0;
};

constexpr Where find(const Grid& grid, const int current) {
  for (int r = 0; r < grid.rowCount; ++r) {
    for (int c = 0; c < grid.count[r]; ++c) {
      if (grid.cells[grid.first[r] + c] == current) return Where{r, c};
    }
  }
  return Where{};
}

constexpr Step step(const Grid& grid, const int current, const Move move) {
  const Where at = find(grid, current);
  if (at.row < 0) return Step{current, 0};
  const int row = at.row;
  const int n = grid.count[row];
  const int column = at.column;

  switch (move) {
    case Move::Left:
    case Move::Right: {
      const int delta = move == Move::Right ? 1 : -1;
      if (row == grid.cycleRow) return Step{current, delta};
      return Step{grid.cells[grid.first[row] + (column + delta + n) % n], 0};
    }
    case Move::Up:
    case Move::Down: {
      const int target = (row + (move == Move::Down ? 1 : -1) + grid.rowCount) % grid.rowCount;
      const int m = grid.count[target];
      // The item under the middle of this one: from a single item (the card, a spanning tile) the
      // first of the row; between rows of two the same side; from three to two the nearer side.
      int to = n == 1 ? 0 : (2 * column + 1) * m / (2 * n);
      if (to > m - 1) to = m - 1;
      return Step{grid.cells[grid.first[target] + to], 0};
    }
  }
  return Step{current, 0};
}

// The cell `delta` (+1 / -1) after `current` in screen order, wrapping; `current` itself when the
// grid is empty. A position in no row goes to the first cell.
constexpr int next(const Grid& grid, const int current, const int delta) {
  if (grid.cellCount == 0) return current;
  int at = -1;
  for (int i = 0; i < grid.cellCount; ++i) {
    if (grid.cells[i] == current) {
      at = i;
      break;
    }
  }
  if (at < 0) return grid.cells[0];
  return grid.cells[(at + delta % grid.cellCount + grid.cellCount) % grid.cellCount];
}

}  // namespace HomeGridNav
