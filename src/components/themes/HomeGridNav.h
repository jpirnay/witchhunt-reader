#pragma once

#include <cstdint>

// Home as rows of selector positions, for a theme that lays it out in two dimensions (Lyra Tiles)
// and a board with a key for each direction (HalCapabilities::hasFourDirectionKeys): Up and Down go
// to the row above or below, Left and Right along the row. On the book card, which is a row of its
// own, Left and Right bring the previous or next recent book instead.
//
// A row's items are consecutive selector positions, as Home's selector runs: the books it shows,
// then the menu in order. Pure integer arithmetic, run on the host (test/home_tiles_layout).
namespace HomeGridNav {

struct Grid {
  static constexpr int kMaxRows = 8;
  int rowCount = 0;  // 0: no grid, Home steps through its items as a list
  int first[kMaxRows]{};
  int count[kMaxRows]{};
  // The first row is the book card: Left and Right cycle the recent books there.
  bool firstRowCyclesBooks = false;

  constexpr void addRow(const int firstIndex, const int items) {
    if (items <= 0 || rowCount >= kMaxRows) return;
    first[rowCount] = firstIndex;
    count[rowCount] = items;
    ++rowCount;
  }
};

enum class Move : uint8_t { Up, Down, Left, Right };

struct Step {
  int index;  // the selector position to go to
  int cycle;  // -1 / 1: bring the previous / next book onto the card instead; 0 otherwise
};

constexpr Step step(const Grid& grid, const int current, const Move move) {
  int row = -1;
  for (int r = 0; r < grid.rowCount; ++r) {
    if (current >= grid.first[r] && current < grid.first[r] + grid.count[r]) {
      row = r;
      break;
    }
  }
  if (row < 0) return Step{current, 0};
  const int n = grid.count[row];
  const int column = current - grid.first[row];

  switch (move) {
    case Move::Left:
    case Move::Right: {
      const int delta = move == Move::Right ? 1 : -1;
      if (row == 0 && grid.firstRowCyclesBooks) return Step{current, delta};
      return Step{grid.first[row] + (column + delta + n) % n, 0};
    }
    case Move::Up:
    case Move::Down: {
      const int target = (row + (move == Move::Down ? 1 : -1) + grid.rowCount) % grid.rowCount;
      const int m = grid.count[target];
      // The item under the middle of this one: from a single item (the card, a spanning tile) the
      // first of the row; between rows of two the same side; from three to two the nearer side.
      int to = n == 1 ? 0 : (2 * column + 1) * m / (2 * n);
      if (to > m - 1) to = m - 1;
      return Step{grid.first[target] + to, 0};
    }
  }
  return Step{current, 0};
}

}  // namespace HomeGridNav
