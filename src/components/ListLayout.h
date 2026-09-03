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

// Cap on rows drawn in one screenful. It exists so a Window is a fixed ~132-byte stack object
// instead of a per-frame heap allocation, which is why this number is kept tight rather than
// generous: unlike ListTouchBand's capacity it is paid on the render task's stack, twice over
// (the Window plus ListLayout.cpp's height cache).
//
// The bound is the shortest theme row (30 px) into the tallest content area: the LilyGo T5S3's
// 540x960 portrait frame less the 40 px button-hint strip is 920 px, so 30 rows. 24 was sized
// for the 800 px panels, where the same sum is 22, and a wrapped list on the T5S3 would stop
// six rows short of the foot of the screen.
//
// Must not exceed ListTouchBand::kMaxRows, or a wrapped list would paint rows the band cannot
// record and they would answer no tap. ListTouchBandTest asserts that direction.
inline constexpr int kMaxRows = 32;

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
