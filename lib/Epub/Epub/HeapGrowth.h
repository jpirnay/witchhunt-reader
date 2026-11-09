#pragma once
// The layout path's heap check for growing a std container: under -fno-exceptions a growth the heap refuses aborts, so
// ParsedText, ShapedStore and BidiLine ask the largest free block first and take their "without" path instead (laid
// out without bidi, a token drawn as its code points). One copy, shared by the three.
//
// The twin of lib/Memory/HeapFit.h for this path: that one reads heap_caps_* (the parser's tables), this one the
// Arduino shim's ESP.getMaxAllocHeap(), which the layout tests steer (ShapedLayoutTest's LargestFreeBlock).

#include <Arduino.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace heap_growth {

// The allocator's header and rounding on top of a request, as ParsedText::reserveWords allows for.
constexpr size_t kAllocSlack = 16;

// What growing `v` to `count` elements asks of the heap: nothing when its capacity holds them, else the block
// reserve(count) makes.
template <typename T>
size_t growthBytes(const std::vector<T>& v, const size_t count) {
  return v.capacity() >= count ? 0 : count * sizeof(T) + kAllocSlack;
}

// The same for a string grown to `size` bytes: libstdc++ asks for twice the capacity it had when that is more, and the
// NUL.
inline size_t growthBytes(const std::string& s, const size_t size) {
  return s.capacity() >= size ? 0 : std::max(size, 2 * s.capacity()) + 1 + kAllocSlack;
}

// Whether the largest free block holds `bytes`: the sum of several growths is checked at once, as reserveWords checks
// its five (each block then fits in what the ones before it leave). Nothing to grow is always allowed.
inline bool heapAllows(const size_t bytes) { return bytes == 0 || ESP.getMaxAllocHeap() >= bytes; }

// The free heap a shaped store's growth leaves (ShapedStore::heapAllows): 3 KB above the parse's hard floor
// (EHP_TEXT_LAYOUT_HARD_MIN_FREE_HEAP, which ChapterHtmlSlimParser asserts is below it). Short of it, the store
// refuses, the token is laid out as its code points and the section is latched as shaping degraded (rebuilt once),
// so a heavy shaped paragraph ends in unjoined text instead of an aborted, truncated section. Measured on the X3 and
// X4 (2026-11-09), no book came near it: the stress chapter's lowest free heap was about 19.8 and 33 KB.
constexpr size_t kShapedFreeFloor = 12 * 1024;

// Whether growing by `bytes` leaves `floor` of free heap.
inline bool heapLeaves(const size_t bytes, const size_t floor) { return ESP.getFreeHeap() >= bytes + floor; }

}  // namespace heap_growth
