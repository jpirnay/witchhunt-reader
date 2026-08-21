#include "ListLayout.h"

namespace ListLayout {
namespace {

// heightOf() reaches the SD index (one entry read per name) and measures wrapped text, so the same
// row must not be measured twice inside one call: the walks below deliberately revisit rows. A
// fixed 2 x kMaxRows table covers the rows one call can ask about, and a miss past that costs a
// re-measure, never a wrong answer. 288 bytes of the render task's 10 KB stack, which is why it is
// a plain array and not a container.
class HeightCache {
 public:
  explicit HeightCache(const std::function<int(int)>& heightOf) : heightOf_(heightOf) {}

  int operator()(const int index) {
    for (int i = 0; i < count_; i++) {
      if (index_[i] == index) return height_[i];
    }
    const int h = heightOf_(index);
    if (count_ < kCapacity) {
      index_[count_] = index;
      height_[count_] = static_cast<int16_t>(h);
      count_++;
    }
    return h;
  }

 private:
  static constexpr int kCapacity = kMaxRows * 2;
  const std::function<int(int)>& heightOf_;
  int index_[kCapacity] = {0};
  int16_t height_[kCapacity] = {0};
  int count_ = 0;
};

// Fills downward from `first` until the next row would cross the bottom edge (or the row cap).
Window fillFrom(const int first, const int itemCount, const int availableHeight, HeightCache& heightOf) {
  Window w;
  w.first = first;
  int y = 0;
  for (int i = first; i < itemCount && w.count < kMaxRows; i++) {
    const int h = heightOf(i);
    if (h <= 0) break;
    // Always place at least one row: an item taller than the rect is clipped rather than dropped,
    // otherwise the list would render empty and the selection could never be seen.
    if (y + h > availableHeight && w.count > 0) break;
    w.top[w.count] = static_cast<int16_t>(y);
    w.height[w.count] = static_cast<int16_t>(h);
    w.count++;
    y += h;
  }
  return w;
}

int usedHeight(const Window& w) {
  int used = 0;
  for (int i = 0; i < w.count; i++) used += w.height[i];
  return used;
}

// Walks up from `bottom`, taking rows while they fit, and returns the topmost one taken.
int highestVisibleAbove(const int bottom, const int availableHeight, HeightCache& heightOf) {
  int first = bottom;
  int used = heightOf(bottom);
  int rows = 1;
  while (first > 0 && rows < kMaxRows) {
    const int h = heightOf(first - 1);
    if (h <= 0 || used + h > availableHeight) break;
    first--;
    used += h;
    rows++;
  }
  return first;
}

}  // namespace

Window computeWindow(const int itemCount, const int selectedIndex, const int availableHeight, int& anchor,
                     const std::function<int(int index)>& heightOf) {
  if (itemCount <= 0 || availableHeight <= 0 || !heightOf) {
    anchor = 0;
    return {};
  }

  HeightCache height(heightOf);
  if (anchor < 0) anchor = 0;
  if (anchor > itemCount - 1) anchor = itemCount - 1;
  const bool hasSelection = selectedIndex >= 0 && selectedIndex < itemCount;

  // Selection above the window (moved up, or the list was rebuilt under us): the window follows it
  // directly, so scrolling up never skips rows.
  if (hasSelection && selectedIndex < anchor) anchor = selectedIndex;

  Window w = fillFrom(anchor, itemCount, availableHeight, height);

  if (hasSelection && selectedIndex >= anchor + w.count) {
    // Selection below the window. Re-seat it on the bottom row by filling UPWARDS from it rather
    // than advancing the anchor a row at a time: one keypress moves one row, but a long-press
    // jumps to the end of the list, and walking a 2000-entry folder row by row would measure
    // (and read from SD) every name in between. Filling upwards costs one screenful either way,
    // and for a single-row step it lands on exactly the window the row-by-row walk would produce.
    anchor = highestVisibleAbove(selectedIndex, availableHeight, height);
    w = fillFrom(anchor, itemCount, availableHeight, height);
  }

  // Take up any slack left below the last row — the end of the list, or a short list under a
  // stale anchor. A full window breaks out on the first test, so this normally costs one
  // (cached) height lookup.
  while (anchor > 0) {
    const int previousHeight = height(anchor - 1);
    if (previousHeight <= 0 || usedHeight(w) + previousHeight > availableHeight) break;
    const Window candidate = fillFrom(anchor - 1, itemCount, availableHeight, height);
    // Only take the step if the selection stays on screen: the row cap can make a taller window
    // hold fewer rows, and a half-applied pull-up would scroll the selection away.
    if (hasSelection && (selectedIndex < candidate.first || selectedIndex >= candidate.first + candidate.count)) break;
    anchor--;
    w = candidate;
  }

  return w;
}

}  // namespace ListLayout
