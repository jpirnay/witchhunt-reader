#pragma once

// A spine item that holds only its chapter's heading has no page of its own (HeadingCarry): the next item's first page
// shows it. Where the reader lands for a position on such an item, and which item it enters after the one on screen.
namespace CarriedSpineNav {

struct Landing {
  int spine = 0;
  bool lastPage = false;  // the item's last page; else its first
};

// A turn back onto the carried item (a last-page target) goes on to the item before it, whose last page came before
// the heading; there is none before the first item, which lands where every other target does: on the next item's
// first page, the heading's. Two carried items never follow each other (HeadingCarry's fourth rule).
constexpr Landing land(const int carriedSpine, const bool lastPageTarget) {
  return lastPageTarget && carriedSpine > 0 ? Landing{carriedSpine - 1, true} : Landing{carriedSpine + 1, false};
}

// The item the reader enters after `current`: the next one, or the one after that when the next is carried.
constexpr int nextReadingSpine(const int current, const bool nextCarried) {
  return current + 1 + (nextCarried ? 1 : 0);
}

}  // namespace CarriedSpineNav
