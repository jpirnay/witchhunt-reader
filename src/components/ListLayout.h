#pragma once

#include <cstdint>
#include <functional>

// Windowing for lists whose rows are NOT all the same height.
//
// The uniform lists compute everything from one number: page = selectedIndex / itemsPerPage, and
// every row sits at (index % itemsPerPage) * rowHeight. Once a row may be one, two or three lines
// tall that arithmetic no longer works — where a page starts depends on the content of the items
// before it, which for an SD-backed list (FileIndex) is not something the renderer can afford to
// walk from index 0 on every frame.
//
// So a wrapped list scrolls instead of paging: the caller owns an anchor (the index of the first
// visible row), computeWindow() fills downward from it and moves it the least amount needed to
// keep the selection on screen. Cost per frame is O(visible rows), independent of list length.
namespace ListLayout {

// Cap on rows drawn in one screenful. The shortest theme row is 30 px and the tallest content
// area is well under 600 px, so this cannot be reached in practice; it exists so a Window is a
// fixed 100-byte stack object instead of a per-frame heap allocation.
inline constexpr int kMaxRows = 24;

struct Window {
  int first = 0;               // index of the first drawn item
  int count = 0;               // number of items drawn
  int16_t top[kMaxRows]{};     // row top, relative to the list rect's y
  int16_t height[kMaxRows]{};  // row height in px
};

// Returns the rows to draw and updates `anchor` in place.
//
// `heightOf(index)` returns the pixel height of a row; it is called O(visible rows) times per
// call, so it is allowed to be moderately expensive (measuring wrapped text, reading a name from
// the SD index) but not free.
Window computeWindow(int itemCount, int selectedIndex, int availableHeight, int& anchor,
                     const std::function<int(int index)>& heightOf);

}  // namespace ListLayout
